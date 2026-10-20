extern "C" {
#include "esp_mem.h"
}
#include "OtaTransport.h"

#include <atomic>
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "lwip/dns.h"
#include "lwip/tcpip.h"

namespace marauder { namespace ota {
namespace {
// The pinned SDK defaults mbedTLS to scarce internal RAM. Use the Mini's
// PSRAM for this task's TLS allocations while a stream exists. WPA and other
// system tasks retain the SDK allocator; both allocation types use heap_caps_free.
std::atomic<TaskHandle_t> tlsTask{nullptr};
unsigned tlsUsers = 0;
void* tlsCalloc(size_t count, size_t bytes) {
  if (xTaskGetCurrentTaskHandle() == tlsTask.load())
    return heap_caps_calloc(count, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return esp_mbedtls_mem_calloc(count, bytes);
}
void beginTlsMemory() {
  if (!tlsUsers++) {
    tlsTask = xTaskGetCurrentTaskHandle();
    mbedtls_platform_set_calloc_free(tlsCalloc, esp_mbedtls_mem_free);
  }
}
void endTlsMemory() {
  if (!--tlsUsers) {
    mbedtls_platform_set_calloc_free(esp_mbedtls_mem_calloc, esp_mbedtls_mem_free);
    tlsTask = nullptr;
  }
}
struct Resolver {
  const char* name;
  std::atomic<unsigned> state{0}; // 0 unused, 1 pending, 2 resolved, 3 failed
  std::atomic<uint32_t> address{0};
};
// DNS callbacks may arrive after a cancelled request. Their storage remains
// alive, and all lwIP DNS calls execute on the TCP/IP thread.
Resolver resolvers[] = {{"api.github.com"}, {"github.com"},
                        {"release-assets.githubusercontent.com"}, {"objects.githubusercontent.com"}};
void resolved(const char*, const ip_addr_t* address, void* argument) {
  auto& slot = *static_cast<Resolver*>(argument);
  if (address && IP_IS_V4(address)) {
    slot.address = ip4_addr_get_u32(ip_2_ip4(address)); slot.state = 2;
  } else slot.state = 3;
}
void resolveOnTcpThread(void* argument) {
  auto& slot = *static_cast<Resolver*>(argument); ip_addr_t address;
  const err_t result = dns_gethostbyname_addrtype(slot.name, &address, resolved, &slot, LWIP_DNS_ADDRTYPE_IPV4);
  if (result == ERR_OK) resolved(slot.name, &address, &slot);
  else if (result != ERR_INPROGRESS) slot.state = 3;
}
bool resolve(const std::string& host, IPAddress& address, uint32_t start, uint32_t budget, Pump pump, Cancelled cancelled) {
  Resolver* slot = nullptr;
  for (auto& entry : resolvers) if (host == entry.name) slot = &entry;
  if (!slot) return false;
  if (slot->state != 1) {
    slot->state = 1;
    if (tcpip_callback(resolveOnTcpThread, slot) != ERR_OK) { slot->state = 3; return false; }
  }
  while (slot->state == 1 && remaining(start, millis(), budget) && !cancelled()) { pump(); delay(1); }
  if (slot->state != 2 || cancelled() || !remaining(start, millis(), budget)) return false;
  address = IPAddress(slot->address.load()); return true;
}
}

bool HttpStream::Client::open(IPAddress ip, const char* host, uint32_t start, uint32_t budget) {
  stage = "TCP"; error = 0;
  uint32_t timeLeft = remaining(start, millis(), budget);
  if (!timeLeft) return false;
  _use_ca_bundle = true;
  attach_ssl_certificate_bundle(sslclient.get(), true); // SDK's built-in trusted root bundle.
  _timeout = timeLeft;
  setPlainStart(); // Configure TLS and open TCP; send no application bytes yet.
  if (!NetworkClientSecure::connect(ip, 443, host, nullptr, nullptr, nullptr)) { error = sslclient->last_error; return false; }
  timeLeft = remaining(start, millis(), budget);
  if (!timeLeft) { stop(); return false; }
  sslclient->handshake_timeout = timeLeft;
  stage = "TLS";
  // The pinned core's startTLS() discards the handshake error when it stops
  // the socket. Preserve it for credential-free update diagnostics.
  error = ssl_starttls_handshake(sslclient.get());
  if (error >= 0 && mbedtls_ssl_get_verify_result(&sslclient->ssl_ctx)) error = MBEDTLS_ERR_X509_CERT_VERIFY_FAILED;
  sslclient->last_error = error;
  if (error < 0 || !remaining(start, millis(), budget)) { stop(); return false; }
  _stillinPlainStart = false;
  _timeout = 100;
  sslclient->socket_timeout = 100;
  return true;
}
HttpStream::HttpStream(Pump pump, Cancelled cancelled) : pump_(pump), cancelled_(cancelled) {
  beginTlsMemory();
  input_ = static_cast<uint8_t*>(heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}
HttpStream::~HttpStream() { client_.stop(); free(input_); endTlsMemory(); }
uint32_t HttpStream::left() const {
  if (cancelled_()) return 0;
  return std::min(remaining(start_, millis(), budget_), remaining(lastByte_, millis(), kServerTimeoutMs));
}
bool HttpStream::fail(const char* message) { error_ = message; failed_ = true; client_.stop(); return false; }
int HttpStream::raw() {
  if (fixture_) return fixture_[fixtureAt_] ? uint8_t(fixture_[fixtureAt_++]) : -1;
  if (inputAt_ < inputSize_) {
    if (!left()) { fail("Failed to connect to update server"); return -1; }
    return input_[inputAt_++];
  }
  while (left()) {
    int count = client_.available();
    if (count > 0) {
      count = client_.read(input_, std::min(count, 4096));
      if (count > 0) { inputAt_ = 1; inputSize_ = count; lastByte_ = millis(); return input_[0]; }
    }
    if (!client_.connected()) return -1;
    pump_(); delay(1);
  }
  fail(cancelled_() ? "Update cancelled" : "Failed to connect to update server"); return -1;
}
bool HttpStream::line(String& result, size_t maximum) {
  result = "";
  while (true) {
    int value = raw(); if (value < 0) return false;
    if (value == '\n') { if (result.endsWith("\r")) result.remove(result.length() - 1); return true; }
    if (result.length() >= maximum || value == 0) return false;
    result += char(value);
  }
}
bool HttpStream::headers(int& status, std::string& location) {
  String value;
  if (!line(value, 128) || (!value.startsWith("HTTP/1.1 ") && !value.startsWith("HTTP/1.0 "))) return false;
  status = value.substring(9, 12).toInt();
  size_t total = value.length(); bool hasLength = false;
  while (line(value, 4096)) {
    total += value.length() + 2; if (total > 16384) return false;
    if (!value.length()) return !(hasLength && chunked_);
    const int colon = value.indexOf(':'); if (colon <= 0) return false;
    String name = value.substring(0, colon), field = value.substring(colon + 1);
    name.toLowerCase(); field.trim();
    if (name == "location") { if (field.length() > 2048 || !location.empty()) return false; location = field.c_str(); }
    else if (name == "content-length") {
      if (!field.length()) return false;
      uint64_t number = 0;
      for (char c : field) { if (c < '0' || c > '9' || number > limit_) return false; number = number * 10 + unsigned(c - '0'); }
      if (number > limit_ || (hasLength && int64_t(number) != length_)) return false;
      hasLength = true; length_ = number;
    } else if (name == "transfer-encoding") {
      field.toLowerCase(); if (field != "chunked" || chunked_) return false; chunked_ = true;
    } else if (name == "content-encoding" && field != "identity") return false;
  }
  return false;
}
bool HttpStream::get(const std::string& initial, uint32_t budgetMs, size_t limit) {
  start_ = lastByte_ = millis(); budget_ = budgetMs; limit_ = limit;
  if (!input_ || !allowedUrl(initial)) return fail("Invalid update URL");
  std::string url = initial;
  for (unsigned hop = 0; hop < 4; ++hop) {
    if (!allowedUrl(url, hop > 0)) return fail("Blocked update redirect");
    Url target; if (!splitUrl(url, target)) return fail("Invalid update URL");
    IPAddress address;
    stage_ = "DNS";
    if (!resolve(target.host, address, start_, budget_, pump_, cancelled_)) return fail("Failed to connect to update server");
    if (!client_.open(address, target.host.c_str(), start_, budget_)) {
      stage_ = client_.stage; transportError_ = client_.error;
      return fail("Failed to connect to update server");
    }
    lastByte_ = millis();
    stage_ = "Request";
    const std::string request = "GET " + target.path + " HTTP/1.1\r\nHost: " + target.host +
        "\r\nUser-Agent: MarauderEternal-MiniV3-OTA\r\nAccept: application/vnd.github+json, application/octet-stream\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n";
    if (client_.write(reinterpret_cast<const uint8_t*>(request.data()), request.size()) != request.size()) return fail("Failed to connect to update server");
    inputAt_ = inputSize_ = 0; length_ = -1; chunked_ = false;
    int status = 0; std::string location;
    stage_ = "Headers";
    if (!headers(status, location)) return fail("Invalid update server response");
    status_ = status;
    if (status == 200) { stage_ = "Body"; return true; }
    client_.stop();
    if ((status == 301 || status == 302 || status == 303 || status == 307 || status == 308) && !location.empty() && initial != kApi) {
      url = location; continue;
    }
    return fail(status == 403 || status == 429 ? "Update server rate limit; try later" : "Failed to connect to update server");
  }
  return fail("Too many update redirects");
}
int HttpStream::read() {
  if (peeked_ >= 0) { const int result = peeked_; peeked_ = -1; return result; }
  if (done_ || failed_) return -1;
  if (chunked_) {
    if (chunkEnd_) {
      if (raw() != '\r' || raw() != '\n') { fail("Invalid update transfer"); return -1; }
      chunkEnd_ = false;
    }
    if (!chunkLeft_) {
      String lineValue;
      if (!line(lineValue, 80)) { fail("Incomplete update transfer"); return -1; }
      const int semicolon = lineValue.indexOf(';'); if (semicolon >= 0) lineValue.remove(semicolon);
      if (!lineValue.length() || lineValue.length() > 8) { fail("Invalid update transfer"); return -1; }
      uint64_t size = 0;
      for (char c : lineValue) {
        const unsigned digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 16;
        if (digit >= 16) { fail("Invalid update transfer"); return -1; }
        size = size * 16 + digit;
      }
      if (size > limit_ - consumed_) { fail("Update response too large"); return -1; }
      chunkLeft_ = size;
      if (!chunkLeft_) {
        // A zero chunk still requires the terminating trailer section. Reject
        // truncated responses rather than treating an unfinished transfer as complete.
        size_t trailers = 0;
        while (line(lineValue, 4096)) {
          trailers += lineValue.length() + 2;
          if (trailers > 8192) break;
          if (!lineValue.length()) { done_ = true; return -1; }
          if (lineValue.indexOf(':') <= 0) break;
        }
        fail("Incomplete update transfer"); return -1;
      }
    }
  } else if (length_ >= 0 && consumed_ == size_t(length_)) { done_ = true; return -1; }
  const int value = raw();
  if (value < 0) {
    if (!failed_ && length_ < 0 && !chunked_) done_ = true;
    else fail("Incomplete update transfer");
    return -1;
  }
  if (consumed_ >= limit_) { fail("Update response too large"); return -1; }
  ++consumed_;
  if (chunked_ && --chunkLeft_ == 0) chunkEnd_ = true;
  return value;
}
int HttpStream::peek() { if (peeked_ < 0) peeked_ = read(); return peeked_; }
int HttpStream::available() { return done_ || failed_ ? 0 : 1; }
size_t HttpStream::readBytes(char* output, size_t count) {
  size_t readCount = 0;
  while (readCount < count) { int value = read(); if (value < 0) break; output[readCount++] = value; }
  return readCount;
}
bool HttpStream::finishJson() {
  int value;
  while ((value = read()) >= 0) if (value != ' ' && value != '\t' && value != '\r' && value != '\n') return fail("Invalid update JSON");
  return complete();
}
bool checkTransportFixtures() {
  bool allocationOk;
  {
    HttpStream scope([]() {}, []() { return false; });
    void* memory = mbedtls_calloc(1, 32768);
    allocationOk = memory && esp_ptr_external_ram(memory);
    mbedtls_free(memory);
  }
  void* normalMemory = mbedtls_calloc(1, 64);
  allocationOk &= normalMemory && esp_ptr_internal(normalMemory);
  mbedtls_free(normalMemory);
  const auto decode = [](const char* wire, const char* expected, bool valid) {
    HttpStream stream([]() {}, []() { return false; });
    stream.fixture_ = wire; stream.limit_ = 64;
    stream.start_ = stream.lastByte_ = millis(); stream.budget_ = 1000;
    int status = 0; std::string location, text;
    bool ok = stream.headers(status, location) && status == 200;
    if (ok) { int value; while ((value = stream.read()) >= 0) text += char(value); ok = stream.complete(); }
    return ok == valid && (!valid || text == expected);
  };
  bool ok = allocationOk && decode("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nabcde", "abcde", true);
  ok &= decode("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n2;ext=1\r\nde\r\n0\r\n\r\n", "abcde", true);
  ok &= decode("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", "", true);
  ok &= decode("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nabc", "", false);
  ok &= decode("HTTP/1.1 200 OK\r\nContent-Length: 65\r\n\r\n", "", false);
  ok &= decode("HTTP/1.1 200 OK\r\nContent-Length: 3\r\nContent-Length: 4\r\n\r\nabc", "", false);
  ok &= decode("HTTP/1.1 200 OK\r\nContent-Length: 3\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n", "", false);
  ok &= decode("HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\n\r\nabc", "", false);
  ok &= decode("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nabX\n0\r\n", "", false);
  ok &= decode("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nZ\r\nab\r\n0\r\n", "", false);
  ok &= decode("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nab\r\n", "", false);
  ok &= decode("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n", "", false);
  ok &= decode("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\nX-Check: done\r\n\r\n", "", true);
  return ok;
}
} }

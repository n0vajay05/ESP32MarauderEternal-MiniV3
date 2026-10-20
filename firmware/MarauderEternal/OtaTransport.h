#pragma once

#include <Arduino.h>
#include <NetworkClientSecure.h>
#include "OtaPolicy.h"

namespace marauder { namespace ota {
bool checkTransportFixtures();
using Pump = void (*)();
using Cancelled = bool (*)();

// GET-only, certificate-verified HTTPS, with a deadline shared by DNS, TCP,
// TLS, headers, redirects and body. Redirects never escape this repository.
class HttpStream : public Stream {
 public:
  HttpStream(Pump pump, Cancelled cancelled);
  ~HttpStream();
  HttpStream(const HttpStream&) = delete;
  HttpStream& operator=(const HttpStream&) = delete;
  bool get(const std::string& url, uint32_t budgetMs, size_t limit);
  int available() override;
  int read() override;
  int peek() override;
  void flush() override {}
  size_t write(uint8_t) override { return 0; }
  size_t readBytes(char* output, size_t count);
  bool finishJson();
  bool complete() const { return done_ && !failed_; }
  int64_t contentLength() const { return length_; }
  const char* error() const { return error_; }
  const char* stage() const { return stage_; }
  int transportError() const { return transportError_; }
  int statusCode() const { return status_; }

 private:
  friend bool checkTransportFixtures();
  const char* fixture_ = nullptr; // Only populated by the isolated parser checks.
  size_t fixtureAt_ = 0;
  class Client : public NetworkClientSecure {
   public:
    bool open(IPAddress ip, const char* host, uint32_t start, uint32_t budget);
    const char* stage = "TCP";
    int error = 0;
  } client_;
  Pump pump_; Cancelled cancelled_;
  uint32_t start_ = 0, budget_ = 0, lastByte_ = 0;
  size_t limit_ = 0, consumed_ = 0, chunkLeft_ = 0;
  int64_t length_ = -1;
  bool chunked_ = false, chunkEnd_ = false, done_ = false, failed_ = false;
  int peeked_ = -1;
  uint8_t* input_ = nullptr;
  size_t inputAt_ = 0, inputSize_ = 0;
  const char* error_ = "Failed to connect to update server";
  const char* stage_ = "Idle";
  int transportError_ = 0, status_ = 0;
  uint32_t left() const;
  int raw();
  bool line(String& result, size_t maximum);
  bool headers(int& status, std::string& location);
  bool fail(const char* error);
};
} }

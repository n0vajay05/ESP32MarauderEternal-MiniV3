#include <algorithm>
#include <assert.h>
#include <limits>
#include <stdint.h>
#include <vector>

#include "BLEClientLifecycle.h"
#include "IPv4Range.h"
#include "MemoryGuard.h"
#include "UploadStreamBuffer.h"

static constexpr uint32_t ip(unsigned a, unsigned b, unsigned c, unsigned d) {
  return (a << 24) | (b << 16) | (c << 8) | d;
}

static void testMemoryPressureRecovery() {
  marauder::MemoryGuardState guard;
  assert(guard.allow(100 * 1024, 50 * 1024, 1024));
  // Lots of total free RAM cannot make up for a fragmented internal heap.
  assert(!guard.allow(100 * 1024, 9 * 1024, 2 * 1024));
  assert(guard.paused());
  assert(!guard.allow(39 * 1024, 20 * 1024));
  assert(!guard.allow(50 * 1024, 11 * 1024));
  assert(guard.allow(40 * 1024, 12 * 1024));
  assert(!guard.allow(25 * 1024, 20 * 1024, 2 * 1024));
  assert(guard.allow(100 * 1024, 50 * 1024));
  // An oversized reservation must not wrap around and bypass the guard.
  assert(!guard.allow(100 * 1024, 50 * 1024,
                      std::numeric_limits<size_t>::max()));

  marauder::QueueBackpressureState queue(20);
  for (size_t count = 0; count < 20; ++count) assert(queue.allow(count));
  assert(!queue.allow(20));
  for (size_t count = 20; count > 10; --count) assert(!queue.allow(count));
  assert(queue.allow(10));
  assert(queue.allow(0)); // a new session recovers after its old queue drains
}

static void assertSubnet(uint32_t local, uint32_t mask, size_t expected) {
  marauder::IPv4HostCursor cursor;
  assert(cursor.next() == 0);
  cursor.reset(local, mask);
  const auto range = marauder::ipv4HostRange(local, mask);
  size_t count = 0;
  uint32_t previous = 0;
  while (const uint32_t current = cursor.next()) {
    assert(current >= range.first && current <= range.last);
    assert(current != local && current > previous);
    previous = current;
    ++count;
    assert(count <= expected);
  }
  assert(count == expected);
  assert(cursor.next() == 0 && cursor.next() == 0);
}

static void testSubnetDiscovery() {
  // Gateway position never determines scan coverage. Test local IPs at the
  // first/last host and inside a batch, plus crossing an octet boundary.
  for (unsigned host : {1, 2, 10, 128, 254})
    assertSubnet(ip(192,168,1,host), ip(255,255,255,0), 253);
  assertSubnet(ip(192,168,1,254), ip(255,255,254,0), 509);
  assertSubnet(ip(10,0,0,1), ip(255,255,0,0), 65533);
  assertSubnet(ip(255,255,255,1), ip(255,255,255,0), 253);
  assertSubnet(ip(192,168,1,1), ip(255,255,255,252), 1);
  for (uint32_t mask : {ip(255,255,255,254), ip(255,255,255,255),
                        ip(255,0,255,0)}) {
    marauder::IPv4HostCursor cursor;
    cursor.reset(ip(192,168,1,1), mask);
    assert(cursor.next() == 0);
  }
  const auto range = marauder::ipv4HostRange(ip(192,168,1,254), ip(255,255,255,0));
  assert(marauder::previousIPv4Host(range.last, 9, range) == ip(192,168,1,245));
  assert(marauder::previousIPv4Host(0, 1, range) == 0);
  assert(marauder::previousIPv4Host(range.first, 1, range) == 0);
}

struct TestSource {
  std::vector<uint8_t> bytes;
  size_t position = 0;
  size_t read(uint8_t* out, size_t wanted) {
    const size_t count = std::min({wanted, bytes.size() - position, size_t(37)});
    std::copy_n(bytes.data() + position, count, out);
    position += count;
    return count;
  }
};

struct TestSink {
  std::vector<uint8_t> bytes;
  size_t failAfter = std::numeric_limits<size_t>::max();
  size_t write(const uint8_t* in, size_t wanted) {
    const size_t count = std::min({wanted, size_t(7), failAfter - bytes.size()});
    bytes.insert(bytes.end(), in, in + count);
    return count;
  }
};

static void testUploadFailures() {
  marauder::UploadStreamBuffer buffer;
  assert(buffer && buffer.size() == 1024);
  TestSource source;
  for (size_t i = 0; i < 5177; ++i) source.bytes.push_back(i % 251);
  TestSink sink;
  size_t progress = 0;
  auto update = [&](size_t sent, size_t total) {
    assert(sent > progress && sent <= total && sent == sink.bytes.size());
    progress = sent;
  };
  assert(marauder::streamUpload(source, sink, buffer.data(), buffer.size(),
                                source.bytes.size(), update));
  assert(sink.bytes == source.bytes && progress == source.bytes.size());

  source.position = 0;
  sink.bytes.clear();
  sink.failAfter = 31;
  progress = 0;
  assert(!marauder::streamUpload(source, sink, buffer.data(), buffer.size(),
                                 source.bytes.size(), update));
  assert(progress == 31 && sink.bytes.size() == 31);

  source.position = 0;
  sink = TestSink{};
  progress = 0;
  assert(!marauder::streamUpload(source, sink, buffer.data(), buffer.size(),
                                 source.bytes.size() + 1, update));
  assert(progress == source.bytes.size()); // truncated SD file: no false success
  auto unused = [](size_t, size_t) { assert(false); };
  assert(!marauder::streamUpload(source, sink, nullptr, 1024, 1, unused));
  assert(!marauder::streamUpload(source, sink, buffer.data(), 0, 1, unused));
  assert(marauder::streamUpload(source, sink, buffer.data(), 1024, 0, unused));
}

struct TestBleClient {
  bool connected = true;
  bool autoDelete = true;
  unsigned disconnects = 0;
  void setSelfDelete(bool onDisconnect, bool onFailure) {
    assert(!onDisconnect && !onFailure);
    autoDelete = false;
  }
  bool isConnected() const { return connected; }
  void disconnect() { assert(!autoDelete); ++disconnects; }
};

static void testBleDisconnectLifecycle() {
  TestBleClient storage;
  TestBleClient* client = &storage;
  uint32_t now = UINT32_MAX - 15;
  unsigned waits = 0;
  unsigned deletions = 0;
  auto clock = [&]() { return now; };
  auto wait = [&]() {
    now += 10;
    if (++waits == 3) storage.connected = false;
  };
  auto destroy = [&](TestBleClient* owned) {
    assert(owned == &storage && !owned->isConnected());
    ++deletions;
    return true;
  };
  assert(marauder::releaseDisconnectedClient(client, clock, wait, destroy));
  assert(client == nullptr && waits == 3 && deletions == 1);
  assert(marauder::releaseDisconnectedClient(client, clock, wait, destroy));
  assert(deletions == 1); // no double deletion after a failed/repeated session

  storage = TestBleClient{};
  client = &storage;
  waits = 0;
  auto stalled = [&]() { now += 10; ++waits; };
  assert(!marauder::releaseDisconnectedClient(client, clock, stalled, destroy, 50));
  assert(client == &storage && waits == 5 && deletions == 1);
  // Late disconnect: retain the slot and retry before shutting down the host.
  storage.connected = false;
  auto rejectDeletion = [](TestBleClient*) { return false; };
  assert(!marauder::releaseDisconnectedClient(client, clock, wait, rejectDeletion));
  assert(client == &storage);
  assert(marauder::releaseDisconnectedClient(client, clock, wait, destroy));
  assert(client == nullptr && deletions == 2);
}

int main() {
  testMemoryPressureRecovery();
  testSubnetDiscovery();
  testUploadFailures();
  testBleDisconnectLifecycle();
}

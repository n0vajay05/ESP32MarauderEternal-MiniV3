#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

namespace marauder {

// Keep TLS uploads off the ESP32 loop task's small stack.
class UploadStreamBuffer {
 public:
  UploadStreamBuffer() : data_(static_cast<uint8_t*>(malloc(1024))) {}
  ~UploadStreamBuffer() { free(data_); }
  UploadStreamBuffer(const UploadStreamBuffer&) = delete;
  UploadStreamBuffer& operator=(const UploadStreamBuffer&) = delete;
  uint8_t* data() const { return data_; }
  size_t size() const { return 1024; }
  explicit operator bool() const { return data_ != nullptr; }

 private:
  uint8_t* data_;
};

// Transfer the announced file size exactly. A short socket write is retried;
// zero progress or an early SD read failure must never be reported as success.
template <typename Source, typename Sink, typename Progress>
bool streamUpload(Source& source, Sink& sink, uint8_t* buffer,
                  size_t capacity, size_t expectedBytes, Progress progress) {
  if (buffer == nullptr || capacity == 0) return false;
  size_t sent = 0;
  while (sent < expectedBytes) {
    const size_t remaining = expectedBytes - sent;
    const size_t wanted = remaining < capacity ? remaining : capacity;
    const size_t received = source.read(buffer, wanted);
    if (received == 0 || received > wanted) return false;
    size_t offset = 0;
    while (offset < received) {
      const size_t written = sink.write(buffer + offset, received - offset);
      if (written == 0 || written > received - offset) return false;
      offset += written;
      sent += written;
      progress(sent, expectedBytes);
    }
  }
  return true;
}

}  // namespace marauder

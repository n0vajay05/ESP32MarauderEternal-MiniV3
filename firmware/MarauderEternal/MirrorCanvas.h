#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace marauder { namespace mirror {
constexpr unsigned kWidth = 128, kHeight = 128;
constexpr size_t kBytes = kWidth * kHeight * sizeof(uint16_t);
constexpr size_t kPacketMax = 278;

inline uint32_t crc32(const uint8_t* bytes, size_t length) {
  uint32_t crc = ~uint32_t(0);
  for (size_t i = 0; i < length; ++i) {
    crc ^= bytes[i];
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1)));
  }
  return ~crc;
}
inline void put32(uint8_t* out, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) out[i] = value >> (8 * i);
}

class Canvas {
 public:
  void attach(uint16_t* storage) {
    data_ = storage;
    if (data_) memset(data_, 0, kBytes);
    invalidate();
  }
  bool ready() const { return data_ != nullptr; }
  void invalidate() { memset(dirty_, 1, sizeof(dirty_)); }
  void rotation(uint8_t value) { rotation_ = value & 3; }
  void window(int32_t x, int32_t y, int32_t xe, int32_t ye) {
    x_ = x; y_ = y; width_ = xe >= x ? uint32_t(xe - x) + 1 : 0;
    height_ = ye >= y ? uint32_t(ye - y) + 1 : 0; cursor_ = 0;
  }
  void pixel(int32_t x, int32_t y, uint16_t color) {
    if (!data_ || x < 0 || y < 0 || x >= int32_t(kWidth) || y >= int32_t(kHeight)) return;
    int32_t px = x, py = y;
    if (rotation_ == 1) { px = kWidth - 1 - y; py = x; }
    else if (rotation_ == 2) { px = kWidth - 1 - x; py = kHeight - 1 - y; }
    else if (rotation_ == 3) { px = y; py = kHeight - 1 - x; }
    uint16_t& old = data_[py * kWidth + px];
    if (old != color) { old = color; dirty_[py] = true; }
  }
  void solid(uint16_t color, uint32_t count) {
    if (!data_ || !width_ || !height_) return;
    // Invalid raw windows must never cause unbounded work or overwrite RAM.
    if (width_ > kWidth || height_ > kHeight) return;
    if (simpleWindow()) {
      while (count) {
        uint32_t length; unsigned y;
        uint16_t* output = span(count, length, y);
        bool changed = dirty_[y];
        for (uint32_t i = 0; i < length; ++i) {
          if (output[i] != color) { output[i] = color; changed = true; }
        }
        dirty_[y] = changed; count -= length;
      }
      return;
    }
    while (count--) { streamPixel(color); }
  }
  void pixels(const void* input, uint32_t count, bool swap) {
    if (!data_ || !input || !width_ || !height_ || width_ > kWidth || height_ > kHeight) return;
    const auto* bytes = static_cast<const uint8_t*>(input);
    if (simpleWindow()) {
      while (count) {
        uint32_t length; unsigned y;
        uint16_t* output = span(count, length, y);
        if (swap) {
          // Native RGB565 can be compared/copied as a contiguous row. The
          // general rotated/clipped path below still handles individual pixels.
          if (dirty_[y] || memcmp(output, bytes, length * 2)) {
            memcpy(output, bytes, length * 2); dirty_[y] = true;
          }
        } else {
          bool changed = dirty_[y];
          for (uint32_t i = 0; i < length; ++i) {
            uint16_t color = uint16_t((bytes[i * 2] << 8) | bytes[i * 2 + 1]);
            if (output[i] != color) { output[i] = color; changed = true; }
          }
          dirty_[y] = changed;
        }
        bytes += length * 2; count -= length;
      }
      return;
    }
    for (uint32_t i = 0; i < count; ++i) {
      // TFT_eSPI's swap=true input is native uint16_t RGB565. swap=false
      // input already contains the big-endian byte order sent over SPI.
      uint16_t color = swap ? uint16_t(bytes[0] | (bytes[1] << 8)) : uint16_t((bytes[0] << 8) | bytes[1]);
      streamPixel(color); bytes += 2;
    }
  }
  // Round-robin rows keep a frequently changing status bar from starving the
  // rest of the image. New writes to an in-flight row mark it dirty again.
  int takeRow() {
    if (!data_) return -1;
    for (unsigned n = 0; n < kHeight; ++n) {
      unsigned row = next_; next_ = (next_ + 1) % kHeight;
      if (dirty_[row]) { dirty_[row] = false; return row; }
    }
    return -1;
  }
  const uint16_t* row(unsigned y) const { return data_ + y * kWidth; }
 private:
  bool simpleWindow() const {
    return rotation_ == 0 && x_ >= 0 && y_ >= 0 &&
        uint32_t(x_) <= kWidth - width_ && uint32_t(y_) <= kHeight - height_;
  }
  uint16_t* span(uint32_t count, uint32_t& length, unsigned& row) {
    uint32_t offset = cursor_ % width_;
    row = y_ + cursor_ / width_;
    uint16_t* output = data_ + row * kWidth + x_ + offset;
    length = count < width_ - offset ? count : width_ - offset;
    cursor_ += length;
    if (cursor_ == width_ * height_) cursor_ = 0;
    return output;
  }
  void streamPixel(uint16_t color) {
    pixel(x_ + cursor_ % width_, y_ + cursor_ / width_, color);
    if (++cursor_ >= width_ * height_) cursor_ = 0;
  }
  uint16_t* data_ = nullptr;
  bool dirty_[kHeight]{};
  unsigned next_ = 0;
  uint8_t rotation_ = 0;
  int32_t x_ = 0, y_ = 0;
  uint32_t width_ = 0, height_ = 0, cursor_ = 0;
};

// A packet is one row or an idle response. Compression is used only when it
// reduces size; the maximum packet is always 278 bytes, without a frame queue.
inline size_t packet(uint8_t* out, const Canvas& canvas, int row,
                     uint32_t session, uint32_t sequence) {
  memcpy(out, "\xa5MRR", 4); out[4] = 1; out[5] = row >= 0 ? 1 : 0;
  put32(out + 6, sequence); put32(out + 10, session);
  out[14] = row >= 0 ? row : 0; out[15] = 0;
  size_t length = 0;
  if (row >= 0) {
    const auto* pixels = canvas.row(row);
    for (unsigned x = 0; x < kWidth;) {
      unsigned end = x + 1;
      while (end < kWidth && pixels[end] == pixels[x]) ++end;
      if (length + 3 >= kWidth * 2) { length = kWidth * 2; break; }
      out[18 + length++] = end - x;
      out[18 + length++] = pixels[x] >> 8;
      out[18 + length++] = pixels[x]; x = end;
    }
    if (length < kWidth * 2) out[15] = 1;
    else for (unsigned x = 0; x < kWidth; ++x) {
      out[18 + x * 2] = pixels[x] >> 8; out[19 + x * 2] = pixels[x];
    }
  }
  out[16] = length; out[17] = length >> 8;
  put32(out + 18 + length, crc32(out, 18 + length));
  return 22 + length;
}
} }

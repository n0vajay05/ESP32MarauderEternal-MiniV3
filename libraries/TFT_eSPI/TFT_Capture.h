#pragma once
#include <stdint.h>

// Optional observer of pixels actually written to the panel. Sprites keep their
// own null observer; pushing a sprite is captured by the destination display.
class TFT_Capture {
 public:
  virtual void window(int32_t x, int32_t y, int32_t xe, int32_t ye) = 0;
  virtual void solid(uint16_t color, uint32_t count) = 0;
  virtual void pixels(const void* data, uint32_t count, bool swap) = 0;
  virtual void pixel(int32_t x, int32_t y, uint16_t color) = 0;
  virtual void rotated(uint8_t rotation) = 0;
};

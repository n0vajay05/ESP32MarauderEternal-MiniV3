#pragma once

#include <TFT_eSPI.h>

// A game draws into this 16-bit canvas, then exposes the completed frame to
// the SPI display in one contiguous transfer. Only one game exists at a time,
// so the 32 KiB 128x128 allocation is released when its run() function exits.
class GameFrameBuffer {
 public:
  explicit GameFrameBuffer(TFT_eSPI& display) : sprite_(&display) {}

  ~GameFrameBuffer() {
    end();
  }

  bool begin(int16_t width, int16_t height) {
    end();
    sprite_.setColorDepth(16);
    ready_ = sprite_.createSprite(width, height) != nullptr;
    if (ready_) {
      // TFT_eSPI does not initialize its inherited GFX free-font pointer in
      // every sprite construction path. Select the built-in font explicitly
      // before any game sends text through Print::write().
      sprite_.setTextFont(1);
      sprite_.setTextWrap(false);
      sprite_.fillSprite(TFT_BLACK);
    }
    return ready_;
  }

  void end() {
    if (ready_) {
      sprite_.deleteSprite();
      ready_ = false;
    }
  }

  TFT_eSprite& canvas() {
    return sprite_;
  }

  void present() {
    if (ready_)
      sprite_.pushSprite(0, 0);
  }

 private:
  TFT_eSprite sprite_;
  bool ready_ = false;
};

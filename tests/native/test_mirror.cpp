#include "MirrorCanvas.h"
#include <array>
#include <cassert>
#include <iostream>

int main() {
  using namespace marauder::mirror;
  std::array<uint16_t, kWidth * kHeight + 2> storage{};
  storage.front() = storage.back() = 0xbeef;
  Canvas canvas; canvas.attach(storage.data() + 1);
  assert(crc32(reinterpret_cast<const uint8_t*>("123456789"), 9) == 0xcbf43926);
  for (unsigned row = 0; row < kHeight; ++row) assert(canvas.takeRow() == int(row));
  assert(canvas.takeRow() == -1);
  canvas.pixel(-1, 0, 42); canvas.pixel(128, 0, 42);
  assert(canvas.takeRow() == -1);
  canvas.window(-1, 2, 1, 2); canvas.solid(0xf800, 3);
  assert(canvas.row(2)[0] == 0xf800 && canvas.row(2)[1] == 0xf800);
  assert(canvas.takeRow() == 2 && canvas.takeRow() == -1);
  canvas.pixel(0, 2, 0xf800); assert(canvas.takeRow() == -1);
  uint16_t native[] = {0x07e0, 0x001f};
  canvas.window(0, 3, 1, 3); canvas.pixels(native, 2, true);
  assert(canvas.row(3)[0] == 0x07e0 && canvas.row(3)[1] == 0x001f);
  uint8_t spi[] = {0xf8, 0x00, 0xff, 0xff};
  canvas.window(2, 3, 3, 3); canvas.pixels(spi, 2, false);
  assert(canvas.row(3)[2] == 0xf800 && canvas.row(3)[3] == 0xffff);
  canvas.rotation(1); canvas.pixel(0, 0, 0x1234); assert(canvas.row(0)[127] == 0x1234);
  canvas.rotation(2); canvas.pixel(0, 0, 0x5678); assert(canvas.row(127)[127] == 0x5678);
  canvas.rotation(3); canvas.pixel(0, 0, 0x9abc); assert(canvas.row(127)[0] == 0x9abc);
  canvas.rotation(0);
  uint16_t crossing[] = {1, 2, 3, 4, 5, 6, 7, 8};
  canvas.window(10, 20, 12, 21); canvas.pixels(crossing, 8, true);
  assert(canvas.row(20)[10] == 7 && canvas.row(20)[11] == 8 && canvas.row(20)[12] == 3);
  assert(canvas.row(21)[10] == 4 && canvas.row(21)[11] == 5 && canvas.row(21)[12] == 6);
  uint8_t crossingSpi[] = {0, 9, 0, 10}; canvas.pixels(crossingSpi, 2, false);
  assert(canvas.row(20)[12] == 9 && canvas.row(21)[10] == 10);
  canvas.solid(0x1234, 3);
  assert(canvas.row(21)[11] == 0x1234 && canvas.row(21)[12] == 0x1234 && canvas.row(20)[10] == 0x1234);
  canvas.window(0, 10, 127, 10); canvas.solid(0xf800, 128);
  uint8_t wire[kPacketMax]{};
  size_t length = packet(wire, canvas, 10, 0x12345678, 7);
  assert(length == 25 && wire[14] == 10 && wire[15] == 1 && wire[16] == 3);
  assert(wire[18] == 128 && wire[19] == 0xf8 && wire[20] == 0);
  assert(wire[6] == 7 && wire[10] == 0x78 && wire[13] == 0x12);
  for (unsigned x = 0; x < 128; ++x) canvas.pixel(x, 11, x);
  assert(packet(wire, canvas, 11, 1, 8) == kPacketMax && wire[15] == 0);
  assert(wire[18] == 0 && wire[19] == 0 && wire[272] == 0 && wire[273] == 127);
  assert(packet(wire, canvas, -1, 1, 9) == 22 && wire[5] == 0 && wire[16] == 0);
  assert(storage.front() == 0xbeef && storage.back() == 0xbeef);
  // A status bar rewritten continuously cannot starve the bottom row.
  canvas.invalidate();
  bool bottom = false;
  for (unsigned n = 0; n < 128; ++n) {
    canvas.pixel(0, 0, n); bottom |= canvas.takeRow() == 127;
  }
  assert(bottom);
  std::cout << "Mirror capture: clipping, rotations, byte order, dirty-row fairness, bounded RLE/raw packets and CRC passed\n";
}

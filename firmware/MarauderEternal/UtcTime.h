#pragma once

#include <stdint.h>

namespace marauder::clock {

struct UtcDateTime {
  uint16_t year;
  uint8_t month;
  uint8_t day;
  uint8_t hour;
  uint8_t minute;
  uint8_t second;
};

bool isLeapYear(uint16_t year);
uint8_t daysInMonth(uint16_t year, uint8_t month);
bool isValid(const UtcDateTime& value);
bool toUnixSeconds(const UtcDateTime& value, int64_t& seconds);
bool fromUnixSeconds(int64_t seconds, UtcDateTime& value);

}  // namespace marauder::clock

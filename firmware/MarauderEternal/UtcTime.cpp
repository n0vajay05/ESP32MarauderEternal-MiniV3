#include "UtcTime.h"

namespace marauder::clock {

bool isLeapYear(uint16_t year) {
  return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

uint8_t daysInMonth(uint16_t year, uint8_t month) {
  static constexpr uint8_t days[] = {
      31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12)
    return 0;
  if (month == 2 && isLeapYear(year))
    return 29;
  return days[month - 1];
}

bool isValid(const UtcDateTime& value) {
  return value.year >= 2020 && value.year <= 2099 &&
         value.month >= 1 && value.month <= 12 &&
         value.day >= 1 && value.day <= daysInMonth(value.year, value.month) &&
         value.hour <= 23 && value.minute <= 59 && value.second <= 59;
}

bool toUnixSeconds(const UtcDateTime& value, int64_t& seconds) {
  if (!isValid(value))
    return false;

  int64_t year = value.year;
  const uint8_t month = value.month;
  year -= month <= 2;
  const int64_t era = (year >= 0 ? year : year - 399) / 400;
  const uint32_t year_of_era = static_cast<uint32_t>(year - era * 400);
  const uint32_t day_of_year =
      (153U * (month > 2 ? month - 3U : month + 9U) + 2U) / 5U +
      value.day - 1U;
  const uint32_t day_of_era =
      year_of_era * 365U + year_of_era / 4U - year_of_era / 100U +
      day_of_year;
  const int64_t days_since_epoch =
      era * 146097LL + static_cast<int64_t>(day_of_era) - 719468LL;
  seconds = days_since_epoch * 86400LL +
            static_cast<int64_t>(value.hour) * 3600LL +
            static_cast<int64_t>(value.minute) * 60LL + value.second;
  return seconds >= 0;
}

bool fromUnixSeconds(int64_t seconds, UtcDateTime& value) {
  if (seconds < 0)
    return false;

  const int64_t whole_days = seconds / 86400LL;
  int64_t civil_days = whole_days + 719468LL;
  const int64_t era =
      (civil_days >= 0 ? civil_days : civil_days - 146096LL) / 146097LL;
  const uint32_t day_of_era =
      static_cast<uint32_t>(civil_days - era * 146097LL);
  const uint32_t year_of_era =
      (day_of_era - day_of_era / 1460U + day_of_era / 36524U -
       day_of_era / 146096U) / 365U;
  int64_t year = static_cast<int64_t>(year_of_era) + era * 400LL;
  const uint32_t day_of_year =
      day_of_era - (365U * year_of_era + year_of_era / 4U -
                    year_of_era / 100U);
  const uint32_t month_prime = (5U * day_of_year + 2U) / 153U;
  const uint8_t day = static_cast<uint8_t>(
      day_of_year - (153U * month_prime + 2U) / 5U + 1U);
  const uint8_t month = static_cast<uint8_t>(
      month_prime < 10U ? month_prime + 3U : month_prime - 9U);
  year += month <= 2;

  const int64_t day_seconds = seconds % 86400LL;
  value = {
      static_cast<uint16_t>(year),
      month,
      day,
      static_cast<uint8_t>(day_seconds / 3600LL),
      static_cast<uint8_t>((day_seconds % 3600LL) / 60LL),
      static_cast<uint8_t>(day_seconds % 60LL),
  };
  return isValid(value);
}

}  // namespace marauder::clock

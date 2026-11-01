#pragma once

#include <stdint.h>
#include <time.h>

// Convert an absolute NTP timestamp to a city's wall clock without changing TZ.
inline tm cityTime(time_t utc, int32_t utcOffset) {
  tm result{};
  time_t local = utc + utcOffset;
  gmtime_r(&local, &result);
  return result;
}

// Bits 0..6 correspond to Monday..Sunday.
inline bool alarmMatches(const tm &local, bool enabled, uint8_t hour,
                         uint8_t minute, uint8_t days) {
  const unsigned mondayFirst = static_cast<unsigned>((local.tm_wday + 6) % 7);
  return enabled && (days & (1U << mondayFirst)) && local.tm_hour == hour &&
         local.tm_min == minute;
}
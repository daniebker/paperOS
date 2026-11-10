#pragma once

#include <cstdint>

namespace calendar {

// Dates and times on the calendar, with no time zone attached: "civil
// seconds" count from 1970-01-01 00:00 as if every day had 86400 seconds.
// For UTC times they equal Unix time.
struct DateTime {
  int year = 1970;
  int month = 1;  // 1-12
  int day = 1;    // 1-31
  int hour = 0;
  int minute = 0;
  int second = 0;
};

constexpr int64_t kSecondsPerDay = 86400;

// Days since 1970-01-01 for a proleptic Gregorian date. `month` may be out of
// range (13 is January next year).
int64_t daysFromCivil(int year, int month, int day);
void civilFromDays(int64_t days, int& year, int& month, int& day);

// 0 = Monday ... 6 = Sunday.
int weekdayOfDays(int64_t days);
int daysInMonth(int year, int month);

int64_t toCivilSeconds(const DateTime& time);
DateTime fromCivilSeconds(int64_t seconds);

// Division that rounds towards negative infinity, for times before 1970.
int64_t floorDiv(int64_t value, int64_t divisor);

// The device's local time (the C library's TZ) for a Unix time, in civil
// seconds.
int64_t utcToLocal(int64_t unixTime);

}  // namespace calendar

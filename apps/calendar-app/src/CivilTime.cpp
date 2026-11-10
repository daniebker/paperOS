#include "CivilTime.h"

#include <ctime>

namespace calendar {

int64_t floorDiv(int64_t value, int64_t divisor) {
  int64_t q = value / divisor;
  if ((value % divisor != 0) && ((value < 0) != (divisor < 0))) --q;
  return q;
}

// Howard Hinnant's days_from_civil / civil_from_days.
int64_t daysFromCivil(int year, int month, int day) {
  int64_t y = year + floorDiv(month - 1, 12);
  const int64_t m = (month - 1) - floorDiv(month - 1, 12) * 12 + 1;
  y -= m <= 2;
  const int64_t era = floorDiv(y, 400);
  const int64_t yoe = y - era * 400;
  const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

void civilFromDays(int64_t days, int& year, int& month, int& day) {
  days += 719468;
  const int64_t era = floorDiv(days, 146097);
  const int64_t doe = days - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  day = int(doy - (153 * mp + 2) / 5 + 1);
  month = int(mp < 10 ? mp + 3 : mp - 9);
  year = int(yoe + era * 400 + (month <= 2));
}

int weekdayOfDays(int64_t days) {
  // 1970-01-01 was a Thursday (3).
  return int((days % 7 + 7 + 3) % 7);
}

int daysInMonth(int year, int month) {
  return int(daysFromCivil(year, month + 1, 1) - daysFromCivil(year, month, 1));
}

int64_t toCivilSeconds(const DateTime& t) {
  return daysFromCivil(t.year, t.month, t.day) * kSecondsPerDay + t.hour * 3600 +
         t.minute * 60 + t.second;
}

DateTime fromCivilSeconds(int64_t seconds) {
  DateTime t;
  const int64_t days = floorDiv(seconds, kSecondsPerDay);
  civilFromDays(days, t.year, t.month, t.day);
  int64_t rest = seconds - days * kSecondsPerDay;
  t.hour = int(rest / 3600);
  rest %= 3600;
  t.minute = int(rest / 60);
  t.second = int(rest % 60);
  return t;
}

int64_t utcToLocal(int64_t unixTime) {
  const time_t t = time_t(unixTime);
  tm local = {};
  localtime_r(&t, &local);
  DateTime d;
  d.year = local.tm_year + 1900;
  d.month = local.tm_mon + 1;
  d.day = local.tm_mday;
  d.hour = local.tm_hour;
  d.minute = local.tm_min;
  d.second = local.tm_sec;
  return toCivilSeconds(d);
}

}  // namespace calendar

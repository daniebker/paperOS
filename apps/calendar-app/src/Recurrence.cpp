#include "Recurrence.h"

#include <algorithm>
#include <climits>
#include <vector>

#include "CivilTime.h"

namespace calendar {

namespace {

// Safety net for rules that never produce an occurrence.
constexpr int64_t kMaxPeriods = 200000;

using WeekdayNum = RRule::WeekdayNum;

// DTSTART split into the parts the rule refers to.
struct Seed {
  explicit Seed(int64_t start) {
    day0 = floorDiv(start, kSecondsPerDay);
    timeOfDay = start - day0 * kSecondsPerDay;
    civilFromDays(day0, year, month, day);
    weekday = weekdayOfDays(day0);
  }
  int64_t day0;
  int64_t timeOfDay;
  int year, month, day;
  int weekday;
};

bool contains(const std::vector<int>& values, int value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

bool hasWeekday(const std::vector<WeekdayNum>& byDay, int weekday) {
  for (const WeekdayNum& w : byDay) {
    if (w.weekday == weekday) return true;
  }
  return false;
}

int monthOfDays(int64_t days) {
  int y, m, d;
  civilFromDays(days, y, m, d);
  return m;
}

// Days in [first, first + length) matching `byDay`, with ordinals such as
// "2nd Tuesday" or "last Friday" counted within that range.
void matchWeekdays(int64_t first, int length, const std::vector<WeekdayNum>& byDay,
                   std::vector<int64_t>& out) {
  const int64_t lastDay = first + length - 1;
  for (const WeekdayNum& w : byDay) {
    if (w.ordinal == 0) {
      for (int64_t d = first + (w.weekday - weekdayOfDays(first) + 7) % 7; d <= lastDay; d += 7) {
        out.push_back(d);
      }
    } else if (w.ordinal > 0) {
      const int64_t d =
          first + (w.weekday - weekdayOfDays(first) + 7) % 7 + 7 * int64_t(w.ordinal - 1);
      if (d <= lastDay) out.push_back(d);
    } else {
      const int64_t d =
          lastDay - (weekdayOfDays(lastDay) - w.weekday + 7) % 7 + 7 * int64_t(w.ordinal + 1);
      if (d >= first) out.push_back(d);
    }
  }
}

// Candidate days in one month, from BYMONTHDAY and BYDAY, or DTSTART's day.
void monthDays(int year, int month, const RRule& rule, const Seed& seed,
               std::vector<int64_t>& out) {
  const int64_t first = daysFromCivil(year, month, 1);
  const int length = daysInMonth(year, month);
  if (!rule.byMonthDay.empty()) {
    std::vector<int64_t> weekdays;
    if (!rule.byDay.empty()) matchWeekdays(first, length, rule.byDay, weekdays);
    for (int monthDay : rule.byMonthDay) {
      const int day = monthDay > 0 ? monthDay : length + monthDay + 1;
      if (day < 1 || day > length) continue;
      const int64_t d = first + day - 1;
      if (rule.byDay.empty() || std::find(weekdays.begin(), weekdays.end(), d) != weekdays.end()) {
        out.push_back(d);
      }
    }
  } else if (!rule.byDay.empty()) {
    matchWeekdays(first, length, rule.byDay, out);
  } else if (seed.day <= length) {
    out.push_back(first + seed.day - 1);  // e.g. no 31 April: skipped
  }
}

// Fills `out` with the sorted days of period `k` (the k-th day, week, month or
// year counted in INTERVALs from DTSTART) and returns the period's first day.
int64_t periodDays(const RRule& rule, const Seed& seed, int64_t k, std::vector<int64_t>& out) {
  out.clear();
  int64_t periodStart = 0;
  switch (rule.freq) {
    case RRule::Freq::Daily: {
      const int64_t d = seed.day0 + k * rule.interval;
      periodStart = d;
      int y, m, md;
      civilFromDays(d, y, m, md);
      if ((rule.byMonth.empty() || contains(rule.byMonth, m)) &&
          (rule.byMonthDay.empty() || contains(rule.byMonthDay, md) ||
           contains(rule.byMonthDay, md - daysInMonth(y, m) - 1)) &&
          (rule.byDay.empty() || hasWeekday(rule.byDay, weekdayOfDays(d)))) {
        out.push_back(d);
      }
      break;
    }
    case RRule::Freq::Weekly: {
      const int64_t weekStart = seed.day0 - (seed.weekday - rule.weekStart + 7) % 7;
      periodStart = weekStart + 7 * k * rule.interval;
      for (int64_t d = periodStart; d < periodStart + 7; ++d) {
        const int weekday = weekdayOfDays(d);
        const bool dayMatches =
            rule.byDay.empty() ? weekday == seed.weekday : hasWeekday(rule.byDay, weekday);
        if (dayMatches && (rule.byMonth.empty() || contains(rule.byMonth, monthOfDays(d)))) {
          out.push_back(d);
        }
      }
      break;
    }
    case RRule::Freq::Monthly: {
      const int64_t index = int64_t(seed.year) * 12 + (seed.month - 1) + k * rule.interval;
      const int year = int(floorDiv(index, 12));
      const int month = int(index - int64_t(year) * 12) + 1;
      periodStart = daysFromCivil(year, month, 1);
      if (rule.byMonth.empty() || contains(rule.byMonth, month)) {
        monthDays(year, month, rule, seed, out);
      }
      break;
    }
    case RRule::Freq::Yearly: {
      const int year = int(seed.year + k * rule.interval);
      periodStart = daysFromCivil(year, 1, 1);
      if (!rule.byMonth.empty()) {
        for (int month = 1; month <= 12; ++month) {
          if (contains(rule.byMonth, month)) monthDays(year, month, rule, seed, out);
        }
      } else if (!rule.byDay.empty() && rule.byMonthDay.empty()) {
        const int length = int(daysFromCivil(year + 1, 1, 1) - periodStart);
        matchWeekdays(periodStart, length, rule.byDay, out);
      } else if (!rule.byMonthDay.empty()) {
        for (int month = 1; month <= 12; ++month) monthDays(year, month, rule, seed, out);
      } else {
        monthDays(year, seed.month, rule, seed, out);
      }
      break;
    }
    case RRule::Freq::None:
      break;
  }

  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  if (!rule.bySetPos.empty() && !out.empty()) {
    std::vector<int64_t> picked;
    const int size = int(out.size());
    for (int pos : rule.bySetPos) {
      const int index = pos > 0 ? pos - 1 : size + pos;
      if (pos != 0 && index >= 0 && index < size) picked.push_back(out[index]);
    }
    std::sort(picked.begin(), picked.end());
    picked.erase(std::unique(picked.begin(), picked.end()), picked.end());
    out.swap(picked);
  }
  return periodStart;
}

// Index of a period a little before the one holding `day`, so expansion can
// start there instead of at DTSTART.
int64_t periodBefore(const RRule& rule, const Seed& seed, int64_t day) {
  int64_t elapsed = 0;  // in units of the frequency
  switch (rule.freq) {
    case RRule::Freq::Daily: elapsed = day - seed.day0; break;
    case RRule::Freq::Weekly: elapsed = floorDiv(day - seed.day0, 7); break;
    case RRule::Freq::Monthly:
    case RRule::Freq::Yearly: {
      int y, m, d;
      civilFromDays(day, y, m, d);
      elapsed = (int64_t(y) - seed.year) * 12 + (m - seed.month);
      if (rule.freq == RRule::Freq::Yearly) elapsed = floorDiv(elapsed, 12);
      break;
    }
    case RRule::Freq::None: break;
  }
  return std::max<int64_t>(0, floorDiv(elapsed, rule.interval) - 1);
}

}  // namespace

void expandRule(const RRule& rule, int64_t start, int64_t until, int64_t first, int64_t last,
                const std::function<bool(int64_t)>& visit) {
  if (rule.freq == RRule::Freq::None) return;
  const Seed seed(start);
  int64_t k = 0;
  if (rule.count == 0 && first > start) {
    k = periodBefore(rule, seed, floorDiv(first, kSecondsPerDay));
  }

  std::vector<int64_t> days;
  int emitted = 0;
  for (int64_t guard = 0; guard < kMaxPeriods; ++guard, ++k) {
    const int64_t periodStart = periodDays(rule, seed, k, days);
    if (periodStart * kSecondsPerDay > last) return;
    for (int64_t day : days) {
      const int64_t t = day * kSecondsPerDay + seed.timeOfDay;
      if (t < start) continue;
      if (t > until || t > last) return;
      if (rule.count > 0 && emitted >= rule.count) return;
      ++emitted;
      if (!visit(t)) return;
    }
  }
}

int64_t yearlyOnset(const RRule& rule, int64_t start, int year) {
  if (rule.freq != RRule::Freq::Yearly) return INT64_MIN;
  const Seed seed(start);
  const int64_t k = int64_t(year) - seed.year;
  if (k < 0 || k % rule.interval != 0) return INT64_MIN;
  std::vector<int64_t> days;
  periodDays(rule, seed, k / rule.interval, days);
  for (int64_t day : days) {
    const int64_t t = day * kSecondsPerDay + seed.timeOfDay;
    if (t < start) continue;
    if (rule.until.valid && t > rule.until.civil) return INT64_MIN;
    return t;
  }
  return INT64_MIN;
}

}  // namespace calendar

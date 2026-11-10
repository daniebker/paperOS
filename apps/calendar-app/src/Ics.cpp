#include "Ics.h"

#include <cstdlib>

#include "CivilTime.h"

namespace calendar {

namespace {

char upper(char c) { return c >= 'a' && c <= 'z' ? char(c - 'a' + 'A') : c; }

std::string toUpper(std::string text) {
  for (char& c : text) c = upper(c);
  return text;
}

bool isDigit(char c) { return c >= '0' && c <= '9'; }

// Parses `count` digits at `text[pos]`.
bool readDigits(const std::string& text, size_t pos, size_t count, int& out) {
  if (pos + count > text.size()) return false;
  out = 0;
  for (size_t i = pos; i < pos + count; ++i) {
    if (!isDigit(text[i])) return false;
    out = out * 10 + (text[i] - '0');
  }
  return true;
}

// Whole number with an optional sign, filling all of `text`.
bool parseInt(const std::string& text, int& out) {
  if (text.empty()) return false;
  char* end = nullptr;
  const long value = std::strtol(text.c_str(), &end, 10);
  if (*end != '\0') return false;
  out = int(value);
  return true;
}

// Splits on `separator`, skipping empty pieces.
std::vector<std::string> split(const std::string& text, char separator) {
  std::vector<std::string> parts;
  size_t start = 0;
  while (start <= text.size()) {
    size_t end = text.find(separator, start);
    if (end == std::string::npos) end = text.size();
    if (end > start) parts.push_back(text.substr(start, end - start));
    start = end + 1;
  }
  return parts;
}

bool parseIntList(const std::string& text, std::vector<int>& out) {
  out.clear();
  for (const std::string& part : split(text, ',')) {
    int value;
    if (!parseInt(part, value)) return false;
    out.push_back(value);
  }
  return true;
}

int weekdayFromCode(const std::string& code) {
  static const char* const kCodes[] = {"MO", "TU", "WE", "TH", "FR", "SA", "SU"};
  for (int i = 0; i < 7; ++i) {
    if (code == kCodes[i]) return i;
  }
  return -1;
}

}  // namespace

std::string icsParameter(const std::string& params, const char* name) {
  const std::string wanted = toUpper(name);
  size_t i = 0;
  while (i < params.size()) {
    if (params[i] == ';') ++i;
    // Read one NAME=VALUE, where VALUE may be quoted.
    const size_t nameStart = i;
    while (i < params.size() && params[i] != '=' && params[i] != ';') ++i;
    const std::string key = toUpper(params.substr(nameStart, i - nameStart));
    std::string value;
    if (i < params.size() && params[i] == '=') {
      ++i;
      bool quoted = false;
      while (i < params.size() && (quoted || params[i] != ';')) {
        if (params[i] == '"') {
          quoted = !quoted;
        } else {
          value += params[i];
        }
        ++i;
      }
    }
    if (key == wanted) return value;
  }
  return "";
}

bool parseIcsTime(const std::string& text, const std::string& params, IcsTime& out) {
  out = IcsTime();
  DateTime t;
  if (!readDigits(text, 0, 4, t.year) || !readDigits(text, 4, 2, t.month) ||
      !readDigits(text, 6, 2, t.day)) {
    return false;
  }
  if (t.month < 1 || t.month > 12 || t.day < 1 || t.day > daysInMonth(t.year, t.month)) {
    return false;
  }
  if (text.size() == 8) {
    out.dateOnly = true;
  } else {
    if (text.size() < 15 || text[8] != 'T' || !readDigits(text, 9, 2, t.hour) ||
        !readDigits(text, 11, 2, t.minute) || !readDigits(text, 13, 2, t.second)) {
      return false;
    }
    if (text.size() == 16 && upper(text[15]) == 'Z') {
      out.utc = true;
    } else if (text.size() != 15) {
      return false;
    }
    if (t.hour > 23 || t.minute > 59 || t.second > 60) return false;
  }
  out.civil = toCivilSeconds(t);
  if (!out.utc) out.tzid = icsParameter(params, "TZID");
  out.valid = true;
  return true;
}

bool parseIcsDuration(const std::string& text, int64_t& seconds) {
  size_t i = 0;
  int sign = 1;
  if (i < text.size() && (text[i] == '+' || text[i] == '-')) sign = text[i++] == '-' ? -1 : 1;
  if (i >= text.size() || upper(text[i++]) != 'P') return false;

  int64_t total = 0;
  bool inTime = false;
  bool any = false;
  while (i < text.size()) {
    if (upper(text[i]) == 'T') {
      inTime = true;
      ++i;
      continue;
    }
    int64_t value = 0;
    const size_t digitsStart = i;
    while (i < text.size() && isDigit(text[i])) value = value * 10 + (text[i++] - '0');
    if (i == digitsStart || i >= text.size()) return false;
    switch (upper(text[i++])) {
      case 'W': total += value * 7 * kSecondsPerDay; break;
      case 'D': total += value * kSecondsPerDay; break;
      case 'H': if (!inTime) return false; total += value * 3600; break;
      case 'M': if (!inTime) return false; total += value * 60; break;
      case 'S': if (!inTime) return false; total += value; break;
      default: return false;
    }
    any = true;
  }
  if (!any) return false;
  seconds = sign * total;
  return true;
}

bool parseIcsUtcOffset(const std::string& text, int& seconds) {
  if (text.size() != 5 && text.size() != 7) return false;
  if (text[0] != '+' && text[0] != '-') return false;
  int hours, minutes, secs = 0;
  if (!readDigits(text, 1, 2, hours) || !readDigits(text, 3, 2, minutes)) return false;
  if (text.size() == 7 && !readDigits(text, 5, 2, secs)) return false;
  seconds = (text[0] == '-' ? -1 : 1) * (hours * 3600 + minutes * 60 + secs);
  return true;
}

bool parseRRule(const std::string& text, RRule& out) {
  out = RRule();
  bool ok = true;
  for (const std::string& part : split(text, ';')) {
    const size_t eq = part.find('=');
    if (eq == std::string::npos) {
      ok = false;
      continue;
    }
    const std::string key = toUpper(part.substr(0, eq));
    const std::string value = toUpper(part.substr(eq + 1));
    if (key == "FREQ") {
      if (value == "DAILY") out.freq = RRule::Freq::Daily;
      else if (value == "WEEKLY") out.freq = RRule::Freq::Weekly;
      else if (value == "MONTHLY") out.freq = RRule::Freq::Monthly;
      else if (value == "YEARLY") out.freq = RRule::Freq::Yearly;
    } else if (key == "INTERVAL") {
      if (!parseInt(value, out.interval) || out.interval < 1) {
        out.interval = 1;
        ok = false;
      }
    } else if (key == "COUNT") {
      if (!parseInt(value, out.count) || out.count < 0) {
        out.count = 0;
        ok = false;
      }
    } else if (key == "UNTIL") {
      ok = parseIcsTime(value, "", out.until) && ok;
    } else if (key == "BYDAY") {
      for (const std::string& item : split(value, ',')) {
        if (item.size() < 2) {
          ok = false;
          continue;
        }
        const int weekday = weekdayFromCode(item.substr(item.size() - 2));
        int ordinal = 0;
        const std::string number = item.substr(0, item.size() - 2);
        if (weekday < 0 || (!number.empty() && !parseInt(number, ordinal))) {
          ok = false;
          continue;
        }
        out.byDay.push_back(RRule::WeekdayNum{ordinal, weekday});
      }
    } else if (key == "BYMONTHDAY") {
      ok = parseIntList(value, out.byMonthDay) && ok;
    } else if (key == "BYMONTH") {
      ok = parseIntList(value, out.byMonth) && ok;
    } else if (key == "BYSETPOS") {
      ok = parseIntList(value, out.bySetPos) && ok;
    } else if (key == "WKST") {
      const int weekday = weekdayFromCode(value);
      if (weekday >= 0) out.weekStart = weekday;
    }
    // Other parts (BYHOUR, BYWEEKNO, ...) are rare in calendars; ignored.
  }
  return ok && out.freq != RRule::Freq::None;
}

std::string unescapeIcsText(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] != '\\' || i + 1 >= text.size()) {
      out += text[i];
      continue;
    }
    const char next = text[++i];
    out += next == 'n' || next == 'N' ? ' ' : next;
  }
  return out;
}

void IcsParser::feed(const char* data, size_t size) {
  for (size_t i = 0; i < size; ++i) {
    const char c = data[i];
    if (lineEnded_) {
      lineEnded_ = false;
      if (c == ' ' || c == '\t') continue;  // folded: the line goes on
      onLine(line_);
      line_.clear();
    }
    if (c == '\n') {
      lineEnded_ = true;
    } else if (c != '\r' && line_.size() < kMaxLineBytes) {
      line_ += c;
    }
  }
}

void IcsParser::finish() {
  if (!line_.empty()) onLine(line_);
  line_.clear();
  lineEnded_ = false;
}

void IcsParser::onLine(const std::string& line) {
  // NAME *(;PARAM) : VALUE, where quoted parameter values may contain ':'.
  size_t i = 0;
  while (i < line.size() && line[i] != ';' && line[i] != ':') ++i;
  const std::string name = toUpper(line.substr(0, i));
  const size_t paramsStart = i;
  bool quoted = false;
  while (i < line.size() && (quoted || line[i] != ':')) {
    if (line[i] == '"') quoted = !quoted;
    ++i;
  }
  if (name.empty() || i >= line.size()) return;  // not a property
  const std::string params = line.substr(paramsStart, i - paramsStart);
  const std::string value = line.substr(i + 1);

  const Component top = stack_.empty() ? Component::Other : stack_.back();
  if (name == "BEGIN") {
    const std::string kind = toUpper(value);
    Component component = Component::Other;
    if (kind == "VCALENDAR") {
      sawCalendar_ = true;
    } else if (kind == "VEVENT") {
      component = Component::Event;
      event_ = IcsEvent();
    } else if (kind == "VTIMEZONE") {
      component = Component::TimeZone;
      zone_ = IcsTimeZone();
    } else if (top == Component::TimeZone && (kind == "STANDARD" || kind == "DAYLIGHT")) {
      component = Component::Observance;
      observance_ = IcsObservance();
    }
    stack_.push_back(component);
    return;
  }
  if (name == "END") {
    if (stack_.empty()) return;
    stack_.pop_back();
    if (top == Component::Event && event_.start.valid) {
      sink_.onEvent(event_);
    } else if (top == Component::TimeZone && !zone_.tzid.empty()) {
      sink_.onTimeZone(zone_);
    } else if (top == Component::Observance) {
      zone_.observances.push_back(observance_);
    }
    return;
  }
  onProperty(name, params, value);
}

void IcsParser::onProperty(const std::string& name, const std::string& params,
                           const std::string& value) {
  const Component top = stack_.empty() ? Component::Other : stack_.back();
  if (top == Component::Event) {
    if (name == "UID") {
      event_.uid = value;
    } else if (name == "SUMMARY") {
      event_.summary = unescapeIcsText(value);
    } else if (name == "LOCATION") {
      event_.location = unescapeIcsText(value);
    } else if (name == "DTSTART") {
      parseIcsTime(value, params, event_.start);
    } else if (name == "DTEND") {
      parseIcsTime(value, params, event_.end);
    } else if (name == "DURATION") {
      if (!parseIcsDuration(value, event_.duration)) event_.duration = -1;
    } else if (name == "RRULE") {
      // A rule with a bad part keeps the parts that could be read.
      parseRRule(value, event_.rule);
    } else if (name == "EXDATE") {
      for (const std::string& item : split(value, ',')) {
        IcsTime time;
        if (parseIcsTime(item, params, time)) event_.exceptions.push_back(time);
      }
    } else if (name == "RECURRENCE-ID") {
      parseIcsTime(value, params, event_.recurrenceId);
    } else if (name == "STATUS") {
      event_.cancelled = toUpper(value) == "CANCELLED";
    }
  } else if (top == Component::TimeZone) {
    if (name == "TZID") zone_.tzid = value;
  } else if (top == Component::Observance) {
    if (name == "DTSTART") {
      IcsTime start;
      if (parseIcsTime(value, "", start)) observance_.start = start.civil;
    } else if (name == "TZOFFSETTO") {
      parseIcsUtcOffset(value, observance_.offsetTo);
    } else if (name == "RRULE") {
      parseRRule(value, observance_.rule);
    }
  }
}

}  // namespace calendar

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace calendar {

// A DATE or DATE-TIME value from an iCalendar file.
struct IcsTime {
  int64_t civil = 0;  // civil seconds, see CivilTime.h
  bool valid = false;
  bool dateOnly = false;  // VALUE=DATE, e.g. all-day events
  bool utc = false;       // ends in "Z"
  std::string tzid;       // TZID parameter; empty for UTC and floating times
};

// The parts of an RRULE paperOS understands. Rules with an unsupported FREQ
// (HOURLY and finer) have freq None, and their event shows once.
struct RRule {
  enum class Freq : uint8_t { None, Daily, Weekly, Monthly, Yearly };
  struct WeekdayNum {
    int ordinal;  // 0 = every; 1 = first, -1 = last, ...
    int weekday;  // 0 = Monday ... 6 = Sunday
  };

  Freq freq = Freq::None;
  int interval = 1;
  int count = 0;  // 0 = no limit
  IcsTime until;  // invalid = no limit
  std::vector<WeekdayNum> byDay;
  std::vector<int> byMonthDay;
  std::vector<int> byMonth;
  std::vector<int> bySetPos;
  int weekStart = 0;  // Monday
};

struct IcsEvent {
  std::string uid;
  std::string summary;   // UTF-8, unescaped
  std::string location;  // UTF-8, unescaped
  IcsTime start;
  IcsTime end;                 // invalid if not given
  int64_t duration = -1;       // seconds from DURATION, or -1
  RRule rule;                  // freq None if not recurring
  std::vector<IcsTime> exceptions;  // EXDATE
  IcsTime recurrenceId;        // valid when this replaces one occurrence
  bool cancelled = false;
};

// One STANDARD or DAYLIGHT part of a VTIMEZONE.
struct IcsObservance {
  int64_t start = 0;   // civil seconds, in the offset before this onset
  int offsetTo = 0;    // seconds east of UTC from this onset
  RRule rule;          // freq None for a single onset
};

struct IcsTimeZone {
  std::string tzid;
  std::vector<IcsObservance> observances;
};

// Reads an iCalendar (RFC 5545) stream fed a piece at a time, so large files
// can be parsed from tick() without holding them in memory. Reports each
// VEVENT and VTIMEZONE as it ends; everything else is skipped.
class IcsParser {
 public:
  class Sink {
   public:
    virtual ~Sink() = default;
    virtual void onEvent(const IcsEvent& event) = 0;
    virtual void onTimeZone(const IcsTimeZone& zone) { (void)zone; }
  };

  // Longer lines (usually attachments) are cut short, which is fine for the
  // properties read here.
  static constexpr size_t kMaxLineBytes = 4096;

  explicit IcsParser(Sink& sink) : sink_(sink) {}

  void feed(const char* data, size_t size);
  // Ends the stream, handling a last line with no line break.
  void finish();

  // True once a BEGIN:VCALENDAR line was seen, to tell a calendar from an
  // error page.
  bool sawCalendar() const { return sawCalendar_; }

 private:
  void onLine(const std::string& line);
  void onProperty(const std::string& name, const std::string& params,
                  const std::string& value);

  enum class Component : uint8_t { Other, Event, TimeZone, Observance };

  Sink& sink_;
  std::string line_;
  bool lineEnded_ = false;  // line_ holds a whole line unless the next folds
  bool sawCalendar_ = false;
  std::vector<Component> stack_;
  IcsEvent event_;
  IcsTimeZone zone_;
  IcsObservance observance_;
};

// Value parsers, exposed for tests. Each returns false if `text` is malformed.

// "20261108", "20261108T093000" or "20261108T093000Z". `params` is the raw
// parameter text after the property name, e.g. ";TZID=Europe/Berlin".
bool parseIcsTime(const std::string& text, const std::string& params, IcsTime& out);
// "P1D", "PT1H30M", "-P2W" in seconds.
bool parseIcsDuration(const std::string& text, int64_t& seconds);
// "+0100", "-0530", "+013045" in seconds east of UTC.
bool parseIcsUtcOffset(const std::string& text, int& seconds);
bool parseRRule(const std::string& text, RRule& out);
// Undoes TEXT escaping: "\n" becomes a space, "\," a comma, and so on.
std::string unescapeIcsText(const std::string& text);
// Value of parameter `name` (upper case) in raw parameter text, or "".
std::string icsParameter(const std::string& params, const char* name);

}  // namespace calendar

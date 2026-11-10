#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "Ics.h"

namespace calendar {

// One occurrence of an event, in the device's local time.
struct Occurrence {
  int64_t start = 0;  // civil seconds, local time
  int64_t end = 0;    // exclusive; equal to start for events with no length
  bool allDay = false;
  std::string summary;   // ASCII, ready to draw
  std::string location;  // ASCII, ready to draw
  uint8_t calendar = 0;  // index of the feed it came from
};

// Collects the occurrences that overlap a window of local time from one or
// more parsed calendars: expands recurring events, drops EXDATEs, applies
// moved or cancelled occurrences (RECURRENCE-ID) and converts every time to
// local time.
//
// Times with a TZID are converted using the calendar's VTIMEZONE of that
// name. Floating times, dates, and TZIDs the calendar doesn't define are
// taken to be in the device's time zone.
class AgendaBuilder : public IcsParser::Sink {
 public:
  // Most occurrences kept, to bound memory on huge calendars.
  static constexpr size_t kMaxOccurrences = 2000;

  // `windowStart` and `windowEnd` are local civil seconds.
  AgendaBuilder(int64_t windowStart, int64_t windowEnd);

  // Starts the calendar from feed `index`. Time zones and UIDs belong to one
  // calendar, so they don't carry over between calls.
  void beginCalendar(uint8_t index);
  void endCalendar();

  void onEvent(const IcsEvent& event) override;
  void onTimeZone(const IcsTimeZone& zone) override;

  // The occurrences so far, sorted by start time, all-day ones first.
  std::vector<Occurrence> take();

  // Local civil seconds for `civil` read in the time zone of `kind`.
  int64_t toLocal(int64_t civil, const IcsTime& kind) const;
  int64_t toLocal(const IcsTime& time) const { return toLocal(time.civil, time); }

 private:
  struct Pending {
    Occurrence occurrence;
    std::string uid;
    int64_t key;  // local start, to match a RECURRENCE-ID
  };
  struct Replaced {
    std::string uid;
    int64_t key;
  };

  const IcsTimeZone* findZone(const std::string& tzid) const;
  // Seconds east of UTC in `zone` at civil time `civil` of that zone.
  int offsetAt(const IcsTimeZone& zone, int64_t civil) const;
  // UNTIL in the civil time of `start`.
  int64_t untilFor(const RRule& rule, const IcsTime& start) const;
  bool overlaps(int64_t start, int64_t end) const;
  Occurrence makeOccurrence(const IcsEvent& event, int64_t start, int64_t end) const;
  void add(Occurrence occurrence);
  void expand(const IcsEvent& event, int64_t length);

  int64_t windowStart_;
  int64_t windowEnd_;
  uint8_t calendar_ = 0;
  std::vector<IcsTimeZone> zones_;
  std::vector<Pending> pending_;    // occurrences of recurring events
  std::vector<Replaced> replaced_;  // RECURRENCE-IDs seen in this calendar
  std::vector<Occurrence> result_;
  size_t count_ = 0;
};

}  // namespace calendar

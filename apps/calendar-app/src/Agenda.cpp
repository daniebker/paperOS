#include "Agenda.h"

#include <algorithm>
#include <climits>

#include <paperos/AsciiFold.h>

#include "CivilTime.h"
#include "Recurrence.h"

namespace calendar {

namespace {

// Room for time zone offsets when comparing civil times in different zones.
constexpr int64_t kZoneSlack = 2 * kSecondsPerDay;
// How far before the window a replaced occurrence is remembered, so long
// events that started earlier can still be matched.
constexpr int64_t kReplacedLookBack = 31 * kSecondsPerDay;

bool isUtcName(const std::string& tzid) {
  return tzid == "UTC" || tzid == "GMT" || tzid == "Etc/UTC" || tzid == "Etc/GMT" ||
         tzid == "Z";
}

bool sameZone(const IcsTime& a, const IcsTime& b) {
  return a.dateOnly == b.dateOnly && a.utc == b.utc && a.tzid == b.tzid;
}

bool occursBefore(const Occurrence& a, const Occurrence& b) {
  const int64_t dayA = floorDiv(a.start, kSecondsPerDay);
  const int64_t dayB = floorDiv(b.start, kSecondsPerDay);
  if (dayA != dayB) return dayA < dayB;
  if (a.allDay != b.allDay) return a.allDay;
  if (a.start != b.start) return a.start < b.start;
  return a.summary < b.summary;
}

}  // namespace

AgendaBuilder::AgendaBuilder(int64_t windowStart, int64_t windowEnd)
    : windowStart_(windowStart), windowEnd_(windowEnd) {}

void AgendaBuilder::beginCalendar(uint8_t index) {
  calendar_ = index;
  zones_.clear();
  pending_.clear();
  replaced_.clear();
}

void AgendaBuilder::endCalendar() {
  for (Pending& p : pending_) {
    const bool wasReplaced =
        std::any_of(replaced_.begin(), replaced_.end(),
                    [&](const Replaced& r) { return r.key == p.key && r.uid == p.uid; });
    if (!wasReplaced) result_.push_back(std::move(p.occurrence));
  }
  pending_.clear();
  replaced_.clear();
  zones_.clear();
}

std::vector<Occurrence> AgendaBuilder::take() {
  std::vector<Occurrence> out;
  out.swap(result_);
  std::sort(out.begin(), out.end(), occursBefore);
  count_ = 0;
  return out;
}

void AgendaBuilder::onTimeZone(const IcsTimeZone& zone) {
  if (zone.observances.empty() || findZone(zone.tzid) != nullptr) return;
  zones_.push_back(zone);
}

const IcsTimeZone* AgendaBuilder::findZone(const std::string& tzid) const {
  for (const IcsTimeZone& zone : zones_) {
    if (zone.tzid == tzid) return &zone;
  }
  return nullptr;
}

int AgendaBuilder::offsetAt(const IcsTimeZone& zone, int64_t civil) const {
  DateTime date = fromCivilSeconds(civil);
  int64_t latest = INT64_MIN;
  int offset = 0;
  int64_t earliest = INT64_MAX;
  for (const IcsObservance& o : zone.observances) {
    if (o.start < earliest) {
      earliest = o.start;
      if (latest == INT64_MIN) offset = o.offsetTo;  // before every onset
    }
    int64_t onsets[2] = {o.start, INT64_MIN};
    if (o.rule.freq == RRule::Freq::Yearly) {
      onsets[0] = yearlyOnset(o.rule, o.start, date.year);
      onsets[1] = yearlyOnset(o.rule, o.start, date.year - 1);
    }
    for (int64_t onset : onsets) {
      if (onset != INT64_MIN && onset <= civil && onset > latest) {
        latest = onset;
        offset = o.offsetTo;
      }
    }
  }
  return offset;
}

int64_t AgendaBuilder::toLocal(int64_t civil, const IcsTime& kind) const {
  if (kind.dateOnly) return civil;
  if (kind.utc || isUtcName(kind.tzid)) return utcToLocal(civil);
  if (kind.tzid.empty()) return civil;  // floating
  const IcsTimeZone* zone = findZone(kind.tzid);
  if (zone == nullptr) return civil;  // unknown zone: assume it is ours
  return utcToLocal(civil - offsetAt(*zone, civil));
}

int64_t AgendaBuilder::untilFor(const RRule& rule, const IcsTime& start) const {
  if (!rule.until.valid) return INT64_MAX;
  const IcsTime& until = rule.until;
  if (until.dateOnly) {
    // A date includes the whole day.
    return start.dateOnly ? until.civil : until.civil + kSecondsPerDay - 1;
  }
  if (!until.utc || start.utc) return until.civil;
  if (start.dateOnly || start.tzid.empty()) return utcToLocal(until.civil);
  if (const IcsTimeZone* zone = findZone(start.tzid)) {
    return until.civil + offsetAt(*zone, until.civil);
  }
  return utcToLocal(until.civil);
}

bool AgendaBuilder::overlaps(int64_t start, int64_t end) const {
  return start < windowEnd_ && (end > windowStart_ || start >= windowStart_);
}

Occurrence AgendaBuilder::makeOccurrence(const IcsEvent& event, int64_t start,
                                         int64_t end) const {
  Occurrence o;
  o.start = start;
  o.end = std::max(start, end);
  o.allDay = event.start.dateOnly;
  o.summary = paperos::foldUtf8ToAscii(event.summary);
  o.location = paperos::foldUtf8ToAscii(event.location);
  o.calendar = calendar_;
  return o;
}

void AgendaBuilder::add(Occurrence occurrence) {
  if (count_ >= kMaxOccurrences) return;
  ++count_;
  result_.push_back(std::move(occurrence));
}

void AgendaBuilder::onEvent(const IcsEvent& event) {
  // Length in the civil time of DTSTART.
  int64_t length = 0;
  if (event.end.valid && sameZone(event.end, event.start)) {
    length = event.end.civil - event.start.civil;
  } else if (event.end.valid) {
    length = toLocal(event.end) - toLocal(event.start);
  } else if (event.duration >= 0) {
    length = event.duration;
  } else if (event.start.dateOnly) {
    length = kSecondsPerDay;
  }
  length = std::max<int64_t>(0, length);

  if (event.recurrenceId.valid) {
    const int64_t key = toLocal(event.recurrenceId);
    if (key >= windowStart_ - kReplacedLookBack && key < windowEnd_) {
      replaced_.push_back(Replaced{event.uid, key});
    }
  }
  if (event.cancelled) return;

  if (event.rule.freq != RRule::Freq::None && !event.recurrenceId.valid) {
    expand(event, length);
    return;
  }
  const int64_t start = toLocal(event.start);
  const int64_t end = toLocal(event.start.civil + length, event.start);
  if (overlaps(start, end)) add(makeOccurrence(event, start, end));
}

void AgendaBuilder::expand(const IcsEvent& event, int64_t length) {
  std::vector<int64_t> exceptions;
  for (const IcsTime& t : event.exceptions) exceptions.push_back(toLocal(t));

  const int64_t first = windowStart_ - length - kZoneSlack;
  const int64_t last = windowEnd_ + kZoneSlack;
  expandRule(event.rule, event.start.civil, untilFor(event.rule, event.start), first, last,
             [&](int64_t civil) {
               const int64_t start = toLocal(civil, event.start);
               if (std::find(exceptions.begin(), exceptions.end(), start) != exceptions.end()) {
                 return true;
               }
               const int64_t end = toLocal(civil + length, event.start);
               if (!overlaps(start, end)) return true;
               if (count_ >= kMaxOccurrences) return false;
               ++count_;
               pending_.push_back(Pending{makeOccurrence(event, start, end), event.uid, start});
               return true;
             });
}

}  // namespace calendar

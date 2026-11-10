#pragma once

#include <cstdint>
#include <functional>

#include "Ics.h"

namespace calendar {

// Calls `visit` with the start of each occurrence of `rule`, in order, in the
// same civil time as `start` (the event's DTSTART). Stops when the rule ends,
// after `last`, or when `visit` returns false.
//
// `until` is the rule's UNTIL already in that civil time (INT64_MAX for no
// limit). When the rule has no COUNT, occurrences well before `first` may be
// skipped to save time; some earlier ones can still be visited.
void expandRule(const RRule& rule, int64_t start, int64_t until, int64_t first, int64_t last,
                const std::function<bool(int64_t)>& visit);

// The start of the onset of `rule` in civil `year` at or after `start`, or
// INT64_MIN if there is none. Used for time zone transitions, which repeat
// yearly.
int64_t yearlyOnset(const RRule& rule, int64_t start, int year);

}  // namespace calendar

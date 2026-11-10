#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Reads the tasks in a Mindwtr data.json, and marks one done by editing the
// file in place.
//
// data.json is one JSON object: { "tasks": [...], "projects": [...],
// "areas": [...], "sections": [...], "people": [...], "settings": {...} }.
// Only the task and project fields paperOS shows are read; everything else is
// skipped, and left untouched when a task is marked done.
namespace mindwtr {

// Seconds since 1970 UTC, or kNoTime.
constexpr int64_t kNoTime = INT64_MIN;

struct ChecklistItem {
  std::string title;
  bool done = false;
};

struct Task {
  std::string id;
  std::string title;
  std::string status;  // inbox, next, waiting, someday, reference, done, archived
  std::string projectId;
  std::string description;
  std::string priority;      // low, medium, high, urgent, or ""
  std::string timeEstimate;  // as written, such as "30min"
  std::string location;
  std::string recurrence;    // "daily", "weekly", ... or "" if it doesn't repeat
  std::vector<std::string> contexts;
  std::vector<std::string> tags;
  std::vector<ChecklistItem> checklist;
  bool focused = false;  // isFocusedToday
  bool deleted = false;  // deletedAt or purgedAt set
  bool recurring = false;
  double focusOrder = 0;
  bool hasFocusOrder = false;
  int64_t start = kNoTime;  // startTime
  bool startHasTime = false;
  int64_t due = kNoTime;  // dueDate
  bool dueHasTime = false;
  int64_t rev = 0;
  size_t order = 0;  // position in the file
};

struct Project {
  std::string id;
  std::string title;
};

struct Data {
  std::vector<Task> tasks;
  std::vector<Project> projects;

  // The project's title, or "" if there is none.
  const std::string& projectTitle(const std::string& id) const;
  // The task with this id, or null.
  const Task* find(const std::string& id) const;
};

// Parses data.json. Returns false with a short reason in `error` if it isn't
// a Mindwtr data file.
bool parse(const std::string& text, Data& data, std::string& error);

// Parses a Mindwtr date: "2026-11-10" (local midnight, `hasTime` false), or
// "2026-11-10T09:30", "...:00.000Z" or "...+01:00" (local time without a zone).
// Returns kNoTime if it can't be read.
int64_t parseTime(const std::string& text, bool* hasTime = nullptr);
// Formats `utc` as an ISO 8601 UTC timestamp, "2026-11-10T08:30:00.000Z".
std::string isoTime(int64_t utc);

enum class DoneResult : uint8_t {
  Ok,
  NotFound,       // no task with this id
  AlreadyDone,    // done or archived
  Recurring,      // Mindwtr must complete it, to create the next one
  Invalid,        // the file couldn't be read
};

// Marks the task `id` done in `text`, the whole data.json: sets status to
// "done", completedAt and updatedAt to `nowUtc`, increments rev and sets
// revBy, so Mindwtr's sync takes this version of the task over older copies.
// The rest of the file is kept byte for byte.
DoneResult markDone(std::string& text, const std::string& id, int64_t nowUtc);

// The three lists the Tasks app shows, as indexes into Data::tasks.
struct Lists {
  std::vector<size_t> focus;     // focused today
  std::vector<size_t> next;      // available next actions
  std::vector<size_t> upcoming;  // starting or due within `days`
};

// Sorts the tasks into the lists, as of `nowUtc` in local time:
//   focus:    focused today, not finished, and started (or no start date);
//                in focus order
//   next:     status next, not focused, and started (or no start date);
//                due ones first, earliest first, then the rest in file order
//   upcoming: inbox, next or waiting, in neither list above, starting or due
//                from today to `days` days ahead; soonest first
// With the time unknown (nowUtc < 0), dates are ignored and upcoming is empty.
Lists buildLists(const Data& data, int64_t nowUtc, int days);

// The date Lists::upcoming sorts a task by: its start or due date, whichever
// is the first from local midnight of `nowUtc` on. kNoTime if neither is.
int64_t upcomingTime(const Task& task, int64_t nowUtc);

}  // namespace mindwtr

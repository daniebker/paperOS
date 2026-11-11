#include "Mindwtr.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace mindwtr {

namespace {

constexpr int64_t kSecondsPerDay = 86400;
constexpr int kMaxDepth = 64;

// A forgiving reader over JSON text that keeps track of positions, so values
// can be replaced in place.
class Reader {
 public:
  explicit Reader(const std::string& text) : text_(text) {}

  size_t pos() const { return pos_; }
  bool ok() const { return ok_; }
  void skipBom() {
    if (text_.compare(0, 3, "\xEF\xBB\xBF") == 0) pos_ = 3;
  }
  bool atEnd() {
    ws();
    return pos_ >= text_.size();
  }

  void ws() {
    while (pos_ < text_.size() &&
           (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' ||
            text_[pos_] == '\r')) {
      ++pos_;
    }
  }

  bool peek(char c) {
    ws();
    return pos_ < text_.size() && text_[pos_] == c;
  }

  bool expect(char c) {
    if (peek(c)) {
      ++pos_;
      return true;
    }
    return fail();
  }

  // Object and array iteration: call begin, then next() before each member.
  bool beginObject() { return expect('{'); }
  bool beginArray() { return expect('['); }

  // Moves to the next member's value and gives its key. False at the end of
  // the object, or on an error.
  bool nextMember(std::string& key) {
    if (!ok_) return false;
    if (peek('}')) {
      ++pos_;
      return false;
    }
    if (peek(',')) ++pos_;
    if (!readString(key)) return false;
    if (!expect(':')) return false;
    ws();
    return true;
  }

  // Moves to the next element. False at the end of the array, or on an error.
  bool nextElement() {
    if (!ok_) return false;
    if (peek(']')) {
      ++pos_;
      return false;
    }
    if (peek(',')) ++pos_;
    ws();
    if (pos_ >= text_.size()) return fail();
    return true;
  }

  bool readString(std::string& out) {
    out.clear();
    if (!expect('"')) return false;
    while (pos_ < text_.size()) {
      const char c = text_[pos_++];
      if (c == '"') return true;
      if (c != '\\') {
        out += c;
        continue;
      }
      if (pos_ >= text_.size()) break;
      const char e = text_[pos_++];
      switch (e) {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'u': {
          uint32_t cp = 0;
          if (!readHex4(cp)) return fail();
          if (cp >= 0xD800 && cp < 0xDC00 && pos_ + 1 < text_.size() && text_[pos_] == '\\' &&
              text_[pos_ + 1] == 'u') {
            pos_ += 2;
            uint32_t low = 0;
            if (!readHex4(low)) return fail();
            if (low >= 0xDC00 && low < 0xE000) {
              cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
            } else {
              appendUtf8(out, 0xFFFD);
              cp = low;
            }
          }
          appendUtf8(out, cp);
          break;
        }
        default: out += e; break;  // \" \\ \/
      }
    }
    return fail();
  }

  // A string, or "" for null or any other value, which is skipped.
  bool readStringOrNull(std::string& out) {
    if (peek('"')) return readString(out);
    out.clear();
    return skipValue();
  }

  // true / false; anything else (null, 0, 1) counts as its truthiness.
  bool readBool(bool& out) {
    ws();
    const size_t start = pos_;
    if (!skipValue()) return false;
    const std::string raw = text_.substr(start, pos_ - start);
    out = raw == "true" || (raw != "false" && raw != "null" && raw != "0" && raw != "\"\"");
    return true;
  }

  // The raw text of a scalar, such as a number.
  bool readRaw(std::string& out) {
    ws();
    const size_t start = pos_;
    if (!skipValue()) return false;
    out = text_.substr(start, pos_ - start);
    return true;
  }

  bool readStringArray(std::vector<std::string>& out) {
    out.clear();
    if (!peek('[')) return skipValue();
    beginArray();
    std::string item;
    while (nextElement()) {
      if (!readStringOrNull(item)) return false;
      if (!item.empty()) out.push_back(item);
    }
    return ok_;
  }

  bool skipValue() {
    ws();
    if (pos_ >= text_.size()) return fail();
    const char c = text_[pos_];
    if (c == '"') {
      return skipString();
    }
    if (c == '{' || c == '[') {
      int depth = 0;
      while (pos_ < text_.size()) {
        const char d = text_[pos_];
        if (d == '"') {
          if (!skipString()) return false;
          continue;
        }
        ++pos_;
        if (d == '{' || d == '[') {
          if (++depth > kMaxDepth) return fail();
        } else if (d == '}' || d == ']') {
          if (--depth == 0) return true;
        }
      }
      return fail();
    }
    const size_t start = pos_;
    while (pos_ < text_.size() && std::strchr(",}] \t\r\n", text_[pos_]) == nullptr) ++pos_;
    return pos_ > start || fail();
  }

  bool fail() {
    ok_ = false;
    return false;
  }

 private:
  bool skipString() {
    ++pos_;  // opening quote
    while (pos_ < text_.size()) {
      const char c = text_[pos_++];
      if (c == '\\') {
        ++pos_;
      } else if (c == '"') {
        return true;
      }
    }
    return fail();
  }

  bool readHex4(uint32_t& out) {
    if (pos_ + 4 > text_.size()) return false;
    out = 0;
    for (int i = 0; i < 4; ++i) {
      const char h = text_[pos_++];
      out <<= 4;
      if (h >= '0' && h <= '9') {
        out |= uint32_t(h - '0');
      } else if (h >= 'a' && h <= 'f') {
        out |= uint32_t(h - 'a' + 10);
      } else if (h >= 'A' && h <= 'F') {
        out |= uint32_t(h - 'A' + 10);
      } else {
        return false;
      }
    }
    return true;
  }

  static void appendUtf8(std::string& out, uint32_t cp) {
    if (cp >= 0xD800 && cp < 0xE000) cp = 0xFFFD;  // lone surrogate
    if (cp < 0x80) {
      out += char(cp);
    } else if (cp < 0x800) {
      out += char(0xC0 | cp >> 6);
      out += char(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out += char(0xE0 | cp >> 12);
      out += char(0x80 | (cp >> 6 & 0x3F));
      out += char(0x80 | (cp & 0x3F));
    } else {
      out += char(0xF0 | cp >> 18);
      out += char(0x80 | (cp >> 12 & 0x3F));
      out += char(0x80 | (cp >> 6 & 0x3F));
      out += char(0x80 | (cp & 0x3F));
    }
  }

  const std::string& text_;
  size_t pos_ = 0;
  bool ok_ = true;
};

// Days since 1970-01-01 of a proleptic Gregorian date.
int64_t daysFromCivil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = unsigned(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + int64_t(doe) - 719468;
}

void civilFromDays(int64_t z, int& y, int& m, int& d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = unsigned(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = int(doy - (153 * mp + 2) / 5 + 1);
  m = int(mp < 10 ? mp + 3 : mp - 9);
  y = int(int64_t(yoe) + era * 400 + (m <= 2));
}

int64_t localToUtc(int year, int month, int day, int hour, int minute, int second) {
  struct tm t = {};
  t.tm_year = year - 1900;
  t.tm_mon = month - 1;
  t.tm_mday = day;
  t.tm_hour = hour;
  t.tm_min = minute;
  t.tm_sec = second;
  t.tm_isdst = -1;
  return int64_t(mktime(&t));
}

int64_t localMidnight(int64_t utc) {
  const time_t seconds = time_t(utc);
  struct tm local;
  localtime_r(&seconds, &local);
  return localToUtc(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, 0, 0, 0);
}

int64_t addLocalDays(int64_t midnight, int days) {
  const time_t seconds = time_t(midnight);
  struct tm local;
  localtime_r(&seconds, &local);
  return localToUtc(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday + days, 0, 0, 0);
}

bool readDigits(const std::string& s, size_t& i, int count, int& out) {
  out = 0;
  for (int k = 0; k < count; ++k, ++i) {
    if (i >= s.size() || s[i] < '0' || s[i] > '9') return false;
    out = out * 10 + (s[i] - '0');
  }
  return true;
}

bool isFinished(const Task& t) {
  return t.status == "done" || t.status == "archived" || t.status == "reference";
}

// Reads one task object at the reader's position.
bool readTask(Reader& r, Task& task) {
  if (!r.beginObject()) return false;
  std::string key;
  std::string value;
  while (r.nextMember(key)) {
    bool ok = true;
    if (key == "id") {
      ok = r.readStringOrNull(task.id);
    } else if (key == "title") {
      ok = r.readStringOrNull(task.title);
    } else if (key == "status") {
      ok = r.readStringOrNull(task.status);
    } else if (key == "projectId") {
      ok = r.readStringOrNull(task.projectId);
    } else if (key == "areaId") {
      ok = r.readStringOrNull(task.areaId);
    } else if (key == "description") {
      ok = r.readStringOrNull(task.description);
    } else if (key == "priority") {
      ok = r.readStringOrNull(task.priority);
    } else if (key == "location") {
      ok = r.readStringOrNull(task.location);
    } else if (key == "timeEstimate") {
      if (r.peek('"')) {
        ok = r.readString(task.timeEstimate);
      } else {
        ok = r.readRaw(task.timeEstimate);
        if (task.timeEstimate == "null") task.timeEstimate.clear();
      }
    } else if (key == "startTime") {
      ok = r.readStringOrNull(value);
      task.start = parseTime(value, &task.startHasTime);
    } else if (key == "dueDate") {
      ok = r.readStringOrNull(value);
      task.due = parseTime(value, &task.dueHasTime);
    } else if (key == "contexts") {
      ok = r.readStringArray(task.contexts);
    } else if (key == "tags") {
      ok = r.readStringArray(task.tags);
    } else if (key == "isFocusedToday") {
      ok = r.readBool(task.focused);
    } else if (key == "focusOrder") {
      ok = r.readRaw(value);
      if (ok && value != "null") {
        task.focusOrder = std::strtod(value.c_str(), nullptr);
        task.hasFocusOrder = true;
      }
    } else if (key == "rev") {
      ok = r.readRaw(value);
      if (ok) task.rev = std::strtoll(value.c_str(), nullptr, 10);
    } else if (key == "deletedAt" || key == "purgedAt") {
      ok = r.readStringOrNull(value);
      if (!value.empty()) task.deleted = true;
    } else if (key == "recurrence") {
      if (r.peek('{')) {
        task.recurring = true;
        r.beginObject();
        std::string inner;
        while (r.nextMember(inner)) {
          if (inner == "rule") {
            ok = r.readStringOrNull(task.recurrence);
          } else {
            ok = r.skipValue();
          }
          if (!ok) break;
        }
        ok = ok && r.ok();
        if (task.recurrence.empty()) task.recurrence = "repeats";
      } else {
        ok = r.readStringOrNull(task.recurrence);
        task.recurring = !task.recurrence.empty();
      }
    } else if (key == "checklist") {
      if (r.peek('[')) {
        r.beginArray();
        while (r.nextElement()) {
          if (!r.peek('{')) {
            ok = r.skipValue();
            if (!ok) break;
            continue;
          }
          ChecklistItem item;
          r.beginObject();
          std::string inner;
          while (r.nextMember(inner)) {
            if (inner == "title") {
              ok = r.readStringOrNull(item.title);
            } else if (inner == "isCompleted") {
              ok = r.readBool(item.done);
            } else {
              ok = r.skipValue();
            }
            if (!ok) break;
          }
          if (!ok || !r.ok()) break;
          task.checklist.push_back(item);
        }
        ok = ok && r.ok();
      } else {
        ok = r.skipValue();
      }
    } else {
      ok = r.skipValue();
    }
    if (!ok) return r.fail();
  }
  return r.ok();
}

bool readProject(Reader& r, Project& project) {
  if (!r.beginObject()) return false;
  std::string key;
  while (r.nextMember(key)) {
    bool ok;
    if (key == "id") {
      ok = r.readStringOrNull(project.id);
    } else if (key == "title") {
      ok = r.readStringOrNull(project.title);
    } else if (key == "areaId") {
      ok = r.readStringOrNull(project.areaId);
    } else {
      ok = r.skipValue();
    }
    if (!ok) return r.fail();
  }
  return r.ok();
}

bool readArea(Reader& r, Area& area) {
  if (!r.beginObject()) return false;
  std::string key;
  std::string value;
  while (r.nextMember(key)) {
    bool ok;
    if (key == "id") {
      ok = r.readStringOrNull(area.id);
    } else if (key == "name" || (key == "title" && area.name.empty())) {
      ok = r.readStringOrNull(area.name);
    } else if (key == "order") {
      ok = r.readRaw(value);
      if (ok && value != "null") {
        area.order = std::strtod(value.c_str(), nullptr);
        area.hasOrder = true;
      }
    } else if (key == "deletedAt" || key == "purgedAt") {
      ok = r.readStringOrNull(value);
      if (!value.empty()) area.deleted = true;
    } else {
      ok = r.skipValue();
    }
    if (!ok) return r.fail();
  }
  return r.ok();
}

// Where a task object is, and where the values markDone() replaces are.
struct TaskSpan {
  struct Value {
    std::string key;
    size_t start = 0;
    size_t end = 0;
  };
  size_t start = 0;  // '{'
  size_t end = 0;    // after '}'
  bool empty = true;
  std::string id;
  std::string status;
  bool recurring = false;
  int64_t rev = 0;
  std::vector<Value> values;
};

bool scanTaskSpan(Reader& r, TaskSpan& span) {
  r.ws();
  span.start = r.pos();
  if (!r.beginObject()) return false;
  std::string key;
  std::string value;
  while (r.nextMember(key)) {
    span.empty = false;
    const size_t valueStart = r.pos();
    bool ok;
    if (key == "id") {
      ok = r.readStringOrNull(span.id);
    } else if (key == "status") {
      ok = r.readStringOrNull(span.status);
    } else if (key == "recurrence") {
      r.ws();
      const bool isNull = r.peek('n');
      std::string raw;
      ok = r.readRaw(raw);
      span.recurring = !isNull && raw != "\"\"";
    } else if (key == "rev") {
      ok = r.readRaw(value);
      if (ok) span.rev = std::strtoll(value.c_str(), nullptr, 10);
    } else {
      ok = r.skipValue();
    }
    if (!ok) return r.fail();
    span.values.push_back({key, valueStart, r.pos()});
  }
  span.end = r.pos();
  return r.ok();
}

// Moves the reader to the value of `key` in the top-level object. False if
// the file isn't an object or has no such key.
bool findTopLevel(Reader& r, const char* key) {
  if (!r.beginObject()) return false;
  std::string name;
  while (r.nextMember(name)) {
    if (name == key) return true;
    if (!r.skipValue()) return false;
  }
  return false;
}

std::string jsonString(const std::string& text) {
  std::string out = "\"";
  for (const char c : text) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (uint8_t(c) < 0x20) {
      char escape[8];
      std::snprintf(escape, sizeof(escape), "\\u%04x", unsigned(uint8_t(c)));
      out += escape;
    } else {
      out += c;
    }
  }
  return out + "\"";
}

}  // namespace

const std::string& Data::projectTitle(const std::string& id) const {
  static const std::string kNone;
  if (id.empty()) return kNone;
  for (const Project& p : projects)
    if (p.id == id) return p.title;
  return kNone;
}

const Area* Data::areaOf(const Task& task) const {
  std::string id = task.areaId;
  if (id.empty() && !task.projectId.empty()) {
    for (const Project& p : projects) {
      if (p.id == task.projectId) {
        id = p.areaId;
        break;
      }
    }
  }
  if (id.empty()) return nullptr;
  for (const Area& a : areas)
    if (a.id == id) return a.deleted ? nullptr : &a;
  return nullptr;
}

const Task* Data::find(const std::string& id) const {
  for (const Task& t : tasks)
    if (t.id == id) return &t;
  return nullptr;
}

bool parse(const std::string& text, Data& data, std::string& error) {
  data.tasks.clear();
  data.projects.clear();
  data.areas.clear();
  Reader r(text);
  r.skipBom();
  if (!r.peek('{')) {
    error = "Not a Mindwtr data file";
    return false;
  }
  r.beginObject();
  bool sawTasks = false;
  std::string key;
  while (r.nextMember(key)) {
    if (key == "tasks" && r.peek('[')) {
      sawTasks = true;
      r.beginArray();
      while (r.nextElement()) {
        if (!r.peek('{')) {
          if (!r.skipValue()) break;
          continue;
        }
        Task task;
        if (!readTask(r, task)) break;
        if (task.id.empty()) continue;
        task.order = data.tasks.size();
        data.tasks.push_back(std::move(task));
      }
    } else if (key == "projects" && r.peek('[')) {
      r.beginArray();
      while (r.nextElement()) {
        if (!r.peek('{')) {
          if (!r.skipValue()) break;
          continue;
        }
        Project project;
        if (!readProject(r, project)) break;
        data.projects.push_back(std::move(project));
      }
    } else if (key == "areas" && r.peek('[')) {
      r.beginArray();
      while (r.nextElement()) {
        if (!r.peek('{')) {
          if (!r.skipValue()) break;
          continue;
        }
        Area area;
        if (!readArea(r, area)) break;
        if (area.id.empty()) continue;
        area.position = data.areas.size();
        data.areas.push_back(std::move(area));
      }
    } else if (!r.skipValue()) {
      break;
    }
  }
  if (!r.ok()) {
    error = "data.json is damaged at byte " + std::to_string(r.pos());
    return false;
  }
  if (!sawTasks) {
    error = "Not a Mindwtr data file (no tasks)";
    return false;
  }
  return true;
}

int64_t parseTime(const std::string& text, bool* hasTime) {
  if (hasTime != nullptr) *hasTime = false;
  size_t i = 0;
  int year, month, day;
  if (!readDigits(text, i, 4, year) || i >= text.size() || text[i++] != '-' ||
      !readDigits(text, i, 2, month) || i >= text.size() || text[i++] != '-' ||
      !readDigits(text, i, 2, day) || month < 1 || month > 12 || day < 1 || day > 31) {
    return kNoTime;
  }
  if (i >= text.size()) return localToUtc(year, month, day, 0, 0, 0);
  if (text[i] != 'T' && text[i] != ' ') return kNoTime;
  ++i;
  int hour, minute, second = 0;
  if (!readDigits(text, i, 2, hour) || i >= text.size() || text[i++] != ':' ||
      !readDigits(text, i, 2, minute)) {
    return kNoTime;
  }
  if (i < text.size() && text[i] == ':') {
    ++i;
    if (!readDigits(text, i, 2, second)) return kNoTime;
    if (i < text.size() && text[i] == '.') {
      ++i;
      while (i < text.size() && text[i] >= '0' && text[i] <= '9') ++i;
    }
  }
  if (hasTime != nullptr) *hasTime = true;
  if (i >= text.size()) return localToUtc(year, month, day, hour, minute, second);
  const int64_t asUtc =
      daysFromCivil(year, unsigned(month), unsigned(day)) * kSecondsPerDay + hour * 3600 +
      minute * 60 + second;
  if (text[i] == 'Z' || text[i] == 'z') return asUtc;
  if (text[i] == '+' || text[i] == '-') {
    const int sign = text[i] == '-' ? -1 : 1;
    ++i;
    int offH, offM = 0;
    if (!readDigits(text, i, 2, offH)) return kNoTime;
    if (i < text.size() && text[i] == ':') ++i;
    if (i < text.size()) readDigits(text, i, 2, offM);
    return asUtc - sign * (offH * 3600 + offM * 60);
  }
  return kNoTime;
}

std::string isoTime(int64_t utc) {
  int64_t days = utc / kSecondsPerDay;
  int64_t rest = utc % kSecondsPerDay;
  if (rest < 0) {
    rest += kSecondsPerDay;
    --days;
  }
  int y, m, d;
  civilFromDays(days, y, m, d);
  char out[32];
  std::snprintf(out, sizeof(out), "%04d-%02d-%02dT%02d:%02d:%02d.000Z", y, m, d,
                int(rest / 3600), int(rest / 60 % 60), int(rest % 60));
  return out;
}

DoneResult markDone(std::string& text, const std::string& id, int64_t nowUtc) {
  Reader r(text);
  r.skipBom();
  if (!findTopLevel(r, "tasks") || !r.beginArray()) return DoneResult::Invalid;
  while (r.nextElement()) {
    if (!r.peek('{')) {
      if (!r.skipValue()) return DoneResult::Invalid;
      continue;
    }
    TaskSpan span;
    if (!scanTaskSpan(r, span)) return DoneResult::Invalid;
    if (span.id != id) continue;
    if (span.status == "done" || span.status == "archived") return DoneResult::AlreadyDone;
    if (span.recurring) return DoneResult::Recurring;

    const std::string now = jsonString(isoTime(nowUtc));
    const std::pair<const char*, std::string> updates[] = {
        {"status", "\"done\""},
        {"completedAt", now},
        {"updatedAt", now},
        {"rev", std::to_string(span.rev + 1)},
        {"revBy", "\"paperos\""},
    };
    // Replace values that exist, from the end so earlier positions hold, and
    // add the others before the closing brace.
    std::vector<std::pair<TaskSpan::Value, std::string>> replacements;
    std::string added;
    bool hasMembers = !span.empty;
    for (const auto& update : updates) {
      bool found = false;
      for (const TaskSpan::Value& v : span.values) {
        if (v.key == update.first) {
          replacements.push_back({v, update.second});
          found = true;
        }
      }
      if (!found) {
        if (hasMembers) added += ',';
        added += jsonString(update.first) + ":" + update.second;
        hasMembers = true;
      }
    }
    text.insert(span.end - 1, added);
    std::sort(replacements.begin(), replacements.end(),
              [](const auto& a, const auto& b) { return a.first.start > b.first.start; });
    for (const auto& rep : replacements) {
      text.replace(rep.first.start, rep.first.end - rep.first.start, rep.second);
    }
    return DoneResult::Ok;
  }
  return r.ok() ? DoneResult::NotFound : DoneResult::Invalid;
}

int64_t upcomingTime(const Task& task, int64_t nowUtc) {
  const int64_t today = localMidnight(nowUtc);
  int64_t best = kNoTime;
  for (const int64_t t : {task.start, task.due}) {
    if (t == kNoTime || t < today) continue;
    if (best == kNoTime || t < best) best = t;
  }
  return best;
}

Lists buildLists(const Data& data, int64_t nowUtc, int days) {
  Lists lists;
  const bool timeKnown = nowUtc >= 0;
  const int64_t tomorrow = timeKnown ? addLocalDays(localMidnight(nowUtc), 1) : 0;
  const int64_t windowEnd = timeKnown ? addLocalDays(localMidnight(nowUtc), days + 1) : 0;
  auto started = [&](const Task& t) {
    return !timeKnown || t.start == kNoTime || t.start < tomorrow;
  };

  for (size_t i = 0; i < data.tasks.size(); ++i) {
    const Task& t = data.tasks[i];
    if (t.deleted || isFinished(t)) continue;
    if (t.focused && started(t)) {
      lists.focus.push_back(i);
    } else if (t.status == "next" && started(t)) {
      lists.next.push_back(i);
    } else if (timeKnown && (t.status == "next" || t.status == "inbox" || t.status == "waiting")) {
      const int64_t when = upcomingTime(t, nowUtc);
      if (when != kNoTime && when < windowEnd) lists.upcoming.push_back(i);
    }
  }

  const auto& tasks = data.tasks;
  std::stable_sort(lists.focus.begin(), lists.focus.end(), [&](size_t a, size_t b) {
    const Task& x = tasks[a];
    const Task& y = tasks[b];
    if (x.hasFocusOrder != y.hasFocusOrder) return x.hasFocusOrder;
    return x.hasFocusOrder && x.focusOrder < y.focusOrder;
  });
  // Earlier first, kNoTime last; 0 when equal.
  auto compareTime = [](int64_t x, int64_t y) {
    if (x == y) return 0;
    if (x == kNoTime) return 1;
    if (y == kNoTime) return -1;
    return x < y ? -1 : 1;
  };
  // Areas in their order (those with one first), then file order; no area last.
  auto areaBefore = [](const Area* x, const Area* y) {
    if (x == y) return false;
    if (x == nullptr || y == nullptr) return y == nullptr;
    if (x->hasOrder != y->hasOrder) return x->hasOrder;
    if (x->hasOrder && x->order != y->order) return x->order < y->order;
    return x->position < y->position;
  };
  std::stable_sort(lists.next.begin(), lists.next.end(), [&](size_t a, size_t b) {
    const Area* areaA = data.areaOf(tasks[a]);
    const Area* areaB = data.areaOf(tasks[b]);
    if (areaA != areaB) return areaBefore(areaA, areaB);
    if (const int c = compareTime(tasks[a].start, tasks[b].start)) return c < 0;
    return compareTime(tasks[a].due, tasks[b].due) < 0;
  });
  std::stable_sort(lists.upcoming.begin(), lists.upcoming.end(), [&](size_t a, size_t b) {
    return upcomingTime(tasks[a], nowUtc) < upcomingTime(tasks[b], nowUtc);
  });
  return lists;
}

}  // namespace mindwtr

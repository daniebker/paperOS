// Host-side tests for the calendar app. Run with: pio test -e native
#include <unity.h>

#include <Agenda.h>
#include <CalendarApp.h>
#include <CivilTime.h>
#include <Ics.h>
#include <Recurrence.h>
#include <paperos.h>

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <vector>

using namespace paperos;
using namespace calendar;

namespace {

// Berlin, which the expectations below are worked out for.
const char* const kBerlin = "CET-1CEST,M3.5.0,M10.5.0/3";

class FakeDisplay : public Display {
 public:
  int width() const override { return 540; }
  int height() const override { return 960; }
  void beginFrame(Color) override {
    texts.clear();
    filledRects = 0;
    outlinedRects = 0;
  }
  void endFrame(Refresh refresh) override { lastRefresh = refresh; }
  void fillRect(int, int, int, int, Color) override { ++filledRects; }
  void drawRect(int, int, int, int, Color) override { ++outlinedRects; }
  void drawLine(int, int, int, int, Color) override {}
  void drawText(const char* text, int, int, TextSize, TextAlign, Color) override {
    texts.emplace_back(text);
  }
  int textHeight(TextSize size) const override { return size == TextSize::Small ? 24 : 36; }
  int textWidth(const char* text, TextSize size) const override {
    return (size == TextSize::Small ? 9 : 14) * int(strlen(text));
  }

  bool shows(const std::string& text) const {
    return std::find(texts.begin(), texts.end(), text) != texts.end();
  }
  bool showsPart(const std::string& text) const {
    return std::any_of(texts.begin(), texts.end(), [&](const std::string& t) {
      return t.find(text) != std::string::npos;
    });
  }

  std::vector<std::string> texts;
  int filledRects = 0;
  int outlinedRects = 0;
  Refresh lastRefresh = Refresh::Fast;
};

class FakeBattery : public Battery {
 public:
  int level() const override { return 80; }
};

class MemoryFileSystem : public FileSystem {
 public:
  class MemoryFile : public File {
   public:
    explicit MemoryFile(std::string& data) : data_(data) {}
    size_t read(void* buffer, size_t size) override {
      const size_t count = pos_ >= data_.size() ? 0 : std::min(size, data_.size() - pos_);
      memcpy(buffer, data_.data() + pos_, count);
      pos_ += count;
      return count;
    }
    size_t write(const void* data, size_t size) override {
      data_.append(static_cast<const char*>(data), size);
      return size;
    }
    bool seek(uint32_t position) override {
      pos_ = position;
      return position <= data_.size();
    }
    uint32_t size() const override { return uint32_t(data_.size()); }

   private:
    std::string& data_;
    size_t pos_ = 0;
  };

  std::unique_ptr<File> open(const char* path, FileMode mode) override {
    if (mode == FileMode::Write) {
      files[path].clear();
    } else if (files.find(path) == files.end()) {
      return nullptr;
    }
    return std::unique_ptr<File>(new MemoryFile(files[path]));
  }
  bool list(const char* path, std::vector<DirEntry>& entries) override {
    entries.clear();
    if (dirs.count(path) == 0) return false;
    const std::string prefix = std::string(path) + "/";
    for (const auto& file : files) {
      if (file.first.compare(0, prefix.size(), prefix) != 0) continue;
      DirEntry entry;
      entry.name = file.first.substr(prefix.size());
      entry.size = uint32_t(file.second.size());
      entries.push_back(entry);
    }
    return true;
  }
  bool makeDir(const char* path) override { return dirs.insert({path, true}).second; }
  bool remove(const char* path) override { return files.erase(path) > 0; }
  bool rename(const char* from, const char* to) override {
    auto found = files.find(from);
    if (found == files.end() || files.count(to) > 0) return false;
    files[to] = found->second;
    files.erase(from);
    return true;
  }

  std::map<std::string, std::string> files;
  std::map<std::string, bool> dirs;
};

// Hands out its body a few bytes at a time, with a pause before each piece.
class FakeResponse : public HttpResponse {
 public:
  FakeResponse(const std::string& body, bool failAtEnd) : body_(body), failAtEnd_(failAtEnd) {}
  int read(void* buffer, size_t size) override {
    waiting_ = !waiting_;
    if (waiting_) return 0;
    if (pos_ >= body_.size()) return failAtEnd_ ? kError : kEnd;
    const size_t count = std::min<size_t>({size, body_.size() - pos_, 700});
    memcpy(buffer, body_.data() + pos_, count);
    pos_ += count;
    return int(count);
  }
  int64_t contentLength() const override { return int64_t(body_.size()); }

 private:
  std::string body_;
  bool failAtEnd_;
  size_t pos_ = 0;
  bool waiting_ = false;
};

class FakeNetwork : public Network {
 public:
  bool configured() const override { return isConfigured; }
  void connect() override {
    ++connects;
    state_ = NetworkState::Connecting;
    polls_ = 0;
  }
  void disconnect() override {
    ++disconnects;
    state_ = NetworkState::Off;
  }
  std::string failureDetail() const override { return joinFails ? detail : ""; }
  NetworkState state() override {
    if (state_ == NetworkState::Connecting && ++polls_ >= 3) {
      state_ = joinFails ? NetworkState::Failed : NetworkState::Connected;
    }
    return state_;
  }
  std::unique_ptr<HttpResponse> get(const char* url, std::string& error) override {
    requests.push_back(url);
    TEST_ASSERT_TRUE(state_ == NetworkState::Connected);
    auto found = bodies.find(url);
    if (found == bodies.end()) {
      error = "HTTP 404";
      return nullptr;
    }
    return std::unique_ptr<HttpResponse>(new FakeResponse(found->second, failAtEnd));
  }

  bool isConfigured = true;
  bool joinFails = false;
  std::string detail = "network not found; reason 201 NO_AP_FOUND";
  bool failAtEnd = false;
  std::map<std::string, std::string> bodies;
  std::vector<std::string> requests;
  int connects = 0;
  int disconnects = 0;

 private:
  NetworkState state_ = NetworkState::Off;
  int polls_ = 0;
};

class FakeClock : public Clock {
 public:
  int64_t now() const override { return time; }
  uint32_t uptimeMs() const override { return ms; }
  void sync() override {
    ++syncs;
    if (syncTo >= 0) time = syncTo;
  }
  int64_t time = -1;
  uint32_t ms = 0;
  int64_t syncTo = -1;
  int syncs = 0;
};

int64_t civil(int y, int mo, int d, int h = 0, int mi = 0) {
  DateTime t;
  t.year = y;
  t.month = mo;
  t.day = d;
  t.hour = h;
  t.minute = mi;
  return toCivilSeconds(t);
}

std::string format(int64_t seconds) {
  const DateTime t = fromCivilSeconds(seconds);
  char text[32];
  snprintf(text, sizeof(text), "%04d-%02d-%02d %02d:%02d", t.year, t.month, t.day, t.hour,
           t.minute);
  return text;
}

std::string expandToText(const char* ruleText, int64_t start, int64_t first, int64_t last,
                         int64_t until = INT64_MAX) {
  RRule rule;
  parseRRule(ruleText, rule);
  std::string out;
  expandRule(rule, start, until, first, last, [&](int64_t t) {
    if (t >= first) out += (out.empty() ? "" : ", ") + format(t);
    return true;
  });
  return out;
}

class EventList : public IcsParser::Sink {
 public:
  void onEvent(const IcsEvent& event) override { events.push_back(event); }
  void onTimeZone(const IcsTimeZone& zone) override { zones.push_back(zone); }
  std::vector<IcsEvent> events;
  std::vector<IcsTimeZone> zones;
};

std::vector<Occurrence> buildAgenda(const std::string& ics, int64_t windowStart, int days) {
  AgendaBuilder builder(windowStart, windowStart + days * kSecondsPerDay);
  builder.beginCalendar(0);
  IcsParser parser(builder);
  parser.feed(ics.data(), ics.size());
  parser.finish();
  builder.endCalendar();
  return builder.take();
}

std::string describe(const std::vector<Occurrence>& list) {
  std::string out;
  for (const Occurrence& o : list) {
    if (!out.empty()) out += "\n";
    out += format(o.start) + " - " + format(o.end) + (o.allDay ? " (all day) " : " ") + o.summary;
  }
  return out;
}

const char* const kNewYorkZone =
    "BEGIN:VTIMEZONE\r\n"
    "TZID:America/New_York\r\n"
    "BEGIN:DAYLIGHT\r\n"
    "TZOFFSETFROM:-0500\r\n"
    "TZOFFSETTO:-0400\r\n"
    "DTSTART:20070311T020000\r\n"
    "RRULE:FREQ=YEARLY;BYMONTH=3;BYDAY=2SU\r\n"
    "END:DAYLIGHT\r\n"
    "BEGIN:STANDARD\r\n"
    "TZOFFSETFROM:-0400\r\n"
    "TZOFFSETTO:-0500\r\n"
    "DTSTART:20071104T020000\r\n"
    "RRULE:FREQ=YEARLY;BYMONTH=11;BYDAY=1SU\r\n"
    "END:STANDARD\r\n"
    "END:VTIMEZONE\r\n";

// A week of events from Monday 9 November 2026, in Berlin.
std::string sampleCalendar() {
  return std::string("BEGIN:VCALENDAR\r\nVERSION:2.0\r\n") + kNewYorkZone +
         "BEGIN:VEVENT\r\n"
         "UID:standup\r\n"
         "SUMMARY:Standup\r\n"
         "DTSTART;TZID=Europe/Berlin:20261102T093000\r\n"
         "DTEND;TZID=Europe/Berlin:20261102T094500\r\n"
         "RRULE:FREQ=DAILY;BYDAY=MO,TU,WE,TH,FR\r\n"
         "EXDATE;TZID=Europe/Berlin:20261111T093000\r\n"
         "END:VEVENT\r\n"
         "BEGIN:VEVENT\r\n"
         "UID:standup\r\n"
         "RECURRENCE-ID;TZID=Europe/Berlin:20261112T093000\r\n"
         "SUMMARY:Standup (moved)\r\n"
         "DTSTART;TZID=Europe/Berlin:20261112T113000\r\n"
         "DTEND;TZID=Europe/Berlin:20261112T114500\r\n"
         "END:VEVENT\r\n"
         "BEGIN:VEVENT\r\n"
         "UID:standup\r\n"
         "RECURRENCE-ID;TZID=Europe/Berlin:20261113T093000\r\n"
         "STATUS:CANCELLED\r\n"
         "SUMMARY:Standup\r\n"
         "DTSTART;TZID=Europe/Berlin:20261113T093000\r\n"
         "END:VEVENT\r\n"
         "BEGIN:VEVENT\r\n"
         "UID:ny\r\n"
         "SUMMARY:New York call\r\n"
         "DTSTART;TZID=America/New_York:20261110T090000\r\n"
         "DTEND;TZID=America/New_York:20261110T100000\r\n"
         "END:VEVENT\r\n"
         "BEGIN:VEVENT\r\n"
         "UID:dentist\r\n"
         "SUMMARY:Caf\xC3\xA9 with\r\n"
         "  Anna\r\n"
         "LOCATION:Main St. 4\\, Berlin\r\n"
         "DTSTART:20261111T120000Z\r\n"
         "DURATION:PT30M\r\n"
         "BEGIN:VALARM\r\n"
         "ACTION:DISPLAY\r\n"
         "SUMMARY:Alarm text\r\n"
         "TRIGGER:-PT15M\r\n"
         "END:VALARM\r\n"
         "END:VEVENT\r\n"
         "BEGIN:VEVENT\r\n"
         "UID:trip\r\n"
         "SUMMARY:Trip\r\n"
         "DTSTART;VALUE=DATE:20261112\r\n"
         "DTEND;VALUE=DATE:20261114\r\n"
         "END:VEVENT\r\n"
         "BEGIN:VEVENT\r\n"
         "UID:called-off\r\n"
         "SUMMARY:Called off\r\n"
         "STATUS:CANCELLED\r\n"
         "DTSTART:20261110T120000Z\r\n"
         "END:VEVENT\r\n"
         "BEGIN:VEVENT\r\n"
         "UID:later\r\n"
         "SUMMARY:Later\r\n"
         "DTSTART:20261201T120000Z\r\n"
         "END:VEVENT\r\n"
         "END:VCALENDAR\r\n";
}

// 2026-11-09 07:00 UTC, 08:00 in Berlin on a Monday.
const int64_t kMonday8am = civil(2026, 11, 9, 7, 0);
const char* const kFeedUrl = "https://example.com/home.ics";
const char* const kOneFeed =
    "[calendar]\n"
    "  days = 7\n"
    "[[calendar.feed]]\n"
    "  name = \"Home\"\n"
    "  url = \"https://example.com/home.ics\"\n";

struct AppFixture {
  explicit AppFixture(const char* configText) {
    config.parse(configText);
    clock.time = kMonday8am;
    network.bodies[kFeedUrl] = sampleCalendar();
  }

  // Ticks, letting time pass, until the app has nothing left to do.
  int runUntilIdle(int maxTicks = 20000) {
    int redraws = 0;
    for (int i = 0; i < maxTicks; ++i) {
      if (app.phase() == CalendarApp::Phase::Idle) return redraws;
      clock.ms += 10;
      if (app.tick()) ++redraws;
    }
    TEST_FAIL_MESSAGE("app never became idle");
    return redraws;
  }

  void draw() {
    display.beginFrame(Color::White);
    app.draw(display);
  }

  std::string allPagesText() {
    std::string out;
    for (size_t p = 0; p < app.pageCount(); ++p) {
      for (const CalendarApp::Row& row : app.pageRows(p)) {
        out += row.time + "|" + row.text + "|" + row.detail + "\n";
      }
    }
    return out;
  }

  Config config;
  MemoryFileSystem fs;
  FakeNetwork network;
  FakeClock clock;
  FakeDisplay display;
  FakeBattery battery;
  CalendarApp app{config, fs, network, clock, display, battery};
};

}  // namespace

void setUp() {
  setenv("TZ", kBerlin, 1);
  tzset();
}
void tearDown() {}

// --- Values ----------------------------------------------------------------

void test_civil_dates_round_trip() {
  TEST_ASSERT_EQUAL_INT(0, (int)daysFromCivil(1970, 1, 1));
  TEST_ASSERT_EQUAL_INT(-1, (int)daysFromCivil(1969, 12, 31));
  TEST_ASSERT_EQUAL_INT(6, weekdayOfDays(daysFromCivil(2026, 11, 8)));  // Sunday
  TEST_ASSERT_EQUAL_INT(29, daysInMonth(2024, 2));
  TEST_ASSERT_EQUAL_INT(28, daysInMonth(2100, 2));
  TEST_ASSERT_EQUAL_INT((int)daysFromCivil(2027, 1, 1), (int)daysFromCivil(2026, 13, 1));
  TEST_ASSERT_EQUAL_STRING("1969-12-31 23:59", format(-60).c_str());
  TEST_ASSERT_EQUAL_STRING("2026-11-09 07:00", format(kMonday8am).c_str());
  TEST_ASSERT_EQUAL_INT(1794207600, (int)kMonday8am);  // equals Unix time
}

void test_local_time_follows_tz() {
  TEST_ASSERT_EQUAL_STRING("2026-11-09 08:00", format(utcToLocal(kMonday8am)).c_str());
  TEST_ASSERT_EQUAL_STRING("2026-07-01 14:00",
                           format(utcToLocal(civil(2026, 7, 1, 12))).c_str());  // summer
}

void test_parses_ics_values() {
  IcsTime t;
  TEST_ASSERT_TRUE(parseIcsTime("20261108", "", t));
  TEST_ASSERT_TRUE(t.dateOnly);
  TEST_ASSERT_EQUAL_STRING("2026-11-08 00:00", format(t.civil).c_str());
  TEST_ASSERT_TRUE(parseIcsTime("20261108T093015Z", ";TZID=Ignored", t));
  TEST_ASSERT_TRUE(t.utc);
  TEST_ASSERT_TRUE(t.tzid.empty());
  TEST_ASSERT_TRUE(parseIcsTime("20261108T093000", ";X-A=\"a;b\";TZID=\"Europe/Berlin\"", t));
  TEST_ASSERT_FALSE(t.utc);
  TEST_ASSERT_EQUAL_STRING("Europe/Berlin", t.tzid.c_str());
  TEST_ASSERT_FALSE(parseIcsTime("20261308", "", t));
  TEST_ASSERT_FALSE(parseIcsTime("20261108T25", "", t));
  TEST_ASSERT_FALSE(parseIcsTime("garbage", "", t));

  int64_t seconds = 0;
  TEST_ASSERT_TRUE(parseIcsDuration("PT1H30M", seconds));
  TEST_ASSERT_EQUAL_INT(5400, (int)seconds);
  TEST_ASSERT_TRUE(parseIcsDuration("P1DT2S", seconds));
  TEST_ASSERT_EQUAL_INT(86402, (int)seconds);
  TEST_ASSERT_TRUE(parseIcsDuration("-P2W", seconds));
  TEST_ASSERT_EQUAL_INT(-14 * 86400, (int)seconds);
  TEST_ASSERT_FALSE(parseIcsDuration("P1H", seconds));
  TEST_ASSERT_FALSE(parseIcsDuration("P", seconds));

  int offset = 0;
  TEST_ASSERT_TRUE(parseIcsUtcOffset("+0100", offset));
  TEST_ASSERT_EQUAL_INT(3600, offset);
  TEST_ASSERT_TRUE(parseIcsUtcOffset("-0530", offset));
  TEST_ASSERT_EQUAL_INT(-19800, offset);
  TEST_ASSERT_FALSE(parseIcsUtcOffset("0100", offset));

  TEST_ASSERT_EQUAL_STRING("a, b; c\\ d e", unescapeIcsText("a\\, b\\; c\\\\ d\\ne").c_str());
}

void test_parses_rrules() {
  RRule rule;
  TEST_ASSERT_TRUE(parseRRule("FREQ=MONTHLY;INTERVAL=2;BYDAY=-1FR,2MO,SU;COUNT=6;WKST=SU", rule));
  TEST_ASSERT_EQUAL_INT((int)RRule::Freq::Monthly, (int)rule.freq);
  TEST_ASSERT_EQUAL_INT(2, rule.interval);
  TEST_ASSERT_EQUAL_INT(6, rule.count);
  TEST_ASSERT_EQUAL_INT(6, rule.weekStart);
  TEST_ASSERT_EQUAL_INT(3, (int)rule.byDay.size());
  TEST_ASSERT_EQUAL_INT(-1, rule.byDay[0].ordinal);
  TEST_ASSERT_EQUAL_INT(4, rule.byDay[0].weekday);
  TEST_ASSERT_EQUAL_INT(2, rule.byDay[1].ordinal);
  TEST_ASSERT_EQUAL_INT(0, rule.byDay[2].ordinal);

  TEST_ASSERT_TRUE(parseRRule("freq=yearly;until=20301231;bymonth=2,3", rule));
  TEST_ASSERT_TRUE(rule.until.valid);
  TEST_ASSERT_EQUAL_INT(2, (int)rule.byMonth.size());

  TEST_ASSERT_FALSE(parseRRule("FREQ=HOURLY", rule));
  TEST_ASSERT_EQUAL_INT((int)RRule::Freq::None, (int)rule.freq);
}

// --- Recurrence ------------------------------------------------------------
// Expected dates come from python-dateutil's rrule.

void test_weekly_rule_with_days_and_count() {
  TEST_ASSERT_EQUAL_STRING(
      "2026-11-02 09:00, 2026-11-04 09:00, 2026-11-06 09:00, 2026-11-09 09:00, 2026-11-11 09:00",
      expandToText("FREQ=WEEKLY;BYDAY=MO,WE,FR;COUNT=5", civil(2026, 11, 2, 9), 0,
                   civil(2027, 1, 1))
          .c_str());
}

void test_every_other_week_until() {
  TEST_ASSERT_EQUAL_STRING(
      "2026-11-03 10:00, 2026-11-17 10:00, 2026-12-01 10:00, 2026-12-15 10:00",
      expandToText("FREQ=WEEKLY;INTERVAL=2;BYDAY=TU", civil(2026, 11, 3, 10), 0,
                   civil(2027, 6, 1), civil(2026, 12, 15, 23, 59))
          .c_str());
}

void test_monthly_rules() {
  TEST_ASSERT_EQUAL_STRING(
      "2026-01-30 18:00, 2026-02-27 18:00, 2026-03-27 18:00, 2026-04-24 18:00, "
      "2026-05-29 18:00, 2026-06-26 18:00",
      expandToText("FREQ=MONTHLY;BYDAY=-1FR", civil(2026, 1, 30, 18), 0, civil(2026, 6, 30))
          .c_str());
  TEST_ASSERT_EQUAL_STRING(
      "2026-01-31 12:00, 2026-02-28 12:00, 2026-03-31 12:00, 2026-04-30 12:00",
      expandToText("FREQ=MONTHLY;BYMONTHDAY=-1;COUNT=4", civil(2026, 1, 31, 12), 0,
                   civil(2030, 1, 1))
          .c_str());
  TEST_ASSERT_EQUAL_STRING(
      "2026-01-30 17:00, 2026-02-27 17:00, 2026-03-31 17:00, 2026-04-30 17:00, 2026-05-29 17:00",
      expandToText("FREQ=MONTHLY;BYDAY=MO,TU,WE,TH,FR;BYSETPOS=-1", civil(2026, 1, 30, 17), 0,
                   civil(2026, 5, 31))
          .c_str());
  TEST_ASSERT_EQUAL_STRING(
      "2026-11-10 19:00, 2026-12-08 19:00, 2027-01-12 19:00",
      expandToText("FREQ=MONTHLY;BYDAY=2TU;COUNT=3", civil(2026, 11, 10, 19), 0,
                   civil(2030, 1, 1))
          .c_str());
  // Months without a 31st are skipped.
  TEST_ASSERT_EQUAL_STRING(
      "2026-01-31 00:00, 2026-03-31 00:00, 2026-05-31 00:00, 2026-07-31 00:00, 2026-08-31 00:00",
      expandToText("FREQ=MONTHLY", civil(2026, 1, 31), 0, civil(2026, 8, 31)).c_str());
}

void test_yearly_rule_on_a_leap_day() {
  TEST_ASSERT_EQUAL_STRING(
      "2024-02-29 00:00, 2028-02-29 00:00, 2032-02-29 00:00",
      expandToText("FREQ=YEARLY", civil(2024, 2, 29), 0, civil(2032, 12, 31)).c_str());
}

void test_long_running_daily_rule_skips_ahead() {
  RRule rule;
  parseRRule("FREQ=DAILY", rule);
  const int64_t first = civil(2026, 11, 8);
  int visits = 0;
  std::string seen;
  expandRule(rule, civil(2010, 1, 1, 7, 30), INT64_MAX, first, civil(2026, 11, 10, 23, 59),
             [&](int64_t t) {
               ++visits;
               if (t >= first) seen += format(t) + ";";
               return true;
             });
  TEST_ASSERT_EQUAL_STRING("2026-11-08 07:30;2026-11-09 07:30;2026-11-10 07:30;", seen.c_str());
  TEST_ASSERT_LESS_THAN_INT(6, visits);
}

void test_time_zone_onsets() {
  RRule rule;
  parseRRule("FREQ=YEARLY;BYMONTH=3;BYDAY=-1SU", rule);
  TEST_ASSERT_EQUAL_STRING("2026-03-29 02:00",
                           format(yearlyOnset(rule, civil(1970, 3, 29, 2), 2026)).c_str());
  parseRRule("FREQ=YEARLY;BYMONTH=11;BYDAY=1SU", rule);
  TEST_ASSERT_EQUAL_STRING("2026-11-01 02:00",
                           format(yearlyOnset(rule, civil(2007, 11, 4, 2), 2026)).c_str());
  TEST_ASSERT_TRUE(yearlyOnset(rule, civil(2007, 11, 4, 2), 2006) == INT64_MIN);
}

// --- Parsing and the agenda ------------------------------------------------

void test_parser_reads_events_in_any_piece_sizes() {
  const std::string ics = sampleCalendar();
  for (size_t piece : {size_t(1), size_t(7), ics.size()}) {
    EventList list;
    IcsParser parser(list);
    for (size_t i = 0; i < ics.size(); i += piece) {
      parser.feed(ics.data() + i, std::min(piece, ics.size() - i));
    }
    parser.finish();
    TEST_ASSERT_TRUE(parser.sawCalendar());
    TEST_ASSERT_EQUAL_INT(8, (int)list.events.size());
    TEST_ASSERT_EQUAL_INT(1, (int)list.zones.size());
    TEST_ASSERT_EQUAL_INT(2, (int)list.zones[0].observances.size());
    const IcsEvent& dentist = list.events[4];
    TEST_ASSERT_EQUAL_STRING("Caf\xC3\xA9 with Anna", dentist.summary.c_str());  // unfolded
    TEST_ASSERT_EQUAL_STRING("Main St. 4, Berlin", dentist.location.c_str());
    TEST_ASSERT_EQUAL_INT(1800, (int)dentist.duration);
    TEST_ASSERT_EQUAL_INT(1, (int)list.events[0].exceptions.size());
    TEST_ASSERT_TRUE(list.events[2].cancelled);
  }
}

void test_parser_handles_lf_endings_and_a_missing_last_newline() {
  const std::string ics =
      "BEGIN:VCALENDAR\nBEGIN:VEVENT\nSUMMARY:A\nDTSTART:20261109T100000Z\nEND:VEVENT\n"
      "BEGIN:VEVENT\nSUMMARY:No start\nEND:VEVENT\nEND:VCALENDAR";
  EventList list;
  IcsParser parser(list);
  parser.feed(ics.data(), ics.size());
  parser.finish();
  TEST_ASSERT_EQUAL_INT(1, (int)list.events.size());  // events need a DTSTART
  TEST_ASSERT_EQUAL_STRING("A", list.events[0].summary.c_str());

  EventList page;
  IcsParser html(page);
  const std::string text = "<html><body>Sign in</body></html>";
  html.feed(text.data(), text.size());
  html.finish();
  TEST_ASSERT_FALSE(html.sawCalendar());
}

void test_agenda_expands_and_converts_to_local_time() {
  const std::vector<Occurrence> list = buildAgenda(sampleCalendar(), civil(2026, 11, 9), 7);
  TEST_ASSERT_EQUAL_STRING(
      "2026-11-09 09:30 - 2026-11-09 09:45 Standup\n"
      "2026-11-10 09:30 - 2026-11-10 09:45 Standup\n"
      "2026-11-10 15:00 - 2026-11-10 16:00 New York call\n"
      "2026-11-11 13:00 - 2026-11-11 13:30 Cafe with Anna\n"
      "2026-11-12 00:00 - 2026-11-14 00:00 (all day) Trip\n"
      "2026-11-12 11:30 - 2026-11-12 11:45 Standup (moved)",
      describe(list).c_str());
  TEST_ASSERT_EQUAL_STRING("Main St. 4, Berlin", list[3].location.c_str());
}

void test_agenda_uses_the_calendars_time_zone_rules() {
  AgendaBuilder builder(0, 1);
  IcsParser parser(builder);
  builder.beginCalendar(0);
  const std::string zone = kNewYorkZone;
  parser.feed(zone.data(), zone.size());
  parser.finish();

  IcsTime t;
  parseIcsTime("20261028T090000", ";TZID=America/New_York", t);  // EDT, Berlin back on CET
  TEST_ASSERT_EQUAL_STRING("2026-10-28 14:00", format(builder.toLocal(t)).c_str());
  parseIcsTime("20260701T090000", ";TZID=America/New_York", t);  // both on summer time
  TEST_ASSERT_EQUAL_STRING("2026-07-01 15:00", format(builder.toLocal(t)).c_str());
  parseIcsTime("20260701T090000", ";TZID=Asia/Unknown", t);  // assumed to be local
  TEST_ASSERT_EQUAL_STRING("2026-07-01 09:00", format(builder.toLocal(t)).c_str());
  parseIcsTime("20260701T090000", ";TZID=UTC", t);
  TEST_ASSERT_EQUAL_STRING("2026-07-01 11:00", format(builder.toLocal(t)).c_str());
}

void test_weekly_event_keeps_its_local_time_across_dst() {
  // 10:00 New York every Tuesday: 16:00 in Berlin, but 15:00 in the week
  // between Europe and the US leaving summer time.
  const std::string ics = std::string("BEGIN:VCALENDAR\r\n") + kNewYorkZone +
                          "BEGIN:VEVENT\r\nUID:w\r\nSUMMARY:Weekly\r\n"
                          "DTSTART;TZID=America/New_York:20261020T100000\r\n"
                          "RRULE:FREQ=WEEKLY;UNTIL=20261104T000000Z\r\n"
                          "END:VEVENT\r\nEND:VCALENDAR\r\n";
  TEST_ASSERT_EQUAL_STRING(
      "2026-10-20 16:00 - 2026-10-20 16:00 Weekly\n"
      "2026-10-27 15:00 - 2026-10-27 15:00 Weekly\n"
      "2026-11-03 16:00 - 2026-11-03 16:00 Weekly",
      describe(buildAgenda(ics, civil(2026, 10, 19), 30)).c_str());
}

void test_agenda_keeps_events_that_started_before_the_window() {
  const std::string ics =
      "BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nSUMMARY:Holiday\r\n"
      "DTSTART;VALUE=DATE:20261101\r\nDTEND;VALUE=DATE:20261111\r\nEND:VEVENT\r\n"
      "BEGIN:VEVENT\r\nSUMMARY:Ended\r\n"
      "DTSTART;VALUE=DATE:20261101\r\nDTEND;VALUE=DATE:20261109\r\nEND:VEVENT\r\n"
      "END:VCALENDAR\r\n";
  TEST_ASSERT_EQUAL_STRING("2026-11-01 00:00 - 2026-11-11 00:00 (all day) Holiday",
                           describe(buildAgenda(ics, civil(2026, 11, 9), 7)).c_str());
}

// --- The app ---------------------------------------------------------------

void test_cache_path_is_stable() {
  TEST_ASSERT_EQUAL_STRING("/.cache/calendar/131b39c8.ics",
                           CalendarApp::cachePath("https://example.com/home.ics").c_str());
}

void test_app_downloads_and_shows_the_agenda() {
  AppFixture f(kOneFeed);
  f.app.onOpen();
  TEST_ASSERT_TRUE(f.runUntilIdle() > 0);

  TEST_ASSERT_EQUAL_INT(1, (int)f.network.requests.size());
  TEST_ASSERT_EQUAL_STRING(kFeedUrl, f.network.requests[0].c_str());
  TEST_ASSERT_EQUAL_INT(1, f.network.disconnects);
  TEST_ASSERT_EQUAL_STRING(sampleCalendar().c_str(),
                           f.fs.files[CalendarApp::cachePath(kFeedUrl)].c_str());
  TEST_ASSERT_EQUAL_STRING(std::to_string(kMonday8am).c_str(), f.fs.files["/.cache/calendar/synced"].c_str());
  TEST_ASSERT_EQUAL_INT(6, (int)f.app.occurrences().size());

  TEST_ASSERT_EQUAL_STRING(
      "|Today, Mon 9 Nov|\n"
      "09:30 - 09:45|Standup|\n"
      "|Tomorrow, Tue 10 Nov|\n"
      "09:30 - 09:45|Standup|\n"
      "15:00 - 16:00|New York call|\n"
      "|Wed 11 Nov|\n"
      "13:00 - 13:30|Cafe with Anna|Main St. 4, Berlin\n"
      "|Thu 12 Nov|\n"
      "All day|Trip|\n"
      "11:30 - 11:45|Standup (moved)|\n"
      "|Fri 13 Nov|\n"
      "All day|Trip|\n",
      f.allPagesText().c_str());

  f.draw();
  TEST_ASSERT_TRUE(f.display.shows("Calendar"));
  TEST_ASSERT_TRUE(f.display.shows("Today, Mon 9 Nov"));
  TEST_ASSERT_TRUE(f.display.shows("Updated 08:00"));
  TEST_ASSERT_TRUE(f.display.shows("80%"));
}

void test_app_shows_the_cached_copy_without_downloading_when_fresh() {
  AppFixture f(kOneFeed);
  f.fs.files[CalendarApp::cachePath(kFeedUrl)] = sampleCalendar();
  f.fs.files["/.cache/calendar/synced"] = std::to_string(kMonday8am - 10 * 60);
  f.app.onOpen();
  f.runUntilIdle();
  TEST_ASSERT_EQUAL_INT(0, f.network.connects);
  TEST_ASSERT_EQUAL_INT(6, (int)f.app.occurrences().size());
  f.draw();
  TEST_ASSERT_TRUE(f.display.shows("Updated 07:50"));
}

void test_app_downloads_again_when_the_copy_is_stale() {
  AppFixture f("[calendar]\nrefresh_minutes = 30\n"
               "[[calendar.feed]]\nurl = \"https://example.com/home.ics\"\n");
  f.fs.files[CalendarApp::cachePath(kFeedUrl)] = sampleCalendar();
  f.fs.files["/.cache/calendar/synced"] = std::to_string(kMonday8am - 31 * 60);
  f.app.onOpen();
  f.runUntilIdle();
  TEST_ASSERT_EQUAL_INT(1, (int)f.network.requests.size());

  // Not again until the interval has passed.
  f.clock.ms += 29 * 60 * 1000;
  f.clock.time += 29 * 60;
  f.app.tick();
  TEST_ASSERT_TRUE(f.app.phase() == CalendarApp::Phase::Idle);
  f.clock.ms += 60 * 1000;
  f.clock.time += 60;
  TEST_ASSERT_TRUE(f.app.tick());
  f.runUntilIdle();
  TEST_ASSERT_EQUAL_INT(2, (int)f.network.requests.size());
}

void test_failed_download_keeps_the_old_copy() {
  AppFixture f(kOneFeed);
  const std::string path = CalendarApp::cachePath(kFeedUrl);
  f.fs.files[path] = sampleCalendar();
  f.fs.files["/.cache/calendar/synced"] = std::to_string(kMonday8am - 60);
  f.app.onOpen();
  f.runUntilIdle();

  f.network.bodies[kFeedUrl] = "<html>Please sign in</html>";
  TEST_ASSERT_TRUE(f.app.onInput(InputEvent::Select));  // download now
  f.runUntilIdle();
  TEST_ASSERT_EQUAL_INT(1, (int)f.network.requests.size());
  TEST_ASSERT_EQUAL_STRING(sampleCalendar().c_str(), f.fs.files[path].c_str());
  TEST_ASSERT_EQUAL_INT(0, (int)f.fs.files.count(path + ".part"));
  TEST_ASSERT_EQUAL_INT(6, (int)f.app.occurrences().size());
  TEST_ASSERT_EQUAL_STRING("Home: not an iCal file, check the url", f.app.statusText().c_str());

  f.network.bodies[kFeedUrl] = sampleCalendar();
  f.network.failAtEnd = true;
  f.app.onInput(InputEvent::Select);
  f.runUntilIdle();
  TEST_ASSERT_EQUAL_STRING("Home: download interrupted", f.app.statusText().c_str());
  TEST_ASSERT_EQUAL_STRING(sampleCalendar().c_str(), f.fs.files[path].c_str());

  f.network.bodies.clear();
  f.app.onInput(InputEvent::Select);
  f.runUntilIdle();
  TEST_ASSERT_EQUAL_STRING("Home: HTTP 404", f.app.statusText().c_str());
}

void test_app_reports_wifi_failure() {
  AppFixture f(kOneFeed);
  f.network.joinFails = true;
  f.app.onOpen();
  f.runUntilIdle();
  TEST_ASSERT_EQUAL_INT(0, (int)f.network.requests.size());
  TEST_ASSERT_EQUAL_INT(1, f.network.disconnects);
  f.draw();
  TEST_ASSERT_TRUE(f.display.shows("Could not connect to Wi-Fi"));
  TEST_ASSERT_TRUE(f.display.shows("Today, Mon 9 Nov"));
  TEST_ASSERT_TRUE(f.display.shows("No events"));
}

void test_debug_shows_why_wifi_failed() {
  // Off by default: no detail.
  {
    AppFixture f(kOneFeed);
    f.network.joinFails = true;
    f.app.onOpen();
    f.runUntilIdle();
    TEST_ASSERT_FALSE(f.app.debug());
    TEST_ASSERT_EQUAL_STRING("", f.app.wifiFailureDetail().c_str());
    f.draw();
    TEST_ASSERT_FALSE(f.display.showsPart("NO_AP_FOUND"));
  }

  const std::string debugConfig = std::string(kOneFeed) + "[Developer]\ndebug = \"true\"\n";
  AppFixture f(debugConfig.c_str());
  f.network.joinFails = true;
  f.network.detail = "\"Home\": network not found; reason 201 NO_AP_FOUND; after 20.0s; "
                     "MAC 24:0A:C4:00:00:01";
  f.app.onOpen();
  f.runUntilIdle();
  TEST_ASSERT_TRUE(f.app.debug());
  TEST_ASSERT_EQUAL_STRING(f.network.detail.c_str(), f.app.wifiFailureDetail().c_str());
  f.draw();
  TEST_ASSERT_TRUE(f.display.shows("Could not connect to Wi-Fi"));
  TEST_ASSERT_TRUE(f.display.showsPart("Wi-Fi: \"Home\": network not found"));
  TEST_ASSERT_TRUE(f.display.showsPart("NO_AP_FOUND"));
  TEST_ASSERT_TRUE(f.display.showsPart("24:0A:C4:00:00:01"));
  TEST_ASSERT_TRUE(f.display.shows("Today, Mon 9 Nov"));

  // A successful retry clears it.
  f.network.joinFails = false;
  TEST_ASSERT_TRUE(f.app.onInput(InputEvent::Select));
  f.runUntilIdle();
  TEST_ASSERT_EQUAL_STRING("", f.app.wifiFailureDetail().c_str());
  f.draw();
  TEST_ASSERT_FALSE(f.display.showsPart("NO_AP_FOUND"));
}

void test_app_gets_the_time_before_downloading() {
  AppFixture f(kOneFeed);
  f.clock.time = -1;
  f.clock.syncTo = kMonday8am;
  f.app.onOpen();
  f.draw();
  TEST_ASSERT_TRUE(f.display.shows("Date and time unknown"));
  TEST_ASSERT_TRUE(f.display.shows("Updating..."));
  f.runUntilIdle();
  TEST_ASSERT_EQUAL_INT(1, f.clock.syncs);
  TEST_ASSERT_EQUAL_INT(6, (int)f.app.occurrences().size());
}

void test_app_without_wifi_or_feeds_explains_what_to_set() {
  AppFixture none("");
  none.app.onOpen();
  none.draw();
  TEST_ASSERT_TRUE(none.display.shows("No calendars set up"));
  TEST_ASSERT_FALSE(none.app.onInput(InputEvent::Select));

  AppFixture f(kOneFeed);
  f.network.isConfigured = false;
  f.clock.time = -1;
  f.app.onOpen();
  TEST_ASSERT_TRUE(f.app.phase() == CalendarApp::Phase::Idle);
  f.draw();
  TEST_ASSERT_TRUE(f.display.shows("Set [wifi] in config.toml to get them"));
  TEST_ASSERT_TRUE(f.app.onInput(InputEvent::Select));
  TEST_ASSERT_EQUAL_STRING("Set [wifi] in config.toml to download", f.app.statusText().c_str());
  TEST_ASSERT_EQUAL_INT(0, f.network.connects);
}

void test_app_pages_through_a_busy_agenda() {
  std::string ics = "BEGIN:VCALENDAR\r\n";
  for (int i = 0; i < 30; ++i) {
    char event[200];
    snprintf(event, sizeof(event),
             "BEGIN:VEVENT\r\nSUMMARY:Meeting %02d\r\nLOCATION:Room %d\r\n"
             "DTSTART:20261110T%02d%02d00Z\r\nEND:VEVENT\r\n",
             i, i, 6 + i / 4, (i % 4) * 15);
    ics += event;
  }
  ics += "END:VCALENDAR\r\n";
  AppFixture f(kOneFeed);
  f.network.bodies[kFeedUrl] = ics;
  f.app.onOpen();
  f.runUntilIdle();

  TEST_ASSERT_TRUE(f.app.pageCount() > 1);
  TEST_ASSERT_FALSE(f.app.onInput(InputEvent::Previous));
  TEST_ASSERT_TRUE(f.app.onInput(InputEvent::Next));
  TEST_ASSERT_EQUAL_INT(1, (int)f.app.currentPage());
  // A day carried onto a new page repeats its heading.
  const std::vector<CalendarApp::Row>& second = f.app.pageRows(1);
  TEST_ASSERT_TRUE(second[0].kind == CalendarApp::Row::Kind::Day);
  TEST_ASSERT_EQUAL_STRING("Tomorrow, Tue 10 Nov", second[0].text.c_str());
  f.draw();
  TEST_ASSERT_TRUE(f.display.shows("2 / " + std::to_string(f.app.pageCount())));

  while (f.app.onInput(InputEvent::Next)) {
  }
  TEST_ASSERT_EQUAL_INT((int)f.app.pageCount() - 1, (int)f.app.currentPage());
  size_t events = 0;
  for (size_t p = 0; p < f.app.pageCount(); ++p) {
    for (const CalendarApp::Row& row : f.app.pageRows(p)) {
      events += row.kind == CalendarApp::Row::Kind::Event;
    }
  }
  TEST_ASSERT_EQUAL_INT(30, (int)events);
}

void test_app_moves_on_at_midnight() {
  AppFixture f(kOneFeed);
  f.app.onOpen();
  f.runUntilIdle();
  TEST_ASSERT_EQUAL_STRING("Today, Mon 9 Nov", f.app.pageRows(0)[0].text.c_str());

  f.clock.time += 17 * 3600;  // 01:00 on Tuesday
  f.clock.ms += 30 * 1000;
  f.app.tick();
  TEST_ASSERT_TRUE(f.app.phase() == CalendarApp::Phase::Loading);
  f.runUntilIdle();
  TEST_ASSERT_EQUAL_STRING("Today, Tue 10 Nov", f.app.pageRows(0)[0].text.c_str());
  TEST_ASSERT_EQUAL_INT(1, (int)f.network.requests.size());  // the copy is still fresh
}

void test_app_shows_which_calendar_an_event_is_from() {
  AppFixture f(
      "[[calendar.feed]]\nname = \"Home\"\nurl = \"https://example.com/home.ics\"\n"
      "[[calendar.feed]]\nname = \"Work\"\nurl = \"https://example.com/work.ics\"\n");
  f.network.bodies["https://example.com/work.ics"] =
      "BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nSUMMARY:Review\r\n"
      "DTSTART:20261109T100000Z\r\nDTEND:20261109T110000Z\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
  // A copy from a feed that is no longer configured is cleaned up.
  f.fs.files["/.cache/calendar/0badf00d.ics"] = "old";
  f.app.onOpen();
  f.runUntilIdle();
  TEST_ASSERT_EQUAL_INT(2, (int)f.network.requests.size());
  TEST_ASSERT_EQUAL_INT(0, (int)f.fs.files.count("/.cache/calendar/0badf00d.ics"));

  const std::string text = f.allPagesText();
  TEST_ASSERT_TRUE(text.find("09:30 - 09:45|Standup|Home\n") != std::string::npos);
  TEST_ASSERT_TRUE(text.find("11:00 - 12:00|Review|Work\n") != std::string::npos);
  TEST_ASSERT_TRUE(text.find("|Cafe with Anna|Main St. 4, Berlin - Home\n") != std::string::npos);
}

void test_header_shows_wifi_while_connected() {
  AppFixture f(kOneFeed);
  f.app.onOpen();
  // Loads the (empty) cache, then starts connecting.
  while (f.app.phase() == CalendarApp::Phase::Loading) f.app.tick();
  TEST_ASSERT_TRUE(f.app.wifiState() == NetworkState::Connecting);
  f.draw();
  TEST_ASSERT_EQUAL_INT(3, f.display.outlinedRects);

  // Joining the network asks for a redraw, which shows the bars filled.
  bool redrawn = false;
  while (f.app.phase() == CalendarApp::Phase::Connecting) redrawn = f.app.tick() || redrawn;
  TEST_ASSERT_TRUE(redrawn);
  TEST_ASSERT_TRUE(f.app.wifiState() == NetworkState::Connected);
  f.draw();
  TEST_ASSERT_EQUAL_INT(0, f.display.outlinedRects);

  // Wi-Fi is off again after the download, and so is the icon.
  f.runUntilIdle();
  TEST_ASSERT_TRUE(f.app.wifiState() == NetworkState::Off);
}

void test_closing_mid_download_cleans_up() {
  AppFixture f(kOneFeed);
  f.app.onOpen();
  while (f.app.phase() != CalendarApp::Phase::Downloading) {
    f.clock.ms += 10;
    f.app.tick();
  }
  f.app.tick();
  f.app.tick();
  f.app.onClose();
  TEST_ASSERT_TRUE(f.app.phase() == CalendarApp::Phase::Idle);
  TEST_ASSERT_EQUAL_INT(1, f.network.disconnects);
  for (const auto& file : f.fs.files) {
    TEST_ASSERT_TRUE(file.first.find(".part") == std::string::npos);
  }
}

void test_app_caches_the_agenda() {
  AppFixture f(kOneFeed);
  f.app.onOpen();
  f.runUntilIdle();
  TEST_ASSERT_EQUAL_INT(1, (int)f.fs.files.count(CalendarApp::kAgendaCachePath));
  TEST_ASSERT_EQUAL_INT(0, (int)f.fs.files.count(std::string(CalendarApp::kAgendaCachePath) + ".part"));
  const std::string agenda = f.allPagesText();

  // Opening again shows the cached agenda at once, without reading the
  // calendar, and there is nothing to download yet.
  f.fs.files.erase(CalendarApp::cachePath(kFeedUrl));
  f.app.onClose();
  f.app.onOpen();
  TEST_ASSERT_TRUE(f.app.phase() == CalendarApp::Phase::Idle);
  TEST_ASSERT_EQUAL_STRING(agenda.c_str(), f.allPagesText().c_str());
  TEST_ASSERT_EQUAL_INT(1, (int)f.network.requests.size());
}

void test_app_shows_a_stale_agenda_while_downloading_a_new_one() {
  AppFixture f(kOneFeed);
  f.app.onOpen();
  f.runUntilIdle();
  f.app.onClose();

  // An hour later the copy is stale, and the calendar has changed.
  f.clock.time += 60 * 60;
  f.network.bodies[kFeedUrl] =
      "BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nSUMMARY:Lunch\r\n"
      "DTSTART:20261109T110000Z\r\nDTEND:20261109T120000Z\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
  f.app.onOpen();
  TEST_ASSERT_TRUE(f.app.phase() == CalendarApp::Phase::Connecting);
  TEST_ASSERT_EQUAL_INT(6, (int)f.app.occurrences().size());
  f.draw();
  TEST_ASSERT_TRUE(f.display.shows("Standup"));
  TEST_ASSERT_TRUE(f.display.shows("Updating..."));

  TEST_ASSERT_TRUE(f.runUntilIdle() > 0);
  TEST_ASSERT_EQUAL_INT(2, (int)f.network.requests.size());
  TEST_ASSERT_TRUE(f.app.wantsFullRefresh());
  TEST_ASSERT_EQUAL_STRING("|Today, Mon 9 Nov|\n12:00 - 13:00|Lunch|\n", f.allPagesText().c_str());

  // The cache now holds the new agenda.
  f.app.onClose();
  f.app.onOpen();
  TEST_ASSERT_TRUE(f.app.phase() == CalendarApp::Phase::Idle);
  TEST_ASSERT_EQUAL_STRING("|Today, Mon 9 Nov|\n12:00 - 13:00|Lunch|\n", f.allPagesText().c_str());
}

void test_app_ignores_a_cached_agenda_that_does_not_fit() {
  AppFixture f(kOneFeed);
  f.app.onOpen();
  f.runUntilIdle();
  f.app.onClose();
  const std::string cached = f.fs.files[CalendarApp::kAgendaCachePath];

  // From yesterday: built again from the calendar for today.
  f.clock.time += 24 * 3600;
  f.app.onOpen();
  TEST_ASSERT_TRUE(f.app.phase() == CalendarApp::Phase::Loading);
  f.runUntilIdle();
  TEST_ASSERT_EQUAL_STRING("Today, Tue 10 Nov", f.app.pageRows(0)[0].text.c_str());
  f.app.onClose();

  // For other settings.
  AppFixture other("[calendar]\n  days = 3\n[[calendar.feed]]\n  url = \"https://example.com/home.ics\"\n");
  other.fs.files[CalendarApp::kAgendaCachePath] = cached;
  other.fs.files[CalendarApp::cachePath(kFeedUrl)] = sampleCalendar();
  other.fs.files["/.cache/calendar/synced"] = std::to_string(kMonday8am);
  other.app.onOpen();
  TEST_ASSERT_TRUE(other.app.phase() == CalendarApp::Phase::Loading);
  other.app.onClose();

  // Cut short, or not an agenda at all.
  for (const std::string& junk : {cached.substr(0, cached.size() - 3), std::string("junk")}) {
    AppFixture broken(kOneFeed);
    broken.fs.files[CalendarApp::kAgendaCachePath] = junk;
    broken.fs.files[CalendarApp::cachePath(kFeedUrl)] = sampleCalendar();
    broken.fs.files["/.cache/calendar/synced"] = std::to_string(kMonday8am);
    broken.app.onOpen();
    TEST_ASSERT_TRUE(broken.app.phase() == CalendarApp::Phase::Loading);
    broken.runUntilIdle();
    TEST_ASSERT_EQUAL_INT(6, (int)broken.app.occurrences().size());
    TEST_ASSERT_EQUAL_STRING(cached.c_str(), broken.fs.files[CalendarApp::kAgendaCachePath].c_str());
  }
}

void test_app_shows_the_cached_agenda_before_the_time_is_known() {
  AppFixture f(kOneFeed);
  f.app.onOpen();
  f.runUntilIdle();
  f.app.onClose();

  // After a power cut the clock is unknown until Wi-Fi sets it, an hour on.
  f.clock.time = -1;
  f.clock.syncTo = kMonday8am + 60 * 60;
  f.app.onOpen();
  TEST_ASSERT_TRUE(f.app.phase() == CalendarApp::Phase::Connecting);
  TEST_ASSERT_EQUAL_STRING("Mon 9 Nov", f.app.pageRows(0)[0].text.c_str());
  f.draw();
  TEST_ASSERT_TRUE(f.display.shows("Standup"));
  TEST_ASSERT_FALSE(f.display.shows("Date and time unknown"));

  f.runUntilIdle();
  TEST_ASSERT_EQUAL_INT(2, f.clock.syncs);  // one per download
  TEST_ASSERT_EQUAL_STRING("Today, Mon 9 Nov", f.app.pageRows(0)[0].text.c_str());
}

void test_app_relabels_and_moves_on_once_the_time_is_known() {
  AppFixture f(kOneFeed);
  f.network.isConfigured = false;
  f.app.onOpen();
  f.runUntilIdle();  // nothing to show: no Wi-Fi to download with
  f.fs.files[CalendarApp::cachePath(kFeedUrl)] = sampleCalendar();
  f.app.onClose();
  f.app.onOpen();
  f.runUntilIdle();
  f.app.onClose();

  f.clock.time = -1;
  f.app.onOpen();
  TEST_ASSERT_TRUE(f.app.phase() == CalendarApp::Phase::Idle);
  TEST_ASSERT_EQUAL_STRING("Mon 9 Nov", f.app.pageRows(0)[0].text.c_str());

  // The same day: just the labels.
  f.clock.time = kMonday8am;
  f.clock.ms += 30 * 1000;
  TEST_ASSERT_TRUE(f.app.tick());
  TEST_ASSERT_TRUE(f.app.phase() == CalendarApp::Phase::Idle);
  TEST_ASSERT_EQUAL_STRING("Today, Mon 9 Nov", f.app.pageRows(0)[0].text.c_str());

  // A later day: built again for it.
  f.app.onClose();
  f.clock.time = -1;
  f.app.onOpen();
  f.clock.time = kMonday8am + 24 * 3600;
  f.clock.ms += 30 * 1000;
  f.app.tick();
  TEST_ASSERT_TRUE(f.app.phase() == CalendarApp::Phase::Loading);
  f.runUntilIdle();
  TEST_ASSERT_EQUAL_STRING("Today, Tue 10 Nov", f.app.pageRows(0)[0].text.c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_civil_dates_round_trip);
  RUN_TEST(test_local_time_follows_tz);
  RUN_TEST(test_parses_ics_values);
  RUN_TEST(test_parses_rrules);
  RUN_TEST(test_weekly_rule_with_days_and_count);
  RUN_TEST(test_every_other_week_until);
  RUN_TEST(test_monthly_rules);
  RUN_TEST(test_yearly_rule_on_a_leap_day);
  RUN_TEST(test_long_running_daily_rule_skips_ahead);
  RUN_TEST(test_time_zone_onsets);
  RUN_TEST(test_parser_reads_events_in_any_piece_sizes);
  RUN_TEST(test_parser_handles_lf_endings_and_a_missing_last_newline);
  RUN_TEST(test_agenda_expands_and_converts_to_local_time);
  RUN_TEST(test_agenda_uses_the_calendars_time_zone_rules);
  RUN_TEST(test_weekly_event_keeps_its_local_time_across_dst);
  RUN_TEST(test_agenda_keeps_events_that_started_before_the_window);
  RUN_TEST(test_cache_path_is_stable);
  RUN_TEST(test_app_downloads_and_shows_the_agenda);
  RUN_TEST(test_app_shows_the_cached_copy_without_downloading_when_fresh);
  RUN_TEST(test_app_downloads_again_when_the_copy_is_stale);
  RUN_TEST(test_failed_download_keeps_the_old_copy);
  RUN_TEST(test_app_reports_wifi_failure);
  RUN_TEST(test_debug_shows_why_wifi_failed);
  RUN_TEST(test_app_gets_the_time_before_downloading);
  RUN_TEST(test_app_without_wifi_or_feeds_explains_what_to_set);
  RUN_TEST(test_app_pages_through_a_busy_agenda);
  RUN_TEST(test_app_moves_on_at_midnight);
  RUN_TEST(test_app_shows_which_calendar_an_event_is_from);
  RUN_TEST(test_header_shows_wifi_while_connected);
  RUN_TEST(test_closing_mid_download_cleans_up);
  RUN_TEST(test_app_caches_the_agenda);
  RUN_TEST(test_app_shows_a_stale_agenda_while_downloading_a_new_one);
  RUN_TEST(test_app_ignores_a_cached_agenda_that_does_not_fit);
  RUN_TEST(test_app_shows_the_cached_agenda_before_the_time_is_known);
  RUN_TEST(test_app_relabels_and_moves_on_once_the_time_is_known);
  return UNITY_END();
}

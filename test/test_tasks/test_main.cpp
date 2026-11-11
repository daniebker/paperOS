// Host-side tests for the Mindwtr reader and the Tasks app. Run with:
// pio test -e native
#include <unity.h>

#include <paperos.h>
#include <Mindwtr.h>
#include <TasksApp.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <deque>
#include <map>
#include <string>
#include <vector>

using namespace paperos;
using mindwtr::kNoTime;

namespace {

constexpr const char* kBerlin = "CET-1CEST,M3.5.0,M10.5.0/3";
constexpr const char* kUrl = "https://dav.example.com/mindwtr/data.json";

// --- Fakes -------------------------------------------------------------------

class FakeDisplay : public Display {
 public:
  int width() const override { return 540; }
  int height() const override { return 960; }
  void beginFrame(Color) override { texts.clear(); }
  void endFrame(Refresh) override {}
  void fillRect(int, int, int, int, Color) override {}
  void drawRect(int, int, int, int, Color) override {}
  void drawLine(int, int, int, int, Color) override {}
  void drawText(const char* text, int, int, TextSize, TextAlign, Color) override {
    texts.emplace_back(text);
  }
  int textHeight(TextSize) const override { return 30; }
  int textWidth(const char* text, TextSize) const override { return 10 * int(strlen(text)); }

  bool shows(const std::string& text) const {
    return std::find(texts.begin(), texts.end(), text) != texts.end();
  }
  bool showsPart(const std::string& text) const {
    for (const auto& t : texts)
      if (t.find(text) != std::string::npos) return true;
    return false;
  }
  std::vector<std::string> texts;
};

class FakeBattery : public Battery {
 public:
  int level() const override { return 80; }
};

class FakeClock : public Clock {
 public:
  int64_t now() const override { return time; }
  uint32_t uptimeMs() const override { return ms; }
  void sync() override {
    if (syncTo >= 0) time = syncTo;
  }
  int64_t time = -1;
  int64_t syncTo = -1;
  uint32_t ms = 0;
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
  bool list(const char*, std::vector<DirEntry>&) override { return false; }
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

class FakeResponse : public HttpResponse {
 public:
  FakeResponse(int status, std::string body, std::string etag)
      : status_(status), body_(std::move(body)), etag_(std::move(etag)) {}
  int read(void* buffer, size_t size) override {
    if (pos_ >= body_.size()) return kEnd;
    const size_t count = std::min<size_t>({size, body_.size() - pos_, 1000});
    memcpy(buffer, body_.data() + pos_, count);
    pos_ += count;
    return int(count);
  }
  int64_t contentLength() const override { return int64_t(body_.size()); }
  int status() const override { return status_; }
  std::string header(const char* name) const override {
    return strcasecmp(name, "ETag") == 0 ? etag_ : "";
  }

 private:
  int status_;
  std::string body_;
  std::string etag_;
  size_t pos_ = 0;
};

// A WebDAV server holding one file, with ETags that change on every write.
class FakeServer : public Network {
 public:
  bool configured() const override { return true; }
  void connect() override {
    ++connects;
    state_ = NetworkState::Connecting;
  }
  void disconnect() override {
    ++disconnects;
    state_ = NetworkState::Off;
  }
  NetworkState state() override {
    if (state_ == NetworkState::Connecting) state_ = joinFails ? NetworkState::Failed
                                                               : NetworkState::Connected;
    return state_;
  }
  std::unique_ptr<HttpResponse> get(const char*, std::string& error) override {
    error = "unexpected";
    return nullptr;
  }
  std::unique_ptr<HttpResponse> send(const HttpRequest& request, std::string& error) override {
    TEST_ASSERT_TRUE(state_ == NetworkState::Connected);
    requests.push_back(request);
    if (request.url != kUrl) return respond(404, "");
    if (request.method == "GET") {
      // Someone else edits the file between our download and upload.
      if (!editBeforeNextPut.empty()) {
        pendingEdit = editBeforeNextPut;
        editBeforeNextPut.clear();
      }
      if (weakenEtags) {
        // Like Apache's compression module in front of Nextcloud.
        return std::unique_ptr<HttpResponse>(new FakeResponse(
            200, body, "W/\"v" + std::to_string(version) + "-gzip\""));
      }
      return respond(200, body);
    }
    if (request.method == "PUT") {
      if (!pendingEdit.empty()) {
        body = pendingEdit;
        pendingEdit.clear();
        ++version;
      }
      std::string ifMatch;
      for (const auto& h : request.headers)
        if (h.first == "If-Match") ifMatch = h.second;
      if (ifMatch != etag() || refuseAll) return respond(412, "");
      body = request.body;
      ++version;
      return respond(201, "");
    }
    error = "unexpected method";
    return nullptr;
  }

  std::string etag() const { return sendEtag ? "\"v" + std::to_string(version) + "\"" : ""; }
  std::unique_ptr<HttpResponse> respond(int status, const std::string& content) {
    return std::unique_ptr<HttpResponse>(new FakeResponse(status, content, etag()));
  }
  int puts() const {
    return int(std::count_if(requests.begin(), requests.end(),
                             [](const HttpRequest& r) { return r.method == "PUT"; }));
  }

  std::string body;
  int version = 1;
  bool sendEtag = true;
  bool weakenEtags = false;
  bool refuseAll = false;
  bool joinFails = false;
  std::string editBeforeNextPut;  // replaces the file just before the next PUT
  std::string pendingEdit;
  int connects = 0;
  int disconnects = 0;
  std::vector<HttpRequest> requests;

 private:
  NetworkState state_ = NetworkState::Off;
};

// --- Sample data ---------------------------------------------------------------

// Tuesday 10 November 2026, 10:00 in Berlin.
int64_t now() { return mindwtr::parseTime("2026-11-10T09:00:00Z"); }

std::string task(const std::string& id, const std::string& title, const std::string& status,
                 const std::string& extra = "") {
  return "{\"id\":\"" + id + "\",\"title\":\"" + title + "\",\"status\":\"" + status +
         "\",\"tags\":[],\"contexts\":[]" + extra +
         ",\"createdAt\":\"2026-11-01T08:00:00.000Z\",\"updatedAt\":\"2026-11-01T08:00:00.000Z\"}";
}

std::string sampleData() {
  const std::vector<std::string> tasks = {
      task("t1", "Write report", "next",
           ",\"isFocusedToday\":true,\"focusOrder\":2,\"dueDate\":\"2026-11-10\",\"rev\":4,"
           "\"revBy\":\"desktop\""),
      task("t2", "Call Anna", "next",
           ",\"isFocusedToday\":true,\"focusOrder\":1,\"contexts\":[\"@phone\"]"),
      task("t3", "Buy milk", "next", ",\"projectId\":\"p1\",\"contexts\":[\"@errands\"]"),
      task("t4", "Fix bike", "next", ",\"dueDate\":\"2026-11-12\""),
      task("t5", "Plan trip", "next", ",\"startTime\":\"2026-11-13T08:00:00.000Z\""),
      task("t6", "Dentist", "waiting", ",\"dueDate\":\"2026-11-11\""),
      task("t7", "Far away", "next", ",\"startTime\":\"2026-12-20\""),
      task("t8", "Done thing", "done", ",\"completedAt\":\"2026-11-09T10:00:00.000Z\""),
      task("t9", "Deleted", "next", ",\"deletedAt\":\"2026-11-09T10:00:00.000Z\""),
      task("t10", "Someday idea", "someday"),
      task("t11", "Weekly review", "next",
           ",\"recurrence\":{\"rule\":\"weekly\",\"strategy\":\"strict\"}"),
      task("t12", "Caf\\u00e9 \\\"quotes\\\"", "next",
           ",\"description\":\"Line one\\nLine two\",\"priority\":\"high\","
           "\"timeEstimate\":\"30min\",\"tags\":[\"#work\"],"
           "\"checklist\":[{\"id\":\"c1\",\"title\":\"Step A\",\"isCompleted\":true},"
           "{\"id\":\"c2\",\"title\":\"Step B\",\"isCompleted\":false}],\"rev\":null"),
      task("t13", "Inbox thing", "inbox"),
  };
  std::string out = "{\n  \"tasks\": [\n    ";
  for (size_t i = 0; i < tasks.size(); ++i) out += (i ? ",\n    " : "") + tasks[i];
  out +=
      "\n  ],\n  \"projects\": [{\"id\":\"p1\",\"title\":\"Home\",\"status\":\"active\","
      "\"color\":\"#fff\",\"order\":0,\"tagIds\":[]}],\n"
      "  \"areas\": [], \"sections\": [], \"people\": [],\n"
      "  \"settings\": {\"gtd\": {\"focus\": {\"limit\": 3}}, \"weird\": [1.5e3, null, true]}\n}\n";
  return out;
}

std::vector<std::string> titles(const mindwtr::Data& data, const std::vector<size_t>& list) {
  std::vector<std::string> out;
  for (size_t i : list) out.push_back(data.tasks[i].title);
  return out;
}

void assertTitles(const std::vector<std::string>& expected, const std::vector<std::string>& got) {
  std::string e, g;
  for (const auto& s : expected) e += s + "|";
  for (const auto& s : got) g += s + "|";
  TEST_ASSERT_EQUAL_STRING(e.c_str(), g.c_str());
}

const char* kConfig =
    "[sync]\n"
    "  url = \"https://dav.example.com/mindwtr/\"\n"
    "  username = \"dan\"\n"
    "  password = \"secret\"\n";

struct Harness {
  Harness() {
    config.parse(kConfig);
    clock.time = now();
    server.body = sampleData();
  }
  void run(int limit = 200) {
    for (int i = 0; i < limit; ++i) {
      app.tick();
      if (app.phase() == TasksApp::Phase::Idle) return;
    }
  }
  std::vector<std::string> taskTitles() const {
    std::vector<std::string> out;
    for (const auto& row : app.rows())
      if (row.kind == TasksApp::Row::Kind::Task) out.push_back(row.text);
    return out;
  }
  // Selects the task with this title on the list.
  void select(const std::string& title) {
    for (int i = 0; i < 40; ++i) {
      const mindwtr::Task* t = app.selectedTask();
      if (t != nullptr && t->title == title) return;
      app.onInput(InputEvent::Next);
    }
    TEST_FAIL_MESSAGE(("task not on the list: " + title).c_str());
  }
  Config config;
  MemoryFileSystem fs;
  FakeServer server;
  FakeClock clock;
  FakeDisplay display;
  FakeBattery battery;
  TasksApp app{config, fs, server, clock, display, battery};
};

}  // namespace

void setUp() {
  setenv("TZ", kBerlin, 1);
  tzset();
}
void tearDown() {}

// --- Reading data.json -----------------------------------------------------------

void test_parse_reads_tasks_and_projects() {
  mindwtr::Data data;
  std::string error;
  TEST_ASSERT_TRUE(mindwtr::parse(sampleData(), data, error));
  TEST_ASSERT_EQUAL(13, int(data.tasks.size()));
  TEST_ASSERT_EQUAL(1, int(data.projects.size()));

  const mindwtr::Task* t1 = data.find("t1");
  TEST_ASSERT_NOT_NULL(t1);
  TEST_ASSERT_EQUAL_STRING("Write report", t1->title.c_str());
  TEST_ASSERT_TRUE(t1->focused);
  TEST_ASSERT_EQUAL(4, int(t1->rev));
  TEST_ASSERT_FALSE(t1->dueHasTime);
  TEST_ASSERT_TRUE(t1->due == mindwtr::parseTime("2026-11-10"));

  const mindwtr::Task* t3 = data.find("t3");
  TEST_ASSERT_EQUAL_STRING("Home", data.projectTitle(t3->projectId).c_str());
  TEST_ASSERT_EQUAL_STRING("@errands", t3->contexts.at(0).c_str());

  const mindwtr::Task* t11 = data.find("t11");
  TEST_ASSERT_TRUE(t11->recurring);
  TEST_ASSERT_EQUAL_STRING("weekly", t11->recurrence.c_str());

  const mindwtr::Task* t12 = data.find("t12");
  TEST_ASSERT_EQUAL_STRING("Caf\xC3\xA9 \"quotes\"", t12->title.c_str());
  TEST_ASSERT_EQUAL_STRING("Line one\nLine two", t12->description.c_str());
  TEST_ASSERT_EQUAL_STRING("high", t12->priority.c_str());
  TEST_ASSERT_EQUAL_STRING("30min", t12->timeEstimate.c_str());
  TEST_ASSERT_EQUAL(2, int(t12->checklist.size()));
  TEST_ASSERT_TRUE(t12->checklist[0].done);
  TEST_ASSERT_EQUAL_STRING("Step B", t12->checklist[1].title.c_str());
  TEST_ASSERT_EQUAL(0, int(t12->rev));

  TEST_ASSERT_TRUE(data.find("t9")->deleted);
}

void test_parse_rejects_other_files() {
  mindwtr::Data data;
  std::string error;
  TEST_ASSERT_FALSE(mindwtr::parse("<html>Login</html>", data, error));
  TEST_ASSERT_EQUAL_STRING("Not a Mindwtr data file", error.c_str());
  TEST_ASSERT_FALSE(mindwtr::parse("{\"settings\":{}}", data, error));
  TEST_ASSERT_EQUAL_STRING("Not a Mindwtr data file (no tasks)", error.c_str());
  TEST_ASSERT_FALSE(mindwtr::parse("{\"tasks\":[{\"id\":\"a\",\"title\":\"x", data, error));
  TEST_ASSERT_TRUE(error.find("damaged") != std::string::npos);
  TEST_ASSERT_TRUE(mindwtr::parse("\xEF\xBB\xBF{\"tasks\":[]}", data, error));
}

void test_parse_time_formats() {
  bool hasTime = true;
  // 10 Nov 2026 is in winter time, UTC+1.
  TEST_ASSERT_TRUE(mindwtr::parseTime("2026-11-10", &hasTime) ==
                   mindwtr::parseTime("2026-11-09T23:00:00Z"));
  TEST_ASSERT_FALSE(hasTime);
  TEST_ASSERT_TRUE(mindwtr::parseTime("2026-11-10T10:00", &hasTime) ==
                   mindwtr::parseTime("2026-11-10T09:00:00.000Z"));
  TEST_ASSERT_TRUE(hasTime);
  TEST_ASSERT_TRUE(mindwtr::parseTime("2026-11-10T12:30:00+02:00") ==
                   mindwtr::parseTime("2026-11-10T10:30:00Z"));
  TEST_ASSERT_TRUE(mindwtr::parseTime("2026-07-01") == mindwtr::parseTime("2026-06-30T22:00Z"));
  TEST_ASSERT_TRUE(mindwtr::parseTime("") == kNoTime);
  TEST_ASSERT_TRUE(mindwtr::parseTime("tomorrow") == kNoTime);
  TEST_ASSERT_TRUE(mindwtr::parseTime("2026-13-01") == kNoTime);
}

void test_iso_time_round_trips() {
  TEST_ASSERT_EQUAL_STRING("2026-11-10T09:00:00.000Z", mindwtr::isoTime(now()).c_str());
  TEST_ASSERT_EQUAL_STRING("1970-01-01T00:00:00.000Z", mindwtr::isoTime(0).c_str());
  TEST_ASSERT_EQUAL_STRING("2028-02-29T23:59:59.000Z",
                           mindwtr::isoTime(mindwtr::parseTime("2028-02-29T23:59:59Z")).c_str());
}

void test_lists_sort_tasks() {
  mindwtr::Data data;
  std::string error;
  mindwtr::parse(sampleData(), data, error);
  const mindwtr::Lists lists = mindwtr::buildLists(data, now(), 7);
  assertTitles({"Call Anna", "Write report"}, titles(data, lists.focus));
  // Due first, then in file order. Not started, done, deleted and someday
  // tasks are left out.
  assertTitles({"Fix bike", "Buy milk", "Weekly review", "Caf\xC3\xA9 \"quotes\""},
               titles(data, lists.next));
  assertTitles({"Dentist", "Plan trip"}, titles(data, lists.upcoming));

  // A shorter window leaves out the trip on Friday.
  assertTitles({"Dentist"}, titles(data, mindwtr::buildLists(data, now(), 2).upcoming));
  // Once the trip's start has come, it is a next action.
  const mindwtr::Lists later =
      mindwtr::buildLists(data, mindwtr::parseTime("2026-11-13T07:00:00Z"), 7);
  const std::vector<std::string> laterNext = titles(data, later.next);
  TEST_ASSERT_EQUAL(1, int(std::count(laterNext.begin(), laterNext.end(), "Plan trip")));
}

void test_lists_without_the_time() {
  mindwtr::Data data;
  std::string error;
  mindwtr::parse(sampleData(), data, error);
  const mindwtr::Lists lists = mindwtr::buildLists(data, -1, 7);
  TEST_ASSERT_EQUAL(2, int(lists.focus.size()));
  TEST_ASSERT_TRUE(lists.upcoming.empty());
  // Start dates can't be checked, so every next action is listed.
  TEST_ASSERT_EQUAL(6, int(lists.next.size()));
}

std::string areaData() {
  const std::vector<std::string> tasks = {
      task("a1", "Loose", "next"),
      task("a2", "Home due later", "next", ",\"areaId\":\"home\",\"dueDate\":\"2026-11-14\""),
      task("a3", "Work undated", "next", ",\"areaId\":\"work\""),
      task("a4", "Home via project", "next", ",\"projectId\":\"p1\",\"dueDate\":\"2026-11-12\""),
      task("a5", "Home started", "next",
           ",\"areaId\":\"home\",\"startTime\":\"2026-11-09\",\"dueDate\":\"2026-11-20\""),
      task("a6", "Work started", "next", ",\"areaId\":\"work\",\"startTime\":\"2026-11-10\""),
      task("a7", "Gone area", "next", ",\"areaId\":\"old\""),
  };
  std::string out = "{\"tasks\":[";
  for (size_t i = 0; i < tasks.size(); ++i) out += (i ? "," : "") + tasks[i];
  out +=
      "],\"projects\":[{\"id\":\"p1\",\"title\":\"House\",\"areaId\":\"home\"}],"
      "\"areas\":[{\"id\":\"work\",\"name\":\"Work\",\"order\":1},"
      "{\"id\":\"home\",\"name\":\"Home\",\"order\":0},"
      "{\"id\":\"old\",\"name\":\"Old\",\"order\":2,\"deletedAt\":\"2026-11-01T08:00:00.000Z\"}]}";
  return out;
}

void test_next_actions_grouped_by_area() {
  mindwtr::Data data;
  std::string error;
  TEST_ASSERT_TRUE(mindwtr::parse(areaData(), data, error));
  TEST_ASSERT_EQUAL(3, int(data.areas.size()));
  // Areas in their order, no area (or a deleted one) last. In each area by
  // start date, then due date, undated last.
  assertTitles({"Home started", "Home via project", "Home due later", "Work started",
                "Work undated", "Loose", "Gone area"},
               titles(data, mindwtr::buildLists(data, now(), 7).next));
}

void test_next_actions_show_area_subheadings() {
  Harness h;
  h.server.body = areaData();
  h.app.onOpen();
  h.run();
  std::vector<std::string> rows;
  for (const auto& row : h.app.rows()) {
    if (row.kind == TasksApp::Row::Kind::Subheading) rows.push_back("## " + row.text);
    if (row.kind == TasksApp::Row::Kind::Task) rows.push_back(row.text);
  }
  assertTitles({"## Home", "Home started", "Home via project", "Home due later", "## Work",
                "Work started", "Work undated", "## No area", "Loose", "Gone area"},
               rows);

  // Without areas, no subheadings.
  Harness plain;
  plain.app.onOpen();
  plain.run();
  for (const auto& row : plain.app.rows())
    TEST_ASSERT_TRUE(row.kind != TasksApp::Row::Kind::Subheading);
}

// --- Marking done ------------------------------------------------------------------

void test_mark_done_changes_only_that_task() {
  const std::string original = sampleData();
  std::string text = original;
  TEST_ASSERT_EQUAL(int(mindwtr::DoneResult::Ok), int(mindwtr::markDone(text, "t1", now())));

  // Everything outside the task is kept byte for byte.
  const size_t start = original.find("{\"id\":\"t1\"");
  const size_t end = original.find("},", start) + 1;
  TEST_ASSERT_EQUAL_STRING(original.substr(0, start).c_str(), text.substr(0, start).c_str());
  const std::string tail = original.substr(end);
  TEST_ASSERT_EQUAL_STRING(tail.c_str(), text.substr(text.size() - tail.size()).c_str());

  const std::string changed = text.substr(start, text.size() - tail.size() - start);
  TEST_ASSERT_TRUE(changed.find("\"status\":\"done\"") != std::string::npos);
  TEST_ASSERT_TRUE(changed.find("\"rev\":5") != std::string::npos);
  TEST_ASSERT_TRUE(changed.find("\"revBy\":\"paperos\"") != std::string::npos);
  TEST_ASSERT_TRUE(changed.find("\"updatedAt\":\"2026-11-10T09:00:00.000Z\"") !=
                   std::string::npos);
  TEST_ASSERT_TRUE(changed.find("\"completedAt\":\"2026-11-10T09:00:00.000Z\"") !=
                   std::string::npos);
  TEST_ASSERT_TRUE(changed.find("\"title\":\"Write report\"") != std::string::npos);
  TEST_ASSERT_TRUE(changed.find("\"isFocusedToday\":true") != std::string::npos);

  mindwtr::Data data;
  std::string error;
  TEST_ASSERT_TRUE(mindwtr::parse(text, data, error));
  TEST_ASSERT_EQUAL_STRING("done", data.find("t1")->status.c_str());
  TEST_ASSERT_EQUAL(5, int(data.find("t1")->rev));
  TEST_ASSERT_EQUAL(13, int(data.tasks.size()));
}

void test_mark_done_adds_missing_fields() {
  std::string text = "{\"tasks\":[{\"id\":\"a\",\"title\":\"A\",\"status\":\"inbox\",\"rev\":null}]}";
  TEST_ASSERT_EQUAL(int(mindwtr::DoneResult::Ok), int(mindwtr::markDone(text, "a", now())));
  TEST_ASSERT_EQUAL_STRING(
      "{\"tasks\":[{\"id\":\"a\",\"title\":\"A\",\"status\":\"done\",\"rev\":1,"
      "\"completedAt\":\"2026-11-10T09:00:00.000Z\","
      "\"updatedAt\":\"2026-11-10T09:00:00.000Z\",\"revBy\":\"paperos\"}]}",
      text.c_str());
}

void test_mark_done_refuses_what_it_should() {
  std::string text = sampleData();
  const std::string original = text;
  TEST_ASSERT_EQUAL(int(mindwtr::DoneResult::AlreadyDone), int(mindwtr::markDone(text, "t8", 0)));
  TEST_ASSERT_EQUAL(int(mindwtr::DoneResult::Recurring), int(mindwtr::markDone(text, "t11", 0)));
  TEST_ASSERT_EQUAL(int(mindwtr::DoneResult::NotFound), int(mindwtr::markDone(text, "zz", 0)));
  TEST_ASSERT_TRUE(text == original);
  std::string broken = "{\"tasks\":[{\"id\":";
  TEST_ASSERT_EQUAL(int(mindwtr::DoneResult::Invalid), int(mindwtr::markDone(broken, "a", 0)));
}

// --- The app -------------------------------------------------------------------------

void test_app_downloads_and_lists_tasks() {
  Harness h;
  h.app.onOpen();
  h.run();
  TEST_ASSERT_EQUAL(1, h.server.connects);
  TEST_ASSERT_EQUAL(1, h.server.disconnects);  // Wi-Fi off again
  TEST_ASSERT_EQUAL_STRING("GET", h.server.requests.at(0).method.c_str());
  assertTitles({"Call Anna", "Write report", "Fix bike", "Buy milk", "Weekly review",
                "Cafe \"quotes\"", "Dentist", "Plan trip"},
               h.taskTitles());
  TEST_ASSERT_TRUE(h.fs.files[TasksApp::kCachePath] == h.server.body);

  h.app.draw(h.display);
  TEST_ASSERT_TRUE(h.display.shows("Today's Focus"));
  TEST_ASSERT_TRUE(h.display.shows("Next Actions"));
  TEST_ASSERT_TRUE(h.display.shows("Upcoming, 7 days"));
  TEST_ASSERT_TRUE(h.display.shows("Due Today"));             // Write report
  TEST_ASSERT_TRUE(h.display.shows("Due Thu 12 Nov"));        // Fix bike
  TEST_ASSERT_TRUE(h.display.shows("Home - @errands"));       // Buy milk
  TEST_ASSERT_TRUE(h.display.shows("Due Tomorrow"));          // Dentist
  TEST_ASSERT_TRUE(h.display.shows("Starts Fri 13 Nov 09:00"));  // Plan trip
  TEST_ASSERT_TRUE(h.display.shows("Updated 10:00"));
}

void test_app_shows_the_saved_copy_first() {
  Harness h;
  h.fs.files[TasksApp::kCachePath] = sampleData();
  h.server.joinFails = true;
  h.app.onOpen();
  TEST_ASSERT_EQUAL(int(TasksApp::Phase::Loading), int(h.app.phase()));
  h.run();
  TEST_ASSERT_EQUAL(8, int(h.taskTitles().size()));
  TEST_ASSERT_EQUAL_STRING("Could not connect to Wi-Fi", h.app.statusText().c_str());
}

void test_app_explains_missing_settings() {
  Harness h;
  h.config.parse("");
  h.app.onOpen();
  h.run();
  TEST_ASSERT_EQUAL(0, h.server.connects);
  h.app.draw(h.display);
  TEST_ASSERT_TRUE(h.display.shows("No tasks yet"));
  TEST_ASSERT_TRUE(h.display.shows("Set [sync] in config.toml"));
}

void test_app_rejects_a_file_that_is_not_mindwtr() {
  Harness h;
  h.server.body = "<html>Sign in</html>";
  h.app.onOpen();
  h.run();
  TEST_ASSERT_EQUAL_STRING("Not a Mindwtr data file", h.app.statusText().c_str());
  TEST_ASSERT_TRUE(h.fs.files.count(TasksApp::kCachePath) == 0);
}

void test_selection_moves_between_tasks_and_wraps() {
  Harness h;
  h.app.onOpen();
  h.run();
  TEST_ASSERT_EQUAL_STRING("Call Anna", h.app.selectedTask()->title.c_str());
  h.app.onInput(InputEvent::Next);
  TEST_ASSERT_EQUAL_STRING("Write report", h.app.selectedTask()->title.c_str());
  h.app.onInput(InputEvent::Previous);
  h.app.onInput(InputEvent::Previous);
  TEST_ASSERT_EQUAL_STRING("Plan trip", h.app.selectedTask()->title.c_str());
}

void test_back_goes_back_one_screen_at_a_time() {
  Harness h;
  h.app.onOpen();
  h.run();
  h.select("Caf\xC3\xA9 \"quotes\"");

  TEST_ASSERT_TRUE(h.app.onInput(InputEvent::Select));
  TEST_ASSERT_EQUAL(int(TasksApp::Screen::Menu), int(h.app.screen()));
  TEST_ASSERT_TRUE(h.app.wantsFullRefresh());
  h.app.draw(h.display);
  TEST_ASSERT_TRUE(h.display.shows("Mark as done"));
  TEST_ASSERT_TRUE(h.display.shows("View details"));

  h.app.onInput(InputEvent::Next);
  TEST_ASSERT_TRUE(h.app.onInput(InputEvent::Select));
  TEST_ASSERT_EQUAL(int(TasksApp::Screen::Details), int(h.app.screen()));
  h.app.draw(h.display);
  TEST_ASSERT_TRUE(h.display.shows("Cafe \"quotes\""));
  TEST_ASSERT_TRUE(h.display.shows("Status: Next action"));
  TEST_ASSERT_TRUE(h.display.shows("Priority: High"));
  TEST_ASSERT_TRUE(h.display.shows("Time: 30min"));
  TEST_ASSERT_TRUE(h.display.shows("Tags: #work"));
  TEST_ASSERT_TRUE(h.display.shows("Checklist (1 of 2 done)"));
  TEST_ASSERT_TRUE(h.display.shows("[x] Step A"));
  TEST_ASSERT_TRUE(h.display.shows("[ ] Step B"));
  TEST_ASSERT_TRUE(h.display.shows("Notes"));
  TEST_ASSERT_TRUE(h.display.shows("Line one"));
  TEST_ASSERT_TRUE(h.display.shows("Line two"));

  TEST_ASSERT_TRUE(h.app.onInput(InputEvent::Back));
  TEST_ASSERT_EQUAL(int(TasksApp::Screen::Menu), int(h.app.screen()));
  TEST_ASSERT_TRUE(h.app.onInput(InputEvent::Back));
  TEST_ASSERT_EQUAL(int(TasksApp::Screen::List), int(h.app.screen()));
  TEST_ASSERT_FALSE(h.app.onInput(InputEvent::Back));  // the shell closes the app
}

void test_long_details_turn_pages() {
  Harness h;
  std::string notes;
  for (int i = 0; i < 60; ++i) notes += "Line " + std::to_string(i) + "\\n";
  h.server.body = "{\"tasks\":[" + task("a", "Long", "next", ",\"description\":\"" + notes + "\"") +
                  "]}";
  h.app.onOpen();
  h.run();
  h.app.onInput(InputEvent::Select);
  h.app.onInput(InputEvent::Next);
  h.app.onInput(InputEvent::Select);
  TEST_ASSERT_TRUE(h.app.detailPageCount() > 1);
  TEST_ASSERT_FALSE(h.app.onInput(InputEvent::Previous));
  TEST_ASSERT_TRUE(h.app.onInput(InputEvent::Next));
  TEST_ASSERT_EQUAL(1, int(h.app.detailPage()));
  h.app.draw(h.display);
  TEST_ASSERT_TRUE(h.display.showsPart("2 / "));
  TEST_ASSERT_FALSE(h.display.shows("Long"));  // the title is on page 1
}

void test_mark_done_saves_to_the_server() {
  Harness h;
  h.app.onOpen();
  h.run();
  h.select("Buy milk");
  h.app.onInput(InputEvent::Select);
  TEST_ASSERT_EQUAL(0, int(h.app.menuIndex()));
  TEST_ASSERT_TRUE(h.app.onInput(InputEvent::Select));
  TEST_ASSERT_EQUAL(int(TasksApp::Screen::List), int(h.app.screen()));
  TEST_ASSERT_EQUAL(int(TasksApp::Phase::Connecting), int(h.app.phase()));
  h.app.draw(h.display);
  TEST_ASSERT_TRUE(h.display.shows("Marking done..."));

  h.run();
  TEST_ASSERT_EQUAL(1, h.server.puts());
  const HttpRequest& put = h.server.requests.back();
  TEST_ASSERT_EQUAL_STRING(kUrl, put.url.c_str());
  bool ifMatch = false;
  for (const auto& header : put.headers)
    if (header.first == "If-Match" && header.second == "\"v1\"") ifMatch = true;
  TEST_ASSERT_TRUE(ifMatch);

  mindwtr::Data saved;
  std::string error;
  TEST_ASSERT_TRUE(mindwtr::parse(h.server.body, saved, error));
  TEST_ASSERT_EQUAL_STRING("done", saved.find("t3")->status.c_str());
  TEST_ASSERT_EQUAL_STRING("next", saved.find("t4")->status.c_str());
  TEST_ASSERT_TRUE(h.fs.files[TasksApp::kCachePath] == h.server.body);
  TEST_ASSERT_EQUAL(7, int(h.taskTitles().size()));
  TEST_ASSERT_EQUAL_STRING("Done: Buy milk", h.app.statusText().c_str());
  TEST_ASSERT_EQUAL(2, h.server.disconnects);
}

void test_mark_done_starts_again_when_the_file_changed() {
  Harness h;
  h.app.onOpen();
  h.run();
  // Mindwtr adds a task while paperOS is saving.
  std::string edited = sampleData();
  edited.replace(edited.find("    {\"id\":\"t1\""), 4, "    " + task("new", "New one", "next") +
                                                        ",\n    ");
  h.server.editBeforeNextPut = edited;

  h.select("Buy milk");
  h.app.onInput(InputEvent::Select);
  h.app.onInput(InputEvent::Select);
  h.run();
  TEST_ASSERT_EQUAL(2, h.server.puts());  // the first was refused with 412
  mindwtr::Data saved;
  std::string error;
  TEST_ASSERT_TRUE(mindwtr::parse(h.server.body, saved, error));
  TEST_ASSERT_NOT_NULL(saved.find("new"));  // their change is kept
  TEST_ASSERT_EQUAL_STRING("done", saved.find("t3")->status.c_str());
  TEST_ASSERT_EQUAL_STRING("Done: Buy milk", h.app.statusText().c_str());
}

void test_mark_done_works_with_weakened_etags() {
  Harness h;
  h.server.weakenEtags = true;
  h.app.onOpen();
  h.run();
  h.select("Buy milk");
  h.app.onInput(InputEvent::Select);
  h.app.onInput(InputEvent::Select);
  h.run();
  TEST_ASSERT_EQUAL(1, h.server.puts());
  TEST_ASSERT_EQUAL_STRING("Done: Buy milk", h.app.statusText().c_str());
}

void test_mark_done_gives_up_when_always_refused() {
  Harness h;
  h.server.refuseAll = true;
  h.app.onOpen();
  h.run();
  h.select("Buy milk");
  h.app.onInput(InputEvent::Select);
  h.app.onInput(InputEvent::Select);
  h.run();
  TEST_ASSERT_EQUAL(TasksApp::kMaxSaveAttempts, h.server.puts());
  TEST_ASSERT_EQUAL_STRING("Not saved: the server refused it 3 times (HTTP 412)",
                           h.app.statusText().c_str());
  TEST_ASSERT_EQUAL(8, int(h.taskTitles().size()));
}

void test_repeating_tasks_are_not_marked_done() {
  Harness h;
  h.app.onOpen();
  h.run();
  h.select("Weekly review");
  h.app.onInput(InputEvent::Select);
  h.app.draw(h.display);
  TEST_ASSERT_TRUE(h.display.shows("Repeating: complete it in Mindwtr"));
  h.app.onInput(InputEvent::Select);
  h.run();
  TEST_ASSERT_EQUAL(0, h.server.puts());
  TEST_ASSERT_EQUAL_STRING("Repeating tasks can only be completed in Mindwtr",
                           h.app.statusText().c_str());
}

void test_no_etag_means_no_save() {
  Harness h;
  h.server.sendEtag = false;
  h.app.onOpen();
  h.run();
  h.select("Buy milk");
  h.app.onInput(InputEvent::Select);
  h.app.onInput(InputEvent::Select);
  h.run();
  TEST_ASSERT_EQUAL(0, h.server.puts());
  TEST_ASSERT_EQUAL_STRING("Server gave no ETag, so not saved", h.app.statusText().c_str());
  TEST_ASSERT_EQUAL(8, int(h.taskTitles().size()));
  h.app.draw(h.display);
  TEST_ASSERT_FALSE(h.display.shows("Marking done..."));
}

void test_mark_done_needs_the_time() {
  Harness h;
  h.clock.time = -1;  // never set
  h.app.onOpen();
  h.run(5);
  h.clock.ms = 100000;  // the time sync gives up
  h.run();
  // No upcoming without a date, but every next action, started or not.
  TEST_ASSERT_EQUAL(8, int(h.taskTitles().size()));
  h.select("Buy milk");
  h.app.onInput(InputEvent::Select);
  h.app.onInput(InputEvent::Select);
  h.run(5);
  h.clock.ms = 200000;
  h.run();
  TEST_ASSERT_EQUAL(0, h.server.puts());
  TEST_ASSERT_EQUAL_STRING("Could not get the time, so not saved", h.app.statusText().c_str());
}

void test_app_downloads_again_after_refresh_minutes() {
  Harness h;
  h.app.onOpen();
  h.run();
  TEST_ASSERT_EQUAL(1, h.server.connects);
  h.clock.ms = TasksApp::kDefaultRefreshMinutes * 60000u - 1;
  h.app.tick();
  TEST_ASSERT_EQUAL(1, h.server.connects);
  h.clock.ms += 1;
  TEST_ASSERT_TRUE(h.app.tick());
  h.run();
  TEST_ASSERT_EQUAL(2, h.server.connects);
}

void test_lists_move_on_at_midnight() {
  Harness h;
  h.app.onOpen();
  h.run();
  h.clock.time = mindwtr::parseTime("2026-11-11T08:00:00Z");  // Wednesday
  TEST_ASSERT_TRUE(h.app.tick());
  h.app.draw(h.display);
  TEST_ASSERT_TRUE(h.display.shows("Overdue, due Tue 10 Nov"));  // Write report
  TEST_ASSERT_TRUE(h.display.shows("Due Today"));                // Dentist
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_parse_reads_tasks_and_projects);
  RUN_TEST(test_parse_rejects_other_files);
  RUN_TEST(test_parse_time_formats);
  RUN_TEST(test_iso_time_round_trips);
  RUN_TEST(test_lists_sort_tasks);
  RUN_TEST(test_lists_without_the_time);
  RUN_TEST(test_next_actions_grouped_by_area);
  RUN_TEST(test_next_actions_show_area_subheadings);
  RUN_TEST(test_mark_done_changes_only_that_task);
  RUN_TEST(test_mark_done_adds_missing_fields);
  RUN_TEST(test_mark_done_refuses_what_it_should);
  RUN_TEST(test_app_downloads_and_lists_tasks);
  RUN_TEST(test_app_shows_the_saved_copy_first);
  RUN_TEST(test_app_explains_missing_settings);
  RUN_TEST(test_app_rejects_a_file_that_is_not_mindwtr);
  RUN_TEST(test_selection_moves_between_tasks_and_wraps);
  RUN_TEST(test_back_goes_back_one_screen_at_a_time);
  RUN_TEST(test_long_details_turn_pages);
  RUN_TEST(test_mark_done_saves_to_the_server);
  RUN_TEST(test_mark_done_starts_again_when_the_file_changed);
  RUN_TEST(test_mark_done_works_with_weakened_etags);
  RUN_TEST(test_mark_done_gives_up_when_always_refused);
  RUN_TEST(test_repeating_tasks_are_not_marked_done);
  RUN_TEST(test_no_etag_means_no_save);
  RUN_TEST(test_mark_done_needs_the_time);
  RUN_TEST(test_app_downloads_again_after_refresh_minutes);
  RUN_TEST(test_lists_move_on_at_midnight);
  return UNITY_END();
}

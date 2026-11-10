#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <paperos.h>

#include "Mindwtr.h"

// Shows your Mindwtr tasks from the data.json it syncs to a WebDAV folder (see
// [sync] in config.toml): Today's Focus, Next Actions, and what's coming up.
//
//   [tasks]
//     file = "data.json"     # in the [sync] folder
//     days = 7               # how far ahead Upcoming looks
//     refresh_minutes = 15   # how often to download again while open
//
// The last copy is kept in /.cache/tasks on the SD card and shown at once on
// opening, then downloaded again over Wi-Fi.
//
// On the list, A / B move between tasks and hold A opens a menu for the
// selected one: Mark as done, or View details. Marking done downloads the
// latest data.json, changes only that task, and uploads it only if nothing
// else changed it in between. Hold B goes back a screen, then home.
class TasksApp : public paperos::App {
 public:
  static constexpr const char* kCacheRoot = "/.cache";
  static constexpr const char* kCacheDir = "/.cache/tasks";
  static constexpr const char* kCachePath = "/.cache/tasks/data.json";
  static constexpr int kDefaultDays = 7;
  static constexpr int kDefaultRefreshMinutes = 15;
  static constexpr size_t kMaxFileBytes = 4u * 1024 * 1024;
  static constexpr uint32_t kConnectTimeoutMs = 30000;
  static constexpr uint32_t kTimeSyncTimeoutMs = 15000;
  // Attempts to save when someone else changes data.json at the same time.
  static constexpr int kMaxSaveAttempts = 3;

  enum class Screen : uint8_t { List, Menu, Details };
  enum class Phase : uint8_t { Idle, Loading, Connecting, SyncingTime, Downloading, Saving };
  enum class MenuItem : uint8_t { MarkDone, Details };

  struct Row {
    enum class Kind : uint8_t { Heading, Task, Empty };
    Kind kind = Kind::Task;
    std::string text;
    std::string detail;  // the line under a task's title
    size_t task = 0;     // index into data().tasks, for Task rows
  };

  TasksApp(const paperos::Config& config, paperos::FileSystem& fs, paperos::Network& network,
           paperos::Clock& clock, const paperos::Display& display,
           const paperos::Battery& battery);

  const char* name() const override { return "Tasks"; }
  void onOpen() override;
  void onClose() override;
  bool onInput(paperos::InputEvent event) override;
  bool tick() override;
  bool wantsFullRefresh() const override { return fullRefreshPending_; }
  void draw(paperos::Display& display) override;

  // For tests.
  Screen screen() const { return screen_; }
  Phase phase() const { return phase_; }
  const mindwtr::Data& data() const { return data_; }
  const std::vector<Row>& rows() const { return rows_; }
  // The selected task, or null.
  const mindwtr::Task* selectedTask() const;
  std::string statusText() const;
  size_t menuIndex() const { return menuIndex_; }
  size_t detailPage() const { return detailPage_; }
  size_t detailPageCount() const { return detailPages_.size(); }

 private:
  enum class Operation : uint8_t { Refresh, MarkDone };
  struct Line {
    std::string text;
    paperos::TextSize size;
    paperos::Color color;
    int gapBefore;
  };

  void readSettings();
  void stopWork();
  bool loadStep();
  void startNetwork(Operation op);
  bool networkStep();
  void startDownload();
  bool downloadStep();
  void finishDownload();
  void saveDone();
  void finishNetwork();
  bool applyText(const std::string& text, std::string& error);
  void saveCache(const std::string& text);
  void clearBody();

  void rebuildRows();
  void buildPages();
  void moveSelection(int step);
  void buildDetails();
  std::string dateLabel(int64_t utc, bool hasTime) const;
  std::string taskDetail(const mindwtr::Task& task, bool upcoming) const;
  std::vector<std::string> wrap(const std::string& text, paperos::TextSize size,
                                int maxWidth) const;
  int rowHeight(const Row& row) const;
  int bodyTop() const;
  int bodyBottom() const;
  paperos::NetworkState wifiState() const;

  void drawHeader(paperos::Display& display) const;
  void drawFooter(paperos::Display& display, const char* legend, const std::string& page) const;
  void drawList(paperos::Display& display);
  void drawMenu(paperos::Display& display);
  void drawDetails(paperos::Display& display);

  const paperos::Config& config_;
  paperos::FileSystem& fs_;
  paperos::Network& network_;
  paperos::Clock& clock_;
  const paperos::Display& display_;
  const paperos::Battery& battery_;

  // Settings.
  paperos::DavSettings dav_;
  std::string fileName_;
  int days_ = kDefaultDays;
  int refreshMinutes_ = kDefaultRefreshMinutes;

  // Tasks and what's on screen.
  mindwtr::Data data_;
  bool loaded_ = false;
  std::vector<Row> rows_;
  std::vector<std::vector<size_t>> pages_;  // row indexes on each page
  int selectedRow_ = -1;                    // index into rows_, or -1
  size_t listPage_ = 0;                     // page shown when nothing is selected
  int64_t listsValidUntil_ = 0;             // when "today" changes, rebuild
  bool listsTimeKnown_ = false;
  Screen screen_ = Screen::List;
  std::string menuTaskId_;
  size_t menuIndex_ = 0;
  std::vector<Line> detailLines_;
  std::vector<size_t> detailPages_;  // first line of each page
  size_t detailPage_ = 0;
  bool fullRefreshPending_ = false;

  // Loading and downloading.
  Phase phase_ = Phase::Idle;
  Operation operation_ = Operation::Refresh;
  std::string doneId_;  // the task being marked done
  int saveAttempts_ = 0;
  uint32_t phaseStartedMs_ = 0;
  uint32_t lastAttemptMs_ = 0;
  bool attempted_ = false;
  std::unique_ptr<paperos::File> loadFile_;
  std::unique_ptr<paperos::HttpResponse> response_;
  std::string body_;
  std::string etag_;
  int64_t lastSync_ = -1;
  std::string error_;
  std::string notice_;
};

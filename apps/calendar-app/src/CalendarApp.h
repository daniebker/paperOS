#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <paperos.h>

#include "Agenda.h"
#include "Ics.h"

// Shows the upcoming events from one or more iCal (.ics) links, set in
// config.toml:
//
//   [calendar]
//     days = 14              # how far ahead to show
//     refresh_minutes = 60   # how often to download again while open
//
//   [[calendar.feed]]
//     name = "Personal"
//     url = "https://calendar.example.com/basic.ics"
//
// Calendars are downloaded over Wi-Fi (see [wifi]) into /calendar on the SD
// card, so the agenda still shows the last copy when offline. Opening the app
// shows the cached copy at once and downloads again if it is older than
// refresh_minutes.
//
// A / B turn pages, hold A downloads now and hold B returns home.
class CalendarApp : public paperos::App {
 public:
  static constexpr const char* kCacheDir = "/calendar";
  static constexpr int kDefaultDays = 14;
  static constexpr int kDefaultRefreshMinutes = 60;

  // What the app is busy with. Idle covers showing the agenda and messages.
  enum class Phase : uint8_t { Idle, Loading, Connecting, SyncingTime, Downloading };

  struct Feed {
    std::string name;
    std::string url;
    std::string path;  // cached copy on the SD card
  };

  // One line of the agenda.
  struct Row {
    enum class Kind : uint8_t { Day, Event, Empty };
    Kind kind = Kind::Event;
    std::string time;    // "09:00 - 10:00", "All day", ... (events only)
    std::string text;    // day name, event title or "No events"
    std::string detail;  // location and calendar name, may be empty
  };

  CalendarApp(const paperos::Config& config, paperos::FileSystem& fs, paperos::Network& network,
              paperos::Clock& clock, const paperos::Display& display,
              const paperos::Battery& battery);

  const char* name() const override { return "Calendar"; }
  void onOpen() override;
  void onClose() override;
  bool onInput(paperos::InputEvent event) override;
  bool tick() override;
  bool wantsFullRefresh() const override { return fullRefreshPending_; }
  void draw(paperos::Display& display) override;

  Phase phase() const { return phase_; }
  const std::vector<Feed>& feeds() const { return feeds_; }
  const std::vector<calendar::Occurrence>& occurrences() const { return occurrences_; }
  size_t pageCount() const { return pages_.size(); }
  size_t currentPage() const { return page_; }  // zero-based
  const std::vector<Row>& pageRows(size_t page) const { return pages_[page]; }
  // Short note for the footer: when the agenda was updated, or what failed.
  std::string statusText() const;
  // Wi-Fi state shown in the header.
  paperos::NetworkState wifiState() const;
  // Unix time of the last download, or 0 if never.
  int64_t lastSync() const { return lastSync_; }

  // Cache file for a feed URL.
  static std::string cachePath(const std::string& url);

 private:
  void readFeeds();
  bool startLoading();
  bool loadStep();
  void finishLoading();
  bool shouldSync() const;
  void startSync();
  bool syncStep();
  bool downloadStep();
  void openNextDownload();
  void finishDownload(bool ok, std::string error);
  void stopWork();
  void finishSync();
  void removeStaleCaches();
  void loadSyncTime();
  void saveSyncTime();
  void buildPages();
  std::vector<Row> buildRows() const;
  int rowHeight(const Row& row) const;
  int bodyTop() const;
  int bodyBottom() const;

  void drawRow(paperos::Display& display, const Row& row, int y) const;
  void drawMessage(paperos::Display& display, const char* title, const char* detail) const;

  const paperos::Config& config_;
  paperos::FileSystem& fs_;
  paperos::Network& network_;
  paperos::Clock& clock_;
  const paperos::Display& display_;
  const paperos::Battery& battery_;

  std::vector<Feed> feeds_;
  int days_ = kDefaultDays;
  int refreshMinutes_ = kDefaultRefreshMinutes;

  Phase phase_ = Phase::Idle;
  uint32_t phaseStartedMs_ = 0;
  uint32_t lastCheckMs_ = 0;

  // Loading the cached files.
  std::unique_ptr<calendar::AgendaBuilder> builder_;
  std::unique_ptr<calendar::IcsParser> parser_;
  std::unique_ptr<paperos::File> loadFile_;
  size_t loadIndex_ = 0;
  int64_t loadingWindowStart_ = 0;
  bool loaded_ = false;  // occurrences_ holds a finished load
  int64_t windowStart_ = 0;  // local civil seconds of the first day's midnight

  // Downloading.
  size_t downloadIndex_ = 0;
  std::unique_ptr<paperos::HttpResponse> response_;
  std::unique_ptr<paperos::File> downloadFile_;
  std::string downloadHead_;  // first bytes, to check it is a calendar
  size_t downloaded_ = 0;  // feeds downloaded in this sync
  bool syncAttempted_ = false;
  uint32_t syncAttemptMs_ = 0;
  std::vector<std::string> errors_;  // from the last sync
  int64_t lastSync_ = 0;

  std::vector<calendar::Occurrence> occurrences_;
  std::vector<std::vector<Row>> pages_;
  size_t page_ = 0;
  bool fullRefreshPending_ = false;
};

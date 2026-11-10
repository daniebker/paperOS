#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <paperos.h>

#include "Feed.h"

// Reads RSS and Atom feeds set in config.toml:
//
//   [rss]
//     refresh_minutes = 60   # how often to download again while open
//     articles = 20          # how many to keep from each feed
//
//   [[rss.feed]]
//     name = "Example"       # optional: the feed's own title otherwise
//     url = "https://example.com/feed.xml"
//
// Like the Calendar, feeds are downloaded over Wi-Fi into /.cache/rss on the
// SD card, and the article list built from them is cached there too and
// served stale-while-revalidate: opening the app shows the cached list at
// once, without reading the feeds, then downloads again in the background if
// the copy is older than refresh_minutes and redraws with the new list. Each
// article's text stays on the SD card until it's opened.
//
// The list shows every feed's articles, newest first, with a mark on unread
// ones. A / B move, hold A opens the article, and hold B goes back.
class RssApp : public paperos::App {
 public:
  static constexpr const char* kCacheRoot = "/.cache";
  static constexpr const char* kCacheDir = "/.cache/rss";
  static constexpr const char* kIndexPath = "/.cache/rss/articles.bin";
  static constexpr const char* kTextPath = "/.cache/rss/articles.txt";
  static constexpr const char* kSyncTimePath = "/.cache/rss/synced";
  static constexpr const char* kReadPath = "/.cache/rss/read";
  static constexpr int kDefaultRefreshMinutes = 60;
  static constexpr int kDefaultArticles = 20;
  static constexpr size_t kMaxFeeds = 16;
  static constexpr size_t kMaxTextChars = 16 * 1024;

  enum class Screen : uint8_t { List, Article };
  enum class Phase : uint8_t { Idle, Loading, Connecting, Downloading };

  struct Feed {
    std::string name;
    std::string url;
    std::string path;  // downloaded copy on the SD card
  };

  // One article in the list. Its text is in kTextPath.
  struct Entry {
    std::string title;
    std::string link;
    int64_t published = rss::kNoTime;
    uint8_t feed = 0;
    uint32_t id = 0;  // hash of the feed and the article's id
    uint32_t textOffset = 0;
    uint32_t textLength = 0;
  };

  RssApp(const paperos::Config& config, paperos::FileSystem& fs, paperos::Network& network,
         paperos::Clock& clock, const paperos::Display& display,
         const paperos::Battery& battery);

  const char* name() const override { return "RSS"; }
  void onOpen() override;
  void onClose() override;
  bool onInput(paperos::InputEvent event) override;
  bool tick() override;
  bool wantsFullRefresh() const override { return fullRefreshPending_; }
  void draw(paperos::Display& display) override;

  // For tests.
  Screen screen() const { return screen_; }
  Phase phase() const { return phase_; }
  bool loaded() const { return loaded_; }
  const std::vector<Entry>& entries() const { return entries_; }
  const std::vector<Feed>& feeds() const { return feeds_; }
  size_t selected() const { return selected_; }
  bool isRead(const Entry& entry) const { return read_.count(entry.id) > 0; }
  size_t unreadCount() const;
  std::string statusText() const;
  size_t articlePage() const { return articlePage_; }
  size_t articlePageCount() const { return articlePages_.size(); }
  // The text of an article, read from the SD card.
  std::string articleText(const Entry& entry) const;
  static std::string cachePath(const std::string& url);

 private:
  struct Line {
    std::string text;
    paperos::TextSize size;
    paperos::Color color;
    int gapBefore;
  };

  void readFeeds();
  void stopWork();
  std::string feedName(size_t index) const;

  // Building the list from the downloaded feeds.
  bool startLoading();
  bool loadStep();
  void takeArticles();
  void finishLoading();
  uint32_t indexKey() const;
  bool loadIndex();
  void saveIndex() const;

  // Downloading.
  bool shouldSync() const;
  void startSync();
  bool syncStep();
  void openNextDownload();
  bool downloadStep();
  void finishDownload(bool ok, std::string error);
  void finishSync();
  void removeStaleCaches();
  void loadSyncTime();
  void saveSyncTime();

  // Read articles.
  void loadReadState();
  void saveReadState();

  // Layout.
  std::string whenText(int64_t utc) const;
  std::vector<std::string> wrap(const std::string& text, paperos::TextSize size,
                                int maxWidth) const;
  std::vector<std::string> titleLines(const Entry& entry) const;
  int rowHeight(const Entry& entry) const;
  int bodyTop() const;
  int bodyBottom() const;
  void buildPages();
  size_t pageOfSelected() const;
  void openArticle();
  paperos::NetworkState wifiState() const;

  void drawHeader(paperos::Display& display) const;
  void drawFooter(paperos::Display& display, const char* legend, const std::string& page) const;
  void drawList(paperos::Display& display);
  void drawArticle(paperos::Display& display);

  const paperos::Config& config_;
  paperos::FileSystem& fs_;
  paperos::Network& network_;
  paperos::Clock& clock_;
  const paperos::Display& display_;
  const paperos::Battery& battery_;

  // Settings.
  std::vector<Feed> feeds_;
  int refreshMinutes_ = kDefaultRefreshMinutes;
  size_t articlesPerFeed_ = kDefaultArticles;

  // The list.
  std::vector<Entry> entries_;
  std::vector<std::string> feedTitles_;  // from the feeds themselves
  bool loaded_ = false;
  std::set<uint32_t> read_;
  std::vector<size_t> pageStarts_;  // first entry of each list page
  size_t selected_ = 0;
  Screen screen_ = Screen::List;
  std::vector<Line> articleLines_;
  std::vector<size_t> articlePages_;  // first line of each page
  size_t articlePage_ = 0;
  bool fullRefreshPending_ = false;
  uint32_t lastCheckMs_ = 0;

  // Building the list.
  Phase phase_ = Phase::Idle;
  size_t loadIndex_ = 0;
  size_t loadedFeeds_ = 0;
  std::unique_ptr<paperos::File> loadFile_;
  std::unique_ptr<rss::FeedParser> parser_;
  std::unique_ptr<paperos::File> textPart_;
  uint32_t textSize_ = 0;
  bool textFailed_ = false;
  std::vector<Entry> newEntries_;
  std::vector<std::string> newFeedTitles_;

  // Downloading.
  size_t downloadIndex_ = 0;
  size_t downloaded_ = 0;
  std::unique_ptr<paperos::HttpResponse> response_;
  std::unique_ptr<paperos::File> downloadFile_;
  std::string downloadHead_;
  uint32_t phaseStartedMs_ = 0;
  bool syncAttempted_ = false;
  uint32_t syncAttemptMs_ = 0;
  int64_t lastSync_ = 0;
  std::vector<std::string> errors_;
};

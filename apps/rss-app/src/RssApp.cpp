#include "RssApp.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>

using paperos::Color;
using paperos::FileMode;
using paperos::HttpResponse;
using paperos::InputEvent;
using paperos::NetworkState;
using paperos::TextAlign;
using paperos::TextSize;

namespace {

constexpr int kMargin = 24;
constexpr int kRowPad = 10;
constexpr int kMaxTitleLines = 2;
constexpr size_t kMaxArticlesPerFeed = 100;
// Work done per tick, small enough that the buttons stay responsive.
constexpr size_t kLoadBytesPerTick = 16 * 1024;
constexpr int kDownloadReadsPerTick = 8;
// How often an idle list checks whether a download is due.
constexpr uint32_t kIdleCheckMs = 30 * 1000;
constexpr uint32_t kConnectTimeoutMs = 30 * 1000;
constexpr const char* kFeedTable = "rss.feed";
// Cached list: magic, format version, then the fields saveIndex() writes.
constexpr char kIndexMagic[4] = {'P', 'R', 'S', 'S'};
constexpr uint8_t kIndexVersion = 1;
constexpr uint32_t kMaxIndexBytes = 2 * 1024 * 1024;

char chunk[2048];

uint32_t fnv1a(const std::string& text, uint32_t hash = 2166136261u) {
  for (char c : text) hash = (hash ^ uint8_t(c)) * 16777619u;
  return hash;
}

void putInt(std::string& out, uint64_t value, int bytes) {
  for (int i = 0; i < bytes; ++i) out.push_back(char(uint8_t(value >> (8 * i))));
}

void putString(std::string& out, const std::string& text) {
  putInt(out, text.size(), 4);
  out += text;
}

// Reads what putInt() and putString() wrote. ok() turns false, and stays so,
// on reading past the end.
class CacheReader {
 public:
  explicit CacheReader(const std::string& data) : data_(data) {}
  uint64_t getInt(int bytes) {
    if (!ok_ || data_.size() - pos_ < size_t(bytes)) {
      ok_ = false;
      return 0;
    }
    uint64_t value = 0;
    for (int i = 0; i < bytes; ++i) value |= uint64_t(uint8_t(data_[pos_ + i])) << (8 * i);
    pos_ += size_t(bytes);
    return value;
  }
  std::string getString() {
    const uint64_t size = getInt(4);
    if (!ok_ || data_.size() - pos_ < size) {
      ok_ = false;
      return {};
    }
    std::string text = data_.substr(pos_, size_t(size));
    pos_ += size_t(size);
    return text;
  }
  bool ok() const { return ok_; }
  bool atEnd() const { return pos_ == data_.size(); }

 private:
  const std::string& data_;
  size_t pos_ = 0;
  bool ok_ = true;
};

std::string fitToWidth(const paperos::Display& display, std::string text, TextSize size,
                       int maxWidth) {
  if (display.textWidth(text.c_str(), size) <= maxWidth) return text;
  while (!text.empty() && display.textWidth((text + "...").c_str(), size) > maxWidth) {
    text.pop_back();
  }
  return text + "...";
}

int64_t localDay(int64_t utc) {
  const time_t seconds = time_t(utc);
  struct tm local;
  localtime_r(&seconds, &local);
  return int64_t(local.tm_year) * 1000 + local.tm_yday;
}

}  // namespace

RssApp::RssApp(const paperos::Config& config, paperos::FileSystem& fs, paperos::Network& network,
               paperos::Clock& clock, const paperos::Display& display,
               const paperos::Battery& battery)
    : config_(config),
      fs_(fs),
      network_(network),
      clock_(clock),
      display_(display),
      battery_(battery) {}

std::string RssApp::cachePath(const std::string& url) {
  char path[40];
  std::snprintf(path, sizeof(path), "%s/%08x.xml", kCacheDir, unsigned(fnv1a(url)));
  return path;
}

void RssApp::readFeeds() {
  feeds_.clear();
  const size_t count = config_.tableCount(kFeedTable);
  for (size_t i = 0; i < count && feeds_.size() < kMaxFeeds; ++i) {
    Feed feed;
    feed.url = config_.getTableString(kFeedTable, i, "url");
    if (feed.url.empty()) continue;
    feed.name = paperos::foldUtf8ToAscii(config_.getTableString(kFeedTable, i, "name"));
    feed.path = cachePath(feed.url);
    feeds_.push_back(feed);
  }
  refreshMinutes_ =
      int(std::max<long>(0, config_.getInt("rss", "refresh_minutes", kDefaultRefreshMinutes)));
  articlesPerFeed_ = size_t(std::min<long>(
      kMaxArticlesPerFeed, std::max<long>(1, config_.getInt("rss", "articles", kDefaultArticles))));
}

std::string RssApp::feedName(size_t index) const {
  if (index >= feeds_.size()) return "";
  if (!feeds_[index].name.empty()) return feeds_[index].name;
  if (index < feedTitles_.size() && !feedTitles_[index].empty()) return feedTitles_[index];
  return "Feed " + std::to_string(index + 1);
}

// --- Opening and closing -----------------------------------------------------

void RssApp::onOpen() {
  readFeeds();
  entries_.clear();
  feedTitles_.clear();
  loaded_ = false;
  selected_ = 0;
  screen_ = Screen::List;
  errors_.clear();
  syncAttempted_ = false;
  fullRefreshPending_ = false;
  phase_ = Phase::Idle;
  lastCheckMs_ = clock_.uptimeMs();

  // Both fail harmlessly if they exist.
  fs_.makeDir(kCacheRoot);
  fs_.makeDir(kCacheDir);
  loadSyncTime();
  loadReadState();
  // Stale-while-revalidate: show the cached list at once, then download in
  // the background if it is due. Without a usable cached list, build one
  // from the downloaded feeds first.
  if (loadIndex()) {
    if (shouldSync()) startSync();
  } else if (!startLoading() && shouldSync()) {
    startSync();
  }
}

void RssApp::onClose() {
  stopWork();
  entries_.clear();
  articleLines_.clear();
}

void RssApp::stopWork() {
  if (downloadFile_) {
    downloadFile_.reset();
    fs_.remove((feeds_[downloadIndex_].path + ".part").c_str());
  }
  response_.reset();
  if (phase_ == Phase::Connecting || phase_ == Phase::Downloading) network_.disconnect();
  if (textPart_) {
    textPart_.reset();
    fs_.remove((std::string(kTextPath) + ".part").c_str());
  }
  loadFile_.reset();
  parser_.reset();
  newEntries_.clear();
  phase_ = Phase::Idle;
}

// --- Building the list from the downloaded feeds ------------------------------

bool RssApp::startLoading() {
  if (feeds_.empty()) return false;
  textPart_ = fs_.open((std::string(kTextPath) + ".part").c_str(), FileMode::Write);
  if (!textPart_) {
    errors_.assign(1, "Could not write to the SD card");
    return false;
  }
  textSize_ = 0;
  textFailed_ = false;
  loadIndex_ = 0;
  loadedFeeds_ = 0;
  loadFile_.reset();
  parser_.reset();
  newEntries_.clear();
  newFeedTitles_.assign(feeds_.size(), "");
  phase_ = Phase::Loading;
  return true;
}

void RssApp::takeArticles() {
  for (rss::Article& article : parser_->take()) {
    Entry entry;
    entry.title = article.title;
    entry.link = article.link;
    entry.published = article.published;
    entry.feed = uint8_t(loadIndex_);
    entry.id = fnv1a(feeds_[loadIndex_].url + "\n" + article.id);
    entry.textOffset = textSize_;
    entry.textLength = uint32_t(article.text.size());
    if (textPart_->write(article.text.data(), article.text.size()) != article.text.size()) {
      textFailed_ = true;
    }
    textSize_ += entry.textLength;
    newEntries_.push_back(std::move(entry));
  }
}

bool RssApp::loadStep() {
  size_t budget = kLoadBytesPerTick;
  while (budget > 0) {
    if (!loadFile_) {
      if (loadIndex_ >= feeds_.size()) {
        finishLoading();
        return true;
      }
      loadFile_ = fs_.open(feeds_[loadIndex_].path.c_str(), FileMode::Read);
      if (!loadFile_) {  // not downloaded yet
        ++loadIndex_;
        continue;
      }
      ++loadedFeeds_;
      parser_.reset(new rss::FeedParser(articlesPerFeed_, kMaxTextChars));
    }
    const size_t n = parser_->full() ? 0 : loadFile_->read(chunk, sizeof(chunk));
    if (n == 0) {
      parser_->finish();
      takeArticles();
      newFeedTitles_[loadIndex_] = parser_->feedTitle();
      parser_.reset();
      loadFile_.reset();
      ++loadIndex_;
      continue;
    }
    parser_->feed(chunk, n);
    takeArticles();
    budget -= std::min(n, budget);
  }
  return false;
}

void RssApp::finishLoading() {
  textPart_.reset();
  phase_ = Phase::Idle;
  const std::string part = std::string(kTextPath) + ".part";
  if (loadedFeeds_ == 0 || textFailed_) {
    // Nothing downloaded yet, or the card is full: keep what there is.
    fs_.remove(part.c_str());
    newEntries_.clear();
    if (textFailed_) errors_.assign(1, "Could not write to the SD card");
    return;
  }
  fs_.remove(kTextPath);
  if (!fs_.rename(part.c_str(), kTextPath)) {
    fs_.remove(part.c_str());
    fs_.remove(kIndexPath);
    errors_.assign(1, "Could not write to the SD card");
    return;
  }
  // Newest first; undated articles after, in feed order.
  std::stable_sort(newEntries_.begin(), newEntries_.end(), [](const Entry& a, const Entry& b) {
    if ((a.published == rss::kNoTime) != (b.published == rss::kNoTime)) {
      return a.published != rss::kNoTime;
    }
    return a.published > b.published;
  });

  // Keep the same article selected.
  const uint32_t selectedId = selected_ < entries_.size() ? entries_[selected_].id : 0;
  entries_ = std::move(newEntries_);
  newEntries_.clear();
  feedTitles_ = newFeedTitles_;
  selected_ = 0;
  for (size_t i = 0; i < entries_.size(); ++i) {
    if (entries_[i].id == selectedId) selected_ = i;
  }
  loaded_ = true;
  buildPages();
  if (screen_ == Screen::List) fullRefreshPending_ = true;
  saveIndex();

  // Forget read marks of articles no longer in any feed.
  std::set<uint32_t> kept;
  for (const Entry& entry : entries_) {
    if (read_.count(entry.id) > 0) kept.insert(entry.id);
  }
  if (kept.size() != read_.size()) {
    read_ = std::move(kept);
    saveReadState();
  }
}

// --- The cached list -------------------------------------------------------------

// Changes whenever something other than the feed files changes what the list
// holds, so a cached list from another setup is not shown.
uint32_t RssApp::indexKey() const {
  std::string key = std::to_string(articlesPerFeed_);
  for (const Feed& feed : feeds_) key += "\n" + feed.url;
  return fnv1a(key);
}

bool RssApp::loadIndex() {
  if (feeds_.empty()) return false;
  std::unique_ptr<paperos::File> file = fs_.open(kIndexPath, FileMode::Read);
  if (!file || file->size() > kMaxIndexBytes) return false;
  std::string data(file->size(), '\0');
  if (file->read(&data[0], data.size()) != data.size()) return false;
  file.reset();

  CacheReader in(data);
  for (char c : kIndexMagic) {
    if (in.getInt(1) != uint8_t(c)) return false;
  }
  if (in.getInt(1) != kIndexVersion || in.getInt(4) != indexKey()) return false;
  // The text file must be the one written with this index.
  const uint32_t textSize = uint32_t(in.getInt(4));
  std::unique_ptr<paperos::File> text = fs_.open(kTextPath, FileMode::Read);
  if (!text || text->size() != textSize) return false;
  text.reset();

  const size_t feedCount = size_t(in.getInt(1));
  if (!in.ok() || feedCount != feeds_.size()) return false;
  std::vector<std::string> titles(feedCount);
  for (std::string& title : titles) title = in.getString();
  const uint64_t count = in.getInt(4);
  if (!in.ok() || count > kMaxFeeds * kMaxArticlesPerFeed) return false;

  std::vector<Entry> entries(static_cast<size_t>(count));
  for (Entry& e : entries) {
    e.published = int64_t(in.getInt(8));
    e.feed = uint8_t(in.getInt(1));
    e.id = uint32_t(in.getInt(4));
    e.textOffset = uint32_t(in.getInt(4));
    e.textLength = uint32_t(in.getInt(4));
    e.title = in.getString();
    e.link = in.getString();
    if (e.feed >= feeds_.size() || uint64_t(e.textOffset) + e.textLength > textSize) return false;
  }
  if (!in.ok() || !in.atEnd()) return false;

  entries_ = std::move(entries);
  feedTitles_ = std::move(titles);
  loaded_ = true;
  selected_ = 0;
  buildPages();
  return true;
}

void RssApp::saveIndex() const {
  std::string data(kIndexMagic, sizeof(kIndexMagic));
  putInt(data, kIndexVersion, 1);
  putInt(data, indexKey(), 4);
  putInt(data, textSize_, 4);
  putInt(data, feeds_.size(), 1);
  for (size_t i = 0; i < feeds_.size(); ++i) {
    putString(data, i < feedTitles_.size() ? feedTitles_[i] : "");
  }
  putInt(data, entries_.size(), 4);
  for (const Entry& e : entries_) {
    putInt(data, uint64_t(e.published), 8);
    putInt(data, e.feed, 1);
    putInt(data, e.id, 4);
    putInt(data, e.textOffset, 4);
    putInt(data, e.textLength, 4);
    putString(data, e.title);
    putString(data, e.link);
  }

  // Like a download, only replace the old copy once the new one is complete.
  const std::string part = std::string(kIndexPath) + ".part";
  std::unique_ptr<paperos::File> file = fs_.open(part.c_str(), FileMode::Write);
  if (!file) return;
  const bool written = file->write(data.data(), data.size()) == data.size();
  file.reset();
  if (written) {
    fs_.remove(kIndexPath);
    if (fs_.rename(part.c_str(), kIndexPath)) return;
  }
  fs_.remove(part.c_str());
  fs_.remove(kIndexPath);  // never leave a cached list that may be out of date
}

std::string RssApp::articleText(const Entry& entry) const {
  std::unique_ptr<paperos::File> file = fs_.open(kTextPath, FileMode::Read);
  if (!file || !file->seek(entry.textOffset)) return "";
  std::string text(entry.textLength, '\0');
  if (entry.textLength > 0) text.resize(file->read(&text[0], entry.textLength));
  return text;
}

// --- Read articles ------------------------------------------------------------------

void RssApp::loadReadState() {
  read_.clear();
  std::unique_ptr<paperos::File> file = fs_.open(kReadPath, FileMode::Read);
  if (!file || file->size() > 64 * 1024) return;
  std::string data(file->size(), '\0');
  data.resize(file->read(&data[0], data.size()));
  size_t start = 0;
  while (start < data.size()) {
    size_t end = data.find('\n', start);
    if (end == std::string::npos) end = data.size();
    if (end > start) read_.insert(uint32_t(std::strtoul(data.substr(start, end - start).c_str(),
                                                        nullptr, 16)));
    start = end + 1;
  }
}

void RssApp::saveReadState() {
  std::string data;
  char line[16];
  for (const uint32_t id : read_) {
    std::snprintf(line, sizeof(line), "%08x\n", unsigned(id));
    data += line;
  }
  std::unique_ptr<paperos::File> file = fs_.open(kReadPath, FileMode::Write);
  if (file) file->write(data.data(), data.size());
}

size_t RssApp::unreadCount() const {
  size_t count = 0;
  for (const Entry& e : entries_) count += read_.count(e.id) == 0 ? 1 : 0;
  return count;
}

// --- Downloading ----------------------------------------------------------------

bool RssApp::shouldSync() const {
  if (feeds_.empty() || !network_.configured()) return false;
  if (syncAttempted_) {
    // Wait a full interval after the last try, even if it failed.
    const uint32_t elapsed = clock_.uptimeMs() - syncAttemptMs_;
    if (refreshMinutes_ == 0 || elapsed < uint32_t(refreshMinutes_) * 60u * 1000u) return false;
  }
  const int64_t now = clock_.now();
  if (now < 0 || lastSync_ <= 0 || now < lastSync_) return true;
  return refreshMinutes_ > 0 && now - lastSync_ >= int64_t(refreshMinutes_) * 60;
}

void RssApp::startSync() {
  errors_.clear();
  downloaded_ = 0;
  syncAttempted_ = true;
  syncAttemptMs_ = clock_.uptimeMs();
  phaseStartedMs_ = syncAttemptMs_;
  phase_ = Phase::Connecting;
  network_.connect();
}

bool RssApp::syncStep() {
  if (phase_ == Phase::Connecting) {
    const NetworkState state = network_.state();
    if (state == NetworkState::Connected) {
      // Articles show their dates once the time is known.
      if (clock_.now() < 0) clock_.sync();
      phase_ = Phase::Downloading;
      downloadIndex_ = 0;
      openNextDownload();
      return true;  // the header shows Wi-Fi as connected
    }
    if (state == NetworkState::Failed ||
        clock_.uptimeMs() - phaseStartedMs_ > kConnectTimeoutMs) {
      errors_.push_back("Could not connect to Wi-Fi");
      finishSync();
      return true;
    }
    return false;
  }
  if (phase_ == Phase::Downloading) return downloadStep();
  return false;
}

void RssApp::openNextDownload() {
  while (downloadIndex_ < feeds_.size()) {
    const Feed& feed = feeds_[downloadIndex_];
    std::string error;
    response_ = network_.get(feed.url.c_str(), error);
    if (!response_) {
      errors_.push_back(feedName(downloadIndex_) + ": " + error);
      ++downloadIndex_;
      continue;
    }
    downloadFile_ = fs_.open((feed.path + ".part").c_str(), FileMode::Write);
    if (!downloadFile_) {
      response_.reset();
      errors_.push_back(feedName(downloadIndex_) + ": could not write to the SD card");
      ++downloadIndex_;
      continue;
    }
    downloadHead_.clear();
    return;
  }
  finishSync();
}

bool RssApp::downloadStep() {
  for (int i = 0; i < kDownloadReadsPerTick; ++i) {
    const int n = response_->read(chunk, sizeof(chunk));
    if (n == 0) return false;
    if (n == HttpResponse::kEnd) {
      finishDownload(true, "");
      return phase_ != Phase::Downloading;
    }
    if (n < 0) {
      finishDownload(false, "download interrupted");
      return phase_ != Phase::Downloading;
    }
    if (downloadHead_.size() < 64) {
      downloadHead_.append(chunk, std::min<size_t>(size_t(n), 64 - downloadHead_.size()));
    }
    if (downloadFile_->write(chunk, size_t(n)) != size_t(n)) {
      finishDownload(false, "could not write to the SD card");
      return phase_ != Phase::Downloading;
    }
  }
  return false;
}

void RssApp::finishDownload(bool ok, std::string error) {
  const Feed& feed = feeds_[downloadIndex_];
  const std::string part = feed.path + ".part";
  response_.reset();
  downloadFile_.reset();
  if (ok && !rss::looksLikeFeed(downloadHead_)) {
    ok = false;
    error = "not a feed, check the url";
  }
  if (ok) {
    // Only replace the old copy once the new one is complete.
    fs_.remove(feed.path.c_str());
    ok = fs_.rename(part.c_str(), feed.path.c_str());
    if (!ok) error = "could not write to the SD card";
  }
  if (ok) {
    ++downloaded_;
  } else {
    fs_.remove(part.c_str());
    errors_.push_back(feedName(downloadIndex_) + ": " + error);
  }
  ++downloadIndex_;
  openNextDownload();
}

void RssApp::finishSync() {
  network_.disconnect();
  phase_ = Phase::Idle;
  if (downloaded_ > 0) {
    lastSync_ = clock_.now();
    saveSyncTime();
    removeStaleCaches();
  }
  if (downloaded_ > 0 || !loaded_) startLoading();
}

void RssApp::removeStaleCaches() {
  std::vector<paperos::DirEntry> entries;
  if (!fs_.list(kCacheDir, entries)) return;
  for (const paperos::DirEntry& entry : entries) {
    const std::string path = std::string(kCacheDir) + "/" + entry.name;
    const bool isFeed = entry.name.size() > 4 &&
                        entry.name.compare(entry.name.size() - 4, 4, ".xml") == 0;
    if (entry.isDirectory || !isFeed) continue;
    const bool inUse = std::any_of(feeds_.begin(), feeds_.end(),
                                   [&](const Feed& feed) { return feed.path == path; });
    if (!inUse) fs_.remove(path.c_str());
  }
}

void RssApp::loadSyncTime() {
  lastSync_ = 0;
  std::unique_ptr<paperos::File> file = fs_.open(kSyncTimePath, FileMode::Read);
  if (!file) return;
  char text[24] = {};
  file->read(text, sizeof(text) - 1);
  lastSync_ = std::strtoll(text, nullptr, 10);
}

void RssApp::saveSyncTime() {
  std::unique_ptr<paperos::File> file = fs_.open(kSyncTimePath, FileMode::Write);
  if (!file) return;
  const std::string text = std::to_string(lastSync_);
  file->write(text.data(), text.size());
}

// --- Main loop and input ----------------------------------------------------------

bool RssApp::tick() {
  switch (phase_) {
    case Phase::Loading:
      if (!loadStep()) return false;
      if (shouldSync()) startSync();
      return true;
    case Phase::Connecting:
    case Phase::Downloading:
      return syncStep();
    case Phase::Idle:
      break;
  }
  const uint32_t now = clock_.uptimeMs();
  if (now - lastCheckMs_ < kIdleCheckMs) return false;
  lastCheckMs_ = now;
  if (shouldSync()) {
    startSync();
    return true;
  }
  return false;
}

bool RssApp::onInput(InputEvent event) {
  if (screen_ == Screen::Article) {
    switch (event) {
      case InputEvent::Previous:
        if (articlePage_ == 0) return false;
        --articlePage_;
        return true;
      case InputEvent::Next:
        if (articlePage_ + 1 >= articlePages_.size()) return false;
        ++articlePage_;
        return true;
      case InputEvent::Back:
        screen_ = Screen::List;
        articleLines_.clear();
        fullRefreshPending_ = true;
        return true;
      default:
        return false;
    }
  }

  switch (event) {
    case InputEvent::Previous:
      if (entries_.empty()) return false;
      selected_ = (selected_ + entries_.size() - 1) % entries_.size();
      return true;
    case InputEvent::Next:
      if (entries_.empty()) return false;
      selected_ = (selected_ + 1) % entries_.size();
      return true;
    case InputEvent::Select:
      if (!entries_.empty()) {
        openArticle();
        return true;
      }
      // Nothing to open: download now.
      if (phase_ != Phase::Idle || feeds_.empty()) return false;
      if (!network_.configured()) {
        errors_.assign(1, "Set [wifi] in config.toml to download");
        return true;
      }
      startSync();
      return true;
    default:
      return false;  // Back returns home
  }
}

// --- Layout ----------------------------------------------------------------------

// "Today 09:30", "Yesterday 18:05", "Mon 9 Nov", "Mon 9 Nov 2025"
std::string RssApp::whenText(int64_t utc) const {
  if (utc == rss::kNoTime) return "";
  const time_t seconds = time_t(utc);
  struct tm local;
  localtime_r(&seconds, &local);
  char text[32];
  const int64_t now = clock_.now();
  if (now >= 0) {
    const int64_t day = localDay(utc);
    if (day == localDay(now)) {
      strftime(text, sizeof(text), "Today %H:%M", &local);
      return text;
    }
    if (day == localDay(now - 86400)) {
      strftime(text, sizeof(text), "Yesterday %H:%M", &local);
      return text;
    }
  }
  const time_t nowSeconds = time_t(now >= 0 ? now : utc);
  struct tm current;
  localtime_r(&nowSeconds, &current);
  strftime(text, sizeof(text), local.tm_year == current.tm_year ? "%a %d %b" : "%a %d %b %Y",
           &local);
  std::string label = text;
  if (label.size() > 4 && label[4] == '0') label.erase(4, 1);  // "Mon 9 Nov"
  return label;
}

std::vector<std::string> RssApp::wrap(const std::string& text, TextSize size, int maxWidth) const {
  std::vector<std::string> lines;
  size_t start = 0;
  while (start <= text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    const std::string paragraph = text.substr(start, end - start);
    std::string line;
    size_t i = 0;
    while (i < paragraph.size()) {
      size_t wordEnd = paragraph.find(' ', i);
      if (wordEnd == std::string::npos) wordEnd = paragraph.size();
      std::string word = paragraph.substr(i, wordEnd - i);
      i = wordEnd + 1;
      const std::string candidate = line.empty() ? word : line + " " + word;
      if (display_.textWidth(candidate.c_str(), size) <= maxWidth) {
        line = candidate;
        continue;
      }
      if (!line.empty()) lines.push_back(line);
      // Break a word too long for a line, such as a url.
      while (word.size() > 1 && display_.textWidth(word.c_str(), size) > maxWidth) {
        size_t n = word.size() - 1;
        while (n > 1 && display_.textWidth(word.substr(0, n).c_str(), size) > maxWidth) --n;
        lines.push_back(word.substr(0, n));
        word = word.substr(n);
      }
      line = word;
    }
    lines.push_back(line);
    start = end + 1;
  }
  return lines;
}

std::vector<std::string> RssApp::titleLines(const Entry& entry) const {
  const int maxW = display_.width() - 2 * kMargin - 24;  // room for the unread mark
  std::vector<std::string> lines =
      wrap(entry.title.empty() ? "(No title)" : entry.title, TextSize::Medium, maxW);
  if (lines.size() > size_t(kMaxTitleLines)) {
    lines.resize(kMaxTitleLines);
    lines.back() = fitToWidth(display_, lines.back() + "...", TextSize::Medium, maxW);
  }
  return lines;
}

int RssApp::rowHeight(const Entry& entry) const {
  return int(titleLines(entry).size()) * (display_.textHeight(TextSize::Medium) + 2) + 4 +
         display_.textHeight(TextSize::Small) + 2 * kRowPad;
}

int RssApp::bodyTop() const {
  return kMargin * 2 + display_.textHeight(TextSize::Title) + kMargin / 2;
}

int RssApp::bodyBottom() const {
  const int smallH = display_.textHeight(TextSize::Small);
  return display_.height() - kMargin - 2 * smallH - 12 - 8 - kRowPad;
}

void RssApp::buildPages() {
  pageStarts_.clear();
  int y = bodyTop();
  for (size_t i = 0; i < entries_.size(); ++i) {
    const int h = rowHeight(entries_[i]);
    if (pageStarts_.empty() || y + h > bodyBottom()) {
      pageStarts_.push_back(i);
      y = bodyTop();
    }
    y += h;
  }
  if (selected_ >= entries_.size()) selected_ = 0;
}

size_t RssApp::pageOfSelected() const {
  size_t page = 0;
  for (size_t p = 0; p < pageStarts_.size(); ++p) {
    if (pageStarts_[p] <= selected_) page = p;
  }
  return page;
}

void RssApp::openArticle() {
  const Entry& entry = entries_[selected_];
  if (read_.insert(entry.id).second) saveReadState();

  articleLines_.clear();
  articlePages_.clear();
  articlePage_ = 0;
  const int maxW = display_.width() - 2 * kMargin;
  auto add = [&](const std::string& text, TextSize size, Color color, int gap) {
    bool first = true;
    for (const std::string& line : wrap(text, size, maxW)) {
      articleLines_.push_back({line, size, color, first ? gap : 0});
      first = false;
    }
  };
  add(entry.title.empty() ? "(No title)" : entry.title, TextSize::Large, Color::Black, 0);
  std::string about = feedName(entry.feed);
  const std::string when = whenText(entry.published);
  if (!when.empty()) about += " - " + when;
  add(about, TextSize::Small, Color::DarkGray, 8);
  std::string text = articleText(entry);
  if (text.empty()) text = "This article has no text. Read it at:\n" + entry.link;
  add(text, TextSize::Medium, Color::Black, 24);

  // Pages. Blank lines between paragraphs aren't carried over to a new page.
  int y = bodyTop();
  for (size_t i = 0; i < articleLines_.size(); ++i) {
    const Line& line = articleLines_[i];
    const int h = line.gapBefore + display_.textHeight(line.size);
    if (articlePages_.empty() || y + h > bodyBottom()) {
      if (!articlePages_.empty() && line.text.empty()) continue;
      articlePages_.push_back(i);
      y = bodyTop();
    } else {
      y += line.gapBefore;
    }
    y += display_.textHeight(line.size) + 4;
  }
  screen_ = Screen::Article;
  fullRefreshPending_ = true;
}

// --- Drawing ------------------------------------------------------------------------

std::string RssApp::statusText() const {
  switch (phase_) {
    case Phase::Connecting:
    case Phase::Downloading:
      return "Updating...";
    case Phase::Loading:
      if (!loaded_) return "Loading...";
      break;
    case Phase::Idle:
      break;
  }
  if (!errors_.empty()) {
    std::string text = errors_.front();
    if (errors_.size() > 1) text += " (+" + std::to_string(errors_.size() - 1) + " more)";
    return text;
  }
  if (lastSync_ > 0) {
    const std::string when = whenText(lastSync_);
    return "Updated " + (when.compare(0, 6, "Today ") == 0 ? when.substr(6) : when);
  }
  if (feeds_.empty()) return "";
  return network_.configured() ? "Not downloaded yet" : "Set [wifi] in config.toml to download";
}

paperos::NetworkState RssApp::wifiState() const {
  switch (phase_) {
    case Phase::Connecting:
      return NetworkState::Connecting;
    case Phase::Downloading:
      return NetworkState::Connected;
    default:
      return NetworkState::Off;
  }
}

void RssApp::drawHeader(paperos::Display& display) const {
  const int w = display.width();
  const int titleH = display.textHeight(TextSize::Title);
  const int smallH = display.textHeight(TextSize::Small);
  display.drawText("RSS", kMargin, kMargin, TextSize::Title);
  const int batteryY = kMargin + (titleH - smallH) / 2;
  const int batteryW =
      paperos::drawBatteryLevel(display, battery_, w - kMargin, batteryY, TextAlign::Right);
  int right = w - kMargin - batteryW - (batteryW > 0 ? 12 : 0);
  const int wifiH = smallH * 2 / 3;
  const int wifiW =
      paperos::drawWifiStatus(display, wifiState(), right, batteryY + (smallH - wifiH) / 2, wifiH);
  right -= wifiW + (wifiW > 0 ? 12 : 0);
  if (loaded_ && screen_ == Screen::List) {
    const std::string unread = std::to_string(unreadCount()) + " unread";
    display.drawText(unread.c_str(), right, batteryY, TextSize::Small, TextAlign::Right,
                     Color::DarkGray);
  }
  const int headerBottom = kMargin * 2 + titleH;
  display.fillRect(0, headerBottom - 4, w, 4, Color::Black);
}

void RssApp::drawFooter(paperos::Display& display, const char* legend,
                        const std::string& page) const {
  const int w = display.width();
  const int smallH = display.textHeight(TextSize::Small);
  const int legendY = display.height() - kMargin - smallH;
  const int lineY = legendY - 12;
  const int statusY = lineY - 8 - smallH;
  if (!page.empty()) {
    display.drawText(page.c_str(), w - kMargin, statusY, TextSize::Small, TextAlign::Right,
                     Color::DarkGray);
  }
  const int pageW = page.empty() ? 0 : display.textWidth(page.c_str(), TextSize::Small) + 16;
  const std::string status =
      fitToWidth(display, statusText(), TextSize::Small, w - 2 * kMargin - pageW);
  display.drawText(status.c_str(), kMargin, statusY, TextSize::Small, TextAlign::Left,
                   Color::DarkGray);
  display.drawLine(0, lineY, w, lineY, Color::DarkGray);
  display.drawText(legend, w / 2, legendY, TextSize::Small, TextAlign::Center, Color::DarkGray);
}

void RssApp::drawList(paperos::Display& display) {
  const int w = display.width();
  const int maxW = w - 2 * kMargin;
  if (feeds_.empty() || !loaded_ || entries_.empty()) {
    const char* title;
    std::string detail;
    if (feeds_.empty()) {
      title = "No feeds set up";
      detail = "Add a [[rss.feed]] url to config.toml";
    } else if (!loaded_ && phase_ != Phase::Idle) {
      title = phase_ == Phase::Loading ? "Loading..." : "Getting the news...";
    } else if (!loaded_) {
      title = "Not downloaded yet";
      detail = network_.configured() ? "Hold A to download" : "Set [wifi] in config.toml";
    } else {
      title = "No articles";
      detail = "Hold A to download again";
    }
    const int y = (bodyTop() + bodyBottom()) / 2 - display.textHeight(TextSize::Large);
    display.drawText(title, w / 2, y, TextSize::Large, TextAlign::Center);
    if (!detail.empty()) {
      display.drawText(detail.c_str(), w / 2, y + display.textHeight(TextSize::Large) + 16,
                       TextSize::Small, TextAlign::Center, Color::DarkGray);
    }
    drawFooter(display, "Hold B: back", "");
    return;
  }

  const size_t page = pageOfSelected();
  const size_t first = pageStarts_[page];
  const size_t end = page + 1 < pageStarts_.size() ? pageStarts_[page + 1] : entries_.size();
  const int mediumH = display.textHeight(TextSize::Medium);
  int y = bodyTop();
  for (size_t i = first; i < end; ++i) {
    const Entry& entry = entries_[i];
    const int h = rowHeight(entry);
    const bool selected = i == selected_;
    if (selected) display.fillRect(kMargin / 2, y + 2, w - kMargin, h - 4, Color::Black);
    const Color text = selected ? Color::White : Color::Black;
    const Color dim = selected ? Color::White : Color::DarkGray;
    if (!isRead(entry)) {
      const int dot = 10;
      display.fillRect(kMargin, y + kRowPad + (mediumH - dot) / 2, dot, dot, text);
    }
    int lineY = y + kRowPad;
    for (const std::string& line : titleLines(entry)) {
      display.drawText(line.c_str(), kMargin + 24, lineY, TextSize::Medium, TextAlign::Left, text);
      lineY += mediumH + 2;
    }
    std::string about = feedName(entry.feed);
    const std::string when = whenText(entry.published);
    if (!when.empty()) about += " - " + when;
    display.drawText(fitToWidth(display, about, TextSize::Small, maxW - 24).c_str(), kMargin + 24,
                     lineY + 4, TextSize::Small, TextAlign::Left, dim);
    y += h;
  }
  const std::string pageText = pageStarts_.size() > 1 ? std::to_string(page + 1) + " / " +
                                                            std::to_string(pageStarts_.size())
                                                      : "";
  drawFooter(display, "A / B: move    Hold A: read    Hold B: back", pageText);
}

void RssApp::drawArticle(paperos::Display& display) {
  if (articlePages_.empty()) return;
  const size_t first = articlePages_[articlePage_];
  const size_t end = articlePage_ + 1 < articlePages_.size() ? articlePages_[articlePage_ + 1]
                                                             : articleLines_.size();
  int y = bodyTop();
  for (size_t i = first; i < end; ++i) {
    const Line& line = articleLines_[i];
    if (i != first) y += line.gapBefore;
    display.drawText(line.text.c_str(), kMargin, y, line.size, TextAlign::Left, line.color);
    y += display.textHeight(line.size) + 4;
  }
  const std::string page = articlePages_.size() > 1 ? std::to_string(articlePage_ + 1) + " / " +
                                                          std::to_string(articlePages_.size())
                                                    : "";
  drawFooter(display,
             articlePages_.size() > 1 ? "A / B: page    Hold B: back" : "Hold B: back", page);
}

void RssApp::draw(paperos::Display& display) {
  fullRefreshPending_ = false;
  drawHeader(display);
  if (screen_ == Screen::Article) {
    drawArticle(display);
  } else {
    drawList(display);
  }
}

#include "CalendarApp.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "CivilTime.h"

using calendar::AgendaBuilder;
using calendar::IcsParser;
using calendar::kSecondsPerDay;
using calendar::Occurrence;
using paperos::Color;
using paperos::FileMode;
using paperos::HttpResponse;
using paperos::InputEvent;
using paperos::NetworkState;
using paperos::TextAlign;
using paperos::TextSize;

namespace {

constexpr int kMargin = 24;
constexpr int kRowGap = 14;
constexpr int kMaxDays = 62;
constexpr size_t kMaxFeeds = 16;
// Work done per tick, small enough that the buttons stay responsive.
constexpr size_t kLoadBytesPerTick = 16 * 1024;
constexpr int kDownloadReadsPerTick = 8;
// How often an idle agenda checks for a new day or a due download.
constexpr uint32_t kIdleCheckMs = 30 * 1000;
constexpr uint32_t kConnectTimeoutMs = 30 * 1000;
constexpr uint32_t kTimeSyncTimeoutMs = 20 * 1000;
constexpr const char* kFeedTable = "calendar.feed";
constexpr const char* kSyncTimeFile = "/.cache/calendar/synced";
// Cached agenda file: magic, format version, then the fields below.
constexpr char kAgendaMagic[4] = {'P', 'C', 'A', 'L'};
constexpr uint8_t kAgendaVersion = 1;
// Bigger than any agenda of kMaxOccurrences could be; anything larger is junk.
constexpr uint32_t kMaxAgendaCacheBytes = 4 * 1024 * 1024;

char chunk[2048];

const char* const kWeekdays[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
const char* const kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                               "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

// "09:05"
std::string clockText(int64_t civil) {
  const calendar::DateTime t = calendar::fromCivilSeconds(civil);
  char text[8];
  std::snprintf(text, sizeof(text), "%02d:%02d", t.hour, t.minute);
  return text;
}

// "Tue 10 Nov"
std::string dateText(int64_t civil) {
  const int64_t days = calendar::floorDiv(civil, kSecondsPerDay);
  const calendar::DateTime t = calendar::fromCivilSeconds(civil);
  return std::string(kWeekdays[calendar::weekdayOfDays(days)]) + " " + std::to_string(t.day) +
         " " + kMonths[t.month - 1];
}

int64_t midnightOf(int64_t civil) {
  return calendar::floorDiv(civil, kSecondsPerDay) * kSecondsPerDay;
}

// Shortens `text` with "..." until it fits in `maxWidth`.
std::string fitToWidth(const paperos::Display& display, std::string text, TextSize size,
                       int maxWidth) {
  if (display.textWidth(text.c_str(), size) <= maxWidth) return text;
  while (!text.empty() && display.textWidth((text + "...").c_str(), size) > maxWidth) {
    text.pop_back();
  }
  return text + "...";
}

// True if the downloaded text starts like an iCalendar file rather than,
// say, an HTML login page.
bool looksLikeCalendar(const std::string& head) {
  static const char kBegin[] = "BEGIN:VCALENDAR";
  size_t i = head.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
  while (i < head.size() && (head[i] == ' ' || head[i] == '\r' || head[i] == '\n')) ++i;
  for (size_t k = 0; kBegin[k] != '\0'; ++k, ++i) {
    if (i >= head.size()) return false;
    const char c = head[i] >= 'a' && head[i] <= 'z' ? char(head[i] - 'a' + 'A') : head[i];
    if (c != kBegin[k]) return false;
  }
  return true;
}

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

}  // namespace

CalendarApp::CalendarApp(const paperos::Config& config, paperos::FileSystem& fs,
                         paperos::Network& network, paperos::Clock& clock,
                         const paperos::Display& display, const paperos::Battery& battery)
    : config_(config),
      fs_(fs),
      network_(network),
      clock_(clock),
      display_(display),
      battery_(battery) {}

std::string CalendarApp::cachePath(const std::string& url) {
  const uint32_t hash = fnv1a(url);
  char path[32];
  std::snprintf(path, sizeof(path), "%s/%08x.ics", kCacheDir, unsigned(hash));
  return path;
}

void CalendarApp::readFeeds() {
  feeds_.clear();
  const size_t count = config_.tableCount(kFeedTable);
  for (size_t i = 0; i < count && feeds_.size() < kMaxFeeds; ++i) {
    Feed feed;
    feed.url = config_.getTableString(kFeedTable, i, "url");
    if (feed.url.empty()) continue;
    feed.name = paperos::foldUtf8ToAscii(config_.getTableString(kFeedTable, i, "name"));
    if (feed.name.empty()) feed.name = "Calendar " + std::to_string(feeds_.size() + 1);
    feed.path = cachePath(feed.url);
    feeds_.push_back(feed);
  }
  days_ = int(std::min<long>(kMaxDays, std::max<long>(1, config_.getInt("calendar", "days",
                                                                        kDefaultDays))));
  refreshMinutes_ = int(std::max<long>(
      0, config_.getInt("calendar", "refresh_minutes", kDefaultRefreshMinutes)));
  debug_ = paperos::debugEnabled(config_);
}

void CalendarApp::onOpen() {
  readFeeds();
  page_ = 0;
  errors_.clear();
  wifiDetail_.clear();
  occurrences_.clear();
  pages_.clear();
  loaded_ = false;
  syncAttempted_ = false;
  fullRefreshPending_ = false;
  phase_ = Phase::Idle;
  lastCheckMs_ = clock_.uptimeMs();

  // Both fail harmlessly if they exist.
  fs_.makeDir(kCacheRoot);
  fs_.makeDir(kCacheDir);
  loadSyncTime();
  // Stale-while-revalidate: show the cached agenda at once, then download in
  // the background if it is due. Without a usable cached agenda, build one
  // from the downloaded calendars first.
  if (loadCachedAgenda()) {
    if (shouldSync()) startSync();
  } else if (!startLoading() && shouldSync()) {
    startSync();
  }
}

void CalendarApp::onClose() { stopWork(); }

void CalendarApp::stopWork() {
  if (downloadFile_) {
    downloadFile_.reset();
    fs_.remove((feeds_[downloadIndex_].path + ".part").c_str());
  }
  response_.reset();
  if (phase_ == Phase::Connecting || phase_ == Phase::SyncingTime ||
      phase_ == Phase::Downloading) {
    network_.disconnect();
  }
  loadFile_.reset();
  parser_.reset();
  builder_.reset();
  phase_ = Phase::Idle;
}

// --- Loading the cached calendars -------------------------------------------

bool CalendarApp::startLoading() {
  const int64_t now = clock_.now();
  if (feeds_.empty() || now < 0) return false;
  loadingWindowStart_ = midnightOf(calendar::utcToLocal(now));
  builder_.reset(
      new AgendaBuilder(loadingWindowStart_, loadingWindowStart_ + days_ * kSecondsPerDay));
  loadIndex_ = 0;
  loadFile_.reset();
  phase_ = Phase::Loading;
  return true;
}

bool CalendarApp::loadStep() {
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
      builder_->beginCalendar(uint8_t(loadIndex_));
      parser_.reset(new IcsParser(*builder_));
    }
    const size_t n = loadFile_->read(chunk, sizeof(chunk));
    if (n == 0) {
      parser_->finish();
      builder_->endCalendar();
      parser_.reset();
      loadFile_.reset();
      ++loadIndex_;
      continue;
    }
    parser_->feed(chunk, n);
    budget -= std::min(n, budget);
  }
  return false;
}

void CalendarApp::finishLoading() {
  occurrences_ = builder_->take();
  builder_.reset();
  windowStart_ = loadingWindowStart_;
  loaded_ = true;
  phase_ = Phase::Idle;
  buildPages();
  if (page_ >= pages_.size()) page_ = pages_.empty() ? 0 : pages_.size() - 1;
  fullRefreshPending_ = true;
  saveCachedAgenda();
}

// --- The cached agenda -----------------------------------------------------

// Changes whenever something other than the calendar files changes what the
// agenda holds, so a cached agenda from another setup is not shown.
uint32_t CalendarApp::agendaCacheKey() const {
  std::string key = std::to_string(days_) + "\n" + config_.getString("time", "timezone", "UTC");
  for (const Feed& feed : feeds_) key += "\n" + feed.url;
  return fnv1a(key);
}

bool CalendarApp::loadCachedAgenda() {
  if (feeds_.empty()) return false;
  std::unique_ptr<paperos::File> file = fs_.open(kAgendaCachePath, FileMode::Read);
  if (!file || file->size() > kMaxAgendaCacheBytes) return false;
  std::string data(file->size(), '\0');
  if (file->read(&data[0], data.size()) != data.size()) return false;
  file.reset();

  CacheReader in(data);
  for (char c : kAgendaMagic) {
    if (in.getInt(1) != uint8_t(c)) return false;
  }
  if (in.getInt(1) != kAgendaVersion || in.getInt(4) != agendaCacheKey()) return false;
  // Only today's agenda: on a new day, build it again from the calendars.
  // While the time is unknown, show it anyway; tick() builds it again if it
  // turns out to be from another day once the time is set.
  const int64_t windowStart = int64_t(in.getInt(8));
  if (today() != kUnknownDay && windowStart != today()) return false;
  const uint64_t count = in.getInt(4);
  if (!in.ok() || count > AgendaBuilder::kMaxOccurrences) return false;

  std::vector<Occurrence> occurrences(static_cast<size_t>(count));
  for (Occurrence& o : occurrences) {
    o.start = int64_t(in.getInt(8));
    o.end = int64_t(in.getInt(8));
    o.allDay = in.getInt(1) != 0;
    o.calendar = uint8_t(in.getInt(1));
    o.summary = in.getString();
    o.location = in.getString();
    if (o.calendar >= feeds_.size()) return false;
  }
  if (!in.ok() || !in.atEnd()) return false;

  occurrences_ = std::move(occurrences);
  windowStart_ = windowStart;
  loaded_ = true;
  buildPages();
  if (page_ >= pages_.size()) page_ = pages_.empty() ? 0 : pages_.size() - 1;
  return true;
}

void CalendarApp::saveCachedAgenda() const {
  std::string data(kAgendaMagic, sizeof(kAgendaMagic));
  putInt(data, kAgendaVersion, 1);
  putInt(data, agendaCacheKey(), 4);
  putInt(data, uint64_t(windowStart_), 8);
  putInt(data, occurrences_.size(), 4);
  for (const Occurrence& o : occurrences_) {
    putInt(data, uint64_t(o.start), 8);
    putInt(data, uint64_t(o.end), 8);
    putInt(data, o.allDay ? 1 : 0, 1);
    putInt(data, o.calendar, 1);
    putString(data, o.summary);
    putString(data, o.location);
  }

  // Like a download, only replace the old copy once the new one is complete.
  const std::string part = std::string(kAgendaCachePath) + ".part";
  std::unique_ptr<paperos::File> file = fs_.open(part.c_str(), FileMode::Write);
  if (!file) return;
  const bool written = file->write(data.data(), data.size()) == data.size();
  file.reset();
  if (written) {
    fs_.remove(kAgendaCachePath);
    if (fs_.rename(part.c_str(), kAgendaCachePath)) return;
  }
  fs_.remove(part.c_str());
  fs_.remove(kAgendaCachePath);  // never leave a cached agenda that may be out of date
}

// --- Downloading -----------------------------------------------------------

bool CalendarApp::shouldSync() const {
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

void CalendarApp::startSync() {
  errors_.clear();
  setWifiDetail("");
  downloaded_ = 0;
  syncAttempted_ = true;
  syncAttemptMs_ = clock_.uptimeMs();
  phaseStartedMs_ = syncAttemptMs_;
  phase_ = Phase::Connecting;
  network_.connect();
}

bool CalendarApp::syncStep() {
  const uint32_t elapsed = clock_.uptimeMs() - phaseStartedMs_;
  switch (phase_) {
    case Phase::Connecting: {
      const NetworkState state = network_.state();
      if (state == NetworkState::Connected) {
        // Redraw so the header shows Wi-Fi as connected.
        clock_.sync();
        if (clock_.now() < 0) {
          phase_ = Phase::SyncingTime;
          phaseStartedMs_ = clock_.uptimeMs();
          return true;
        }
        phase_ = Phase::Downloading;
        downloadIndex_ = 0;
        openNextDownload();
        return true;
      }
      if (state == NetworkState::Failed || elapsed > kConnectTimeoutMs) {
        errors_.push_back("Could not connect to Wi-Fi");
        if (debug_) {
          std::string detail = network_.failureDetail();
          if (state != NetworkState::Failed) {
            detail = "gave up after " + std::to_string(elapsed / 1000) + "s" +
                     (detail.empty() ? "" : "; " + detail);
          }
          setWifiDetail(detail.empty() ? "no detail from the Wi-Fi driver" : detail);
        }
        finishSync();
        return true;
      }
      return false;
    }
    case Phase::SyncingTime:
      if (clock_.now() >= 0) {
        phase_ = Phase::Downloading;
        downloadIndex_ = 0;
        openNextDownload();
        return phase_ != Phase::Downloading;
      }
      if (elapsed > kTimeSyncTimeoutMs) {
        errors_.push_back("Could not get the time from the internet");
        finishSync();
        return true;
      }
      return false;
    case Phase::Downloading:
      return downloadStep();
    default:
      return false;
  }
}

void CalendarApp::openNextDownload() {
  while (downloadIndex_ < feeds_.size()) {
    const Feed& feed = feeds_[downloadIndex_];
    std::string error;
    response_ = network_.get(feed.url.c_str(), error);
    if (!response_) {
      errors_.push_back(feed.name + ": " + error);
      ++downloadIndex_;
      continue;
    }
    downloadFile_ = fs_.open((feed.path + ".part").c_str(), FileMode::Write);
    if (!downloadFile_) {
      response_.reset();
      errors_.push_back(feed.name + ": could not write to the SD card");
      ++downloadIndex_;
      continue;
    }
    downloadHead_.clear();
    return;
  }
  finishSync();
}

bool CalendarApp::downloadStep() {
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

void CalendarApp::finishDownload(bool ok, std::string error) {
  const Feed& feed = feeds_[downloadIndex_];
  const std::string part = feed.path + ".part";
  response_.reset();
  downloadFile_.reset();
  if (ok && !looksLikeCalendar(downloadHead_)) {
    ok = false;
    error = "not an iCal file, check the url";
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
    errors_.push_back(feed.name + ": " + error);
  }
  ++downloadIndex_;
  openNextDownload();
}

void CalendarApp::finishSync() {
  network_.disconnect();
  phase_ = Phase::Idle;
  if (downloaded_ > 0) {
    lastSync_ = clock_.now();
    saveSyncTime();
    removeStaleCaches();
  }
  if (downloaded_ > 0 || !loaded_) startLoading();
}

void CalendarApp::removeStaleCaches() {
  std::vector<paperos::DirEntry> entries;
  if (!fs_.list(kCacheDir, entries)) return;
  for (const paperos::DirEntry& entry : entries) {
    const std::string path = std::string(kCacheDir) + "/" + entry.name;
    const bool isCache = entry.name.size() > 4 &&
                         entry.name.compare(entry.name.size() - 4, 4, ".ics") == 0;
    if (entry.isDirectory || !isCache) continue;
    const bool inUse = std::any_of(feeds_.begin(), feeds_.end(),
                                   [&](const Feed& feed) { return feed.path == path; });
    if (!inUse) fs_.remove(path.c_str());
  }
}

void CalendarApp::loadSyncTime() {
  lastSync_ = 0;
  std::unique_ptr<paperos::File> file = fs_.open(kSyncTimeFile, FileMode::Read);
  if (!file) return;
  char text[24] = {};
  file->read(text, sizeof(text) - 1);
  lastSync_ = std::strtoll(text, nullptr, 10);
}

void CalendarApp::saveSyncTime() {
  std::unique_ptr<paperos::File> file = fs_.open(kSyncTimeFile, FileMode::Write);
  if (!file) return;
  const std::string text = std::to_string(lastSync_);
  file->write(text.data(), text.size());
}

// --- Main loop and input ---------------------------------------------------

bool CalendarApp::tick() {
  switch (phase_) {
    case Phase::Loading:
      if (!loadStep()) return false;
      if (shouldSync()) startSync();
      return true;
    case Phase::Connecting:
    case Phase::SyncingTime:
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
  // Move on at midnight, or load once the time becomes known.
  const int64_t day = today();
  if (day == kUnknownDay) return false;
  if (!loaded_ || day != windowStart_) {
    startLoading();
  } else if (pagesToday_ != day) {  // add "Today" now that it is known
    buildPages();
    return true;
  }
  return false;
}

bool CalendarApp::onInput(InputEvent event) {
  switch (event) {
    case InputEvent::Previous:
      if (page_ == 0) return false;
      --page_;
      return true;
    case InputEvent::Next:
      if (page_ + 1 >= pages_.size()) return false;
      ++page_;
      return true;
    case InputEvent::Select:
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

// --- Layout ----------------------------------------------------------------

int64_t CalendarApp::today() const {
  const int64_t now = clock_.now();
  return now < 0 ? kUnknownDay : midnightOf(calendar::utcToLocal(now));
}

std::vector<CalendarApp::Row> CalendarApp::buildRows() const {
  std::vector<Row> rows;
  for (int d = 0; d < days_; ++d) {
    const int64_t dayStart = windowStart_ + d * kSecondsPerDay;
    const int64_t dayEnd = dayStart + kSecondsPerDay;

    std::vector<Row> events;
    for (const Occurrence& o : occurrences_) {
      if (o.start >= dayEnd || (o.end <= dayStart && o.start < dayStart)) continue;
      Row row;
      if (o.allDay || (o.start <= dayStart && o.end >= dayEnd)) {
        row.time = "All day";
      } else if (o.start < dayStart) {
        row.time = "Until " + clockText(o.end);
      } else if (o.end > dayEnd) {
        row.time = "From " + clockText(o.start);
      } else if (o.end == o.start) {
        row.time = clockText(o.start);
      } else {
        row.time = clockText(o.start) + " - " + clockText(o.end);
      }
      row.text = o.summary.empty() ? "(No title)" : o.summary;
      row.detail = o.location;
      if (feeds_.size() > 1 && o.calendar < feeds_.size()) {
        if (!row.detail.empty()) row.detail += " - ";
        row.detail += feeds_[o.calendar].name;
      }
      events.push_back(row);
    }
    if (events.empty() && d > 0) continue;  // only today shows when empty

    Row day;
    day.kind = Row::Kind::Day;
    day.text = dateText(dayStart);
    if (pagesToday_ != kUnknownDay && dayStart == pagesToday_) day.text = "Today, " + day.text;
    if (pagesToday_ != kUnknownDay && dayStart == pagesToday_ + kSecondsPerDay) {
      day.text = "Tomorrow, " + day.text;
    }
    rows.push_back(day);
    if (events.empty()) {
      Row empty;
      empty.kind = Row::Kind::Empty;
      empty.text = "No events";
      rows.push_back(empty);
    }
    rows.insert(rows.end(), events.begin(), events.end());
  }
  return rows;
}

int CalendarApp::rowHeight(const Row& row) const {
  switch (row.kind) {
    case Row::Kind::Day:
      return display_.textHeight(TextSize::Large) + kRowGap;
    case Row::Kind::Empty:
      return display_.textHeight(TextSize::Medium) + kRowGap;
    case Row::Kind::Event:
      break;
  }
  int h = display_.textHeight(TextSize::Medium) + kRowGap;
  if (!row.detail.empty()) h += display_.textHeight(TextSize::Small) + 4;
  return h;
}

int CalendarApp::bodyTop() const {
  return kMargin * 2 + display_.textHeight(TextSize::Title) + kMargin / 2;
}

int CalendarApp::bodyBottom() const {
  // Footer: debug lines, status line, separator, button legend.
  const int smallH = display_.textHeight(TextSize::Small);
  const size_t debugCount = debugLines().size();
  const int debugH = debugCount == 0 ? 0 : int(debugCount) * (smallH + 4) + 8;
  return display_.height() - kMargin - 2 * smallH - 12 - 8 - kRowGap - debugH;
}

std::vector<std::string> CalendarApp::debugLines() const {
  constexpr size_t kMaxLines = 4;
  std::vector<std::string> lines;
  if (wifiDetail_.empty()) return lines;
  const int maxW = display_.width() - 2 * kMargin;
  const std::string text = "Wi-Fi: " + paperos::foldUtf8ToAscii(wifiDetail_);
  // Word wrap; the last line is shortened with "..." if there is more.
  std::string line;
  size_t i = 0;
  while (i < text.size()) {
    size_t end = text.find(' ', i);
    if (end == std::string::npos) end = text.size();
    const std::string word = text.substr(i, end - i);
    const std::string candidate = line.empty() ? word : line + " " + word;
    if (line.empty() || display_.textWidth(candidate.c_str(), TextSize::Small) <= maxW) {
      line = candidate;
    } else {
      if (lines.size() + 1 == kMaxLines) {
        lines.push_back(fitToWidth(display_, line + " " + text.substr(i), TextSize::Small, maxW));
        return lines;
      }
      lines.push_back(fitToWidth(display_, line, TextSize::Small, maxW));
      line = word;
    }
    i = end + 1;
  }
  if (!line.empty()) lines.push_back(fitToWidth(display_, line, TextSize::Small, maxW));
  return lines;
}

void CalendarApp::setWifiDetail(std::string detail) {
  if (detail == wifiDetail_) return;
  wifiDetail_ = std::move(detail);
  // The footer changed height, so the agenda pages do too.
  if (loaded_) {
    buildPages();
    if (page_ >= pages_.size()) page_ = pages_.empty() ? 0 : pages_.size() - 1;
  }
}

void CalendarApp::buildPages() {
  pages_.clear();
  pagesToday_ = today();
  const std::vector<Row> rows = buildRows();
  const int top = bodyTop();
  const int bottom = bodyBottom();

  std::vector<Row> page;
  const Row* day = nullptr;
  int y = top;
  for (size_t i = 0; i < rows.size(); ++i) {
    const Row& row = rows[i];
    const int h = rowHeight(row);
    if (row.kind == Row::Kind::Day) {
      day = &row;
      // Keep a day's heading together with its first event.
      const int need = h + (i + 1 < rows.size() ? rowHeight(rows[i + 1]) : 0);
      if (!page.empty() && y + need > bottom) {
        pages_.push_back(page);
        page.clear();
        y = top;
      }
    } else if (!page.empty() && y + h > bottom) {
      pages_.push_back(page);
      page.clear();
      y = top;
      if (day != nullptr) {  // repeat the heading on the new page
        page.push_back(*day);
        y += rowHeight(*day);
      }
    }
    page.push_back(row);
    y += h;
  }
  if (!page.empty()) pages_.push_back(page);
}

std::string CalendarApp::statusText() const {
  switch (phase_) {
    case Phase::Connecting:
    case Phase::SyncingTime:
    case Phase::Downloading:
      return "Updating...";
    case Phase::Loading:
    case Phase::Idle:
      break;
  }
  if (!errors_.empty()) {
    std::string text = errors_.front();
    if (errors_.size() > 1) text += " (+" + std::to_string(errors_.size() - 1) + " more)";
    return text;
  }
  if (lastSync_ > 0) {
    const int64_t when = calendar::utcToLocal(lastSync_);
    const int64_t now = clock_.now();
    const bool today = now >= 0 && midnightOf(calendar::utcToLocal(now)) == midnightOf(when);
    return "Updated " + (today ? clockText(when) : dateText(when) + ", " + clockText(when));
  }
  if (feeds_.empty()) return "";
  return network_.configured() ? "Not downloaded yet" : "Set [wifi] in config.toml to download";
}

// --- Drawing ---------------------------------------------------------------

paperos::NetworkState CalendarApp::wifiState() const {
  // Wi-Fi is only on while updating, and the phase follows it.
  switch (phase_) {
    case Phase::Connecting:
      return NetworkState::Connecting;
    case Phase::SyncingTime:
    case Phase::Downloading:
      return NetworkState::Connected;
    case Phase::Loading:
    case Phase::Idle:
      break;
  }
  return NetworkState::Off;
}

void CalendarApp::draw(paperos::Display& display) {
  fullRefreshPending_ = false;
  const int w = display.width();
  const int h = display.height();

  // Header.
  const int titleH = display.textHeight(TextSize::Title);
  display.drawText("Calendar", kMargin, kMargin, TextSize::Title);
  const int smallH = display.textHeight(TextSize::Small);
  const int batteryY = kMargin + (titleH - smallH) / 2;
  const int batteryW =
      paperos::drawBatteryLevel(display, battery_, w - kMargin, batteryY, TextAlign::Right);
  const int wifiH = smallH * 2 / 3;
  paperos::drawWifiStatus(display, wifiState(), w - kMargin - batteryW - (batteryW > 0 ? 12 : 0),
                          batteryY + (smallH - wifiH) / 2, wifiH);
  const int headerBottom = kMargin * 2 + titleH;
  display.fillRect(0, headerBottom - 4, w, 4, Color::Black);

  // Body.
  if (feeds_.empty()) {
    drawMessage(display, "No calendars set up", "Add a [[calendar.feed]] url to config.toml");
  } else if (!pages_.empty()) {
    int y = bodyTop();
    for (const Row& row : pages_[page_]) {
      drawRow(display, row, y);
      y += rowHeight(row);
    }
  } else if (clock_.now() < 0) {
    drawMessage(display, "Date and time unknown",
                network_.configured() ? "Getting them from the internet"
                                      : "Set [wifi] in config.toml to get them");
  } else {
    drawMessage(display, "Loading...", "");
  }

  // Footer: status and page on one line, then the button legend.
  const int legendY = h - kMargin - smallH;
  const int lineY = legendY - 12;
  const int statusY = lineY - 8 - smallH;
  std::string pageText;
  if (pages_.size() > 1) {
    pageText = std::to_string(page_ + 1) + " / " + std::to_string(pages_.size());
    display.drawText(pageText.c_str(), w - kMargin, statusY, TextSize::Small, TextAlign::Right,
                     Color::DarkGray);
  }
  const int pageW = pageText.empty() ? 0 : display.textWidth(pageText.c_str(), TextSize::Small);
  const std::string status =
      fitToWidth(display, statusText(), TextSize::Small, w - 2 * kMargin - pageW - 16);
  display.drawText(status.c_str(), kMargin, statusY, TextSize::Small, TextAlign::Left,
                   Color::DarkGray);
  const std::vector<std::string> debug = debugLines();
  int debugY = statusY - int(debug.size()) * (smallH + 4);
  for (const std::string& line : debug) {
    display.drawText(line.c_str(), kMargin, debugY, TextSize::Small, TextAlign::Left,
                     Color::DarkGray);
    debugY += smallH + 4;
  }
  display.drawLine(0, lineY, w, lineY, Color::DarkGray);
  display.drawText("A / B: page    Hold A: update    Hold B: back", w / 2, legendY,
                   TextSize::Small, TextAlign::Center, Color::DarkGray);
}

void CalendarApp::drawRow(paperos::Display& display, const Row& row, int y) const {
  const int w = display.width();
  switch (row.kind) {
    case Row::Kind::Day: {
      const std::string text = fitToWidth(display, row.text, TextSize::Large, w - 2 * kMargin);
      display.drawText(text.c_str(), kMargin, y, TextSize::Large);
      const int lineY = y + display.textHeight(TextSize::Large) + 2;
      display.drawLine(kMargin, lineY, w - kMargin, lineY, Color::LightGray);
      return;
    }
    case Row::Kind::Empty:
      display.drawText(row.text.c_str(), kMargin, y, TextSize::Medium, TextAlign::Left,
                       Color::DarkGray);
      return;
    case Row::Kind::Event:
      break;
  }
  // Times sit in a column wide enough for "00:00 - 00:00".
  const int timeW = display.textWidth("00:00 - 00:00", TextSize::Small) + 16;
  const int textX = kMargin + timeW;
  const int textW = w - kMargin - textX;
  const int timeY = y + (display.textHeight(TextSize::Medium) - display.textHeight(TextSize::Small));
  display.drawText(row.time.c_str(), kMargin, timeY, TextSize::Small, TextAlign::Left,
                   Color::DarkGray);
  const std::string title = fitToWidth(display, row.text, TextSize::Medium, textW);
  display.drawText(title.c_str(), textX, y, TextSize::Medium);
  if (!row.detail.empty()) {
    const std::string detail = fitToWidth(display, row.detail, TextSize::Small, textW);
    display.drawText(detail.c_str(), textX, y + display.textHeight(TextSize::Medium) + 4,
                     TextSize::Small, TextAlign::Left, Color::DarkGray);
  }
}

void CalendarApp::drawMessage(paperos::Display& display, const char* title,
                              const char* detail) const {
  const int w = display.width();
  const int y = (bodyTop() + bodyBottom()) / 2 - display.textHeight(TextSize::Large);
  display.drawText(title, w / 2, y, TextSize::Large, TextAlign::Center);
  if (*detail != '\0') {
    display.drawText(detail, w / 2, y + display.textHeight(TextSize::Large) + 16,
                     TextSize::Small, TextAlign::Center, Color::DarkGray);
  }
}

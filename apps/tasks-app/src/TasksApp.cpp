#include "TasksApp.h"

#include <algorithm>
#include <cstdio>
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
constexpr size_t kLoadBytesPerTick = 32 * 1024;
constexpr int kDownloadReadsPerTick = 16;
char chunk[4096];

const char* const kMenuItems[] = {"Mark as done", "View details"};
constexpr size_t kMenuItemCount = 2;

std::string fitToWidth(const paperos::Display& display, std::string text, TextSize size,
                       int maxWidth) {
  if (display.textWidth(text.c_str(), size) <= maxWidth) return text;
  while (!text.empty() && display.textWidth((text + "...").c_str(), size) > maxWidth) {
    text.pop_back();
  }
  return text + "...";
}

std::string join(const std::vector<std::string>& parts, const char* separator) {
  std::string out;
  for (const std::string& part : parts) {
    if (part.empty()) continue;
    if (!out.empty()) out += separator;
    out += part;
  }
  return out;
}

std::string capitalized(std::string text) {
  if (!text.empty() && text[0] >= 'a' && text[0] <= 'z') text[0] = char(text[0] - 'a' + 'A');
  return text;
}

std::string statusName(const std::string& status) {
  if (status == "next") return "Next action";
  return capitalized(status);
}

int64_t localMidnight(int64_t utc) {
  const time_t seconds = time_t(utc);
  struct tm local;
  localtime_r(&seconds, &local);
  local.tm_hour = 0;
  local.tm_min = 0;
  local.tm_sec = 0;
  local.tm_isdst = -1;
  return int64_t(mktime(&local));
}

int64_t nextLocalMidnight(int64_t utc) {
  const time_t seconds = time_t(utc);
  struct tm local;
  localtime_r(&seconds, &local);
  local.tm_mday += 1;
  local.tm_hour = 0;
  local.tm_min = 0;
  local.tm_sec = 0;
  local.tm_isdst = -1;
  return int64_t(mktime(&local));
}

}  // namespace

TasksApp::TasksApp(const paperos::Config& config, paperos::FileSystem& fs,
                   paperos::Network& network, paperos::Clock& clock,
                   const paperos::Display& display, const paperos::Battery& battery)
    : config_(config),
      fs_(fs),
      network_(network),
      clock_(clock),
      display_(display),
      battery_(battery) {}

void TasksApp::readSettings() {
  dav_ = paperos::DavSettings::fromConfig(config_);
  fileName_ = config_.getString("tasks", "file", "data.json");
  if (fileName_.empty()) fileName_ = "data.json";
  days_ = int(std::min<long>(62, std::max<long>(0, config_.getInt("tasks", "days", kDefaultDays))));
  refreshMinutes_ = int(
      std::max<long>(0, config_.getInt("tasks", "refresh_minutes", kDefaultRefreshMinutes)));
}

// --- Opening and closing -----------------------------------------------------

void TasksApp::onOpen() {
  readSettings();
  data_ = mindwtr::Data();
  loaded_ = false;
  rows_.clear();
  pages_.clear();
  selectedRow_ = -1;
  listPage_ = 0;
  screen_ = Screen::List;
  menuTaskId_.clear();
  error_.clear();
  notice_.clear();
  lastSync_ = -1;
  attempted_ = false;
  fullRefreshPending_ = false;
  phase_ = Phase::Idle;
  fs_.makeDir(kCacheRoot);
  fs_.makeDir(kCacheDir);
  rebuildRows();

  // Show the saved copy at once, then download a new one.
  loadFile_ = fs_.open(kCachePath, FileMode::Read);
  if (loadFile_ && loadFile_->size() > 0 && loadFile_->size() <= kMaxFileBytes) {
    body_.clear();
    body_.reserve(loadFile_->size());
    phase_ = Phase::Loading;
  } else {
    loadFile_.reset();
    startNetwork(Operation::Refresh);
  }
}

void TasksApp::onClose() {
  stopWork();
  data_ = mindwtr::Data();
  rows_.clear();
  pages_.clear();
  detailLines_.clear();
}

void TasksApp::stopWork() {
  loadFile_.reset();
  response_.reset();
  if (phase_ == Phase::Connecting || phase_ == Phase::SyncingTime ||
      phase_ == Phase::Downloading || phase_ == Phase::Saving) {
    network_.disconnect();
  }
  phase_ = Phase::Idle;
  doneId_.clear();
  clearBody();
}

void TasksApp::clearBody() {
  std::string().swap(body_);
  etag_.clear();
}

// --- The saved copy -----------------------------------------------------------

bool TasksApp::loadStep() {
  const size_t n = loadFile_->read(chunk, std::min(sizeof(chunk), kLoadBytesPerTick));
  if (n > 0) {
    body_.append(chunk, n);
    if (body_.size() < loadFile_->size()) return false;
  }
  loadFile_.reset();
  std::string error;
  if (!applyText(body_, error)) error_ = "Saved copy: " + error;
  clearBody();
  phase_ = Phase::Idle;
  startNetwork(Operation::Refresh);
  return true;
}

void TasksApp::saveCache(const std::string& text) {
  const std::string part = std::string(kCachePath) + ".part";
  std::unique_ptr<paperos::File> file = fs_.open(part.c_str(), FileMode::Write);
  bool ok = file != nullptr;
  for (size_t pos = 0; ok && pos < text.size(); pos += sizeof(chunk)) {
    const size_t count = std::min(sizeof(chunk), text.size() - pos);
    ok = file->write(text.data() + pos, count) == count;
  }
  file.reset();
  if (ok) {
    fs_.remove(kCachePath);
    ok = fs_.rename(part.c_str(), kCachePath);
  }
  if (!ok) fs_.remove(part.c_str());
}

bool TasksApp::applyText(const std::string& text, std::string& error) {
  mindwtr::Data data;
  if (!mindwtr::parse(text, data, error)) return false;
  data_ = std::move(data);
  loaded_ = true;
  rebuildRows();
  if (screen_ != Screen::List && data_.find(menuTaskId_) == nullptr) screen_ = Screen::List;
  if (screen_ == Screen::Details) buildDetails();
  return true;
}

// --- Downloading and saving ---------------------------------------------------

void TasksApp::startNetwork(Operation op) {
  if (!dav_.configured() || !network_.configured()) return;
  attempted_ = true;
  lastAttemptMs_ = clock_.uptimeMs();
  operation_ = op;
  saveAttempts_ = 0;
  error_.clear();
  phase_ = Phase::Connecting;
  phaseStartedMs_ = clock_.uptimeMs();
  network_.connect();
}

void TasksApp::startDownload() {
  paperos::DavClient client(network_, dav_);
  std::string error;
  clearBody();
  response_ = client.getFile(fileName_, error);
  if (!response_) {
    error_ = error;
    finishNetwork();
    return;
  }
  etag_ = response_->header("ETag");
  const int64_t length = response_->contentLength();
  if (length > int64_t(kMaxFileBytes)) {
    error_ = fileName_ + " is too large";
    finishNetwork();
    return;
  }
  if (length > 0) body_.reserve(size_t(length));
  phase_ = Phase::Downloading;
  phaseStartedMs_ = clock_.uptimeMs();
}

bool TasksApp::networkStep() {
  const uint32_t elapsed = clock_.uptimeMs() - phaseStartedMs_;
  switch (phase_) {
    case Phase::Connecting: {
      const NetworkState state = network_.state();
      if (state == NetworkState::Connected) {
        if (clock_.now() < 0) {
          clock_.sync();
          phase_ = Phase::SyncingTime;
          phaseStartedMs_ = clock_.uptimeMs();
          return true;
        }
        startDownload();
        return true;
      }
      if (state == NetworkState::Failed || elapsed > kConnectTimeoutMs) {
        error_ = "Could not connect to Wi-Fi";
        finishNetwork();
        return true;
      }
      return false;
    }
    case Phase::SyncingTime:
      if (clock_.now() >= 0) {
        rebuildRows();  // the dates can be shown now
        startDownload();
        return true;
      }
      if (elapsed > kTimeSyncTimeoutMs) {
        if (operation_ == Operation::MarkDone) {
          error_ = "Could not get the time, so not saved";
          finishNetwork();
          return true;
        }
        startDownload();  // the lists still work without dates
        return true;
      }
      return false;
    case Phase::Downloading:
      return downloadStep();
    case Phase::Saving:
      saveDone();
      return true;
    default:
      return false;
  }
}

bool TasksApp::downloadStep() {
  for (int i = 0; i < kDownloadReadsPerTick; ++i) {
    const int n = response_->read(chunk, sizeof(chunk));
    if (n == 0) return false;
    if (n == HttpResponse::kEnd) {
      response_.reset();
      finishDownload();
      return true;
    }
    if (n < 0) {
      error_ = "Download interrupted";
      finishNetwork();
      return true;
    }
    body_.append(chunk, size_t(n));
    if (body_.size() > kMaxFileBytes) {
      error_ = fileName_ + " is too large";
      finishNetwork();
      return true;
    }
  }
  return false;
}

void TasksApp::finishDownload() {
  if (operation_ == Operation::MarkDone) {
    // Redraw with "Saving..." first: the upload waits for the server.
    phase_ = Phase::Saving;
    return;
  }
  std::string error;
  if (applyText(body_, error)) {
    saveCache(body_);
    lastSync_ = clock_.now();
  } else {
    error_ = error;
  }
  finishNetwork();
}

void TasksApp::saveDone() {
  const int64_t now = clock_.now();
  const mindwtr::Task* before = data_.find(doneId_);
  const std::string title = before != nullptr ? paperos::foldUtf8ToAscii(before->title) : "Task";
  std::string text = body_;
  const mindwtr::DoneResult result = mindwtr::markDone(text, doneId_, now);
  std::string error;
  switch (result) {
    case mindwtr::DoneResult::Ok:
      break;
    case mindwtr::DoneResult::AlreadyDone:
      notice_ = title + " was already done";
      break;
    case mindwtr::DoneResult::NotFound:
      error_ = title + " is no longer in Mindwtr";
      break;
    case mindwtr::DoneResult::Recurring:
      error_ = "Repeating tasks can only be completed in Mindwtr";
      break;
    case mindwtr::DoneResult::Invalid:
      error_ = "Could not read " + fileName_;
      break;
  }
  if (result != mindwtr::DoneResult::Ok) {
    // Show the latest tasks anyway.
    if (applyText(body_, error)) {
      saveCache(body_);
      lastSync_ = now;
    }
    finishNetwork();
    return;
  }
  if (etag_.empty()) {
    error_ = "Server gave no ETag, so not saved";
    finishNetwork();
    return;
  }

  paperos::DavClient client(network_, dav_);
  const paperos::DavWrite write =
      client.putFile(fileName_, text, etag_, "application/json", error);
  if (write == paperos::DavWrite::Conflict && ++saveAttempts_ < kMaxSaveAttempts) {
    // Changed elsewhere since the download: start again from the new copy.
    startDownload();
    return;
  }
  if (write == paperos::DavWrite::Conflict) {
    error_ = "Not saved: the server refused it " + std::to_string(saveAttempts_) +
             " times (HTTP 412)";
    finishNetwork();
    return;
  }
  if (write != paperos::DavWrite::Ok) {
    error_ = "Not saved: " + error;
    finishNetwork();
    return;
  }
  notice_ = "Done: " + title;
  if (applyText(text, error)) {
    saveCache(text);
    lastSync_ = now;
  }
  finishNetwork();
}

void TasksApp::finishNetwork() {
  response_.reset();
  network_.disconnect();
  phase_ = Phase::Idle;
  const bool wasMarking = !doneId_.empty();
  doneId_.clear();
  clearBody();
  if (wasMarking && loaded_) rebuildRows();  // drop "Marking done..."
}

// --- Input and ticks ----------------------------------------------------------

bool TasksApp::onInput(InputEvent event) {
  switch (screen_) {
    case Screen::List:
      switch (event) {
        case InputEvent::Previous:
        case InputEvent::Next:
          moveSelection(event == InputEvent::Next ? 1 : -1);
          return true;
        case InputEvent::Select:
          if (const mindwtr::Task* task = selectedTask()) {
            menuTaskId_ = task->id;
            menuIndex_ = 0;
            screen_ = Screen::Menu;
            fullRefreshPending_ = true;
            return true;
          }
          if (phase_ == Phase::Idle) {  // nothing to select: download again
            startNetwork(Operation::Refresh);
            return true;
          }
          return false;
        default:
          return false;  // Back: the shell closes the app
      }

    case Screen::Menu:
      switch (event) {
        case InputEvent::Previous:
          menuIndex_ = (menuIndex_ + kMenuItemCount - 1) % kMenuItemCount;
          return true;
        case InputEvent::Next:
          menuIndex_ = (menuIndex_ + 1) % kMenuItemCount;
          return true;
        case InputEvent::Back:
          screen_ = Screen::List;
          fullRefreshPending_ = true;
          return true;
        case InputEvent::Select: {
          const mindwtr::Task* task = data_.find(menuTaskId_);
          if (task == nullptr) {
            screen_ = Screen::List;
            return true;
          }
          if (MenuItem(menuIndex_) == MenuItem::Details) {
            screen_ = Screen::Details;
            detailPage_ = 0;
            buildDetails();
            fullRefreshPending_ = true;
            return true;
          }
          // Mark as done.
          if (task->recurring) {
            notice_ = "Repeating tasks can only be completed in Mindwtr";
            screen_ = Screen::List;
            fullRefreshPending_ = true;
            return true;
          }
          if (!dav_.configured() || !network_.configured()) {
            error_ = !dav_.configured() ? "Set [sync] in config.toml to save"
                                        : "Set [wifi] in config.toml to save";
            screen_ = Screen::List;
            fullRefreshPending_ = true;
            return true;
          }
          if (operation_ == Operation::MarkDone && phase_ != Phase::Idle) {
            notice_ = "Still saving, try again in a moment";
          } else {
            notice_.clear();
            if (phase_ == Phase::Idle || phase_ == Phase::Loading) {
              if (phase_ == Phase::Loading) stopWork();
              startNetwork(Operation::MarkDone);
            } else {
              // A download is running: use it, and save once it's done.
              operation_ = Operation::MarkDone;
              saveAttempts_ = 0;
            }
            doneId_ = task->id;
            rebuildRows();  // shows "Marking done..." on the task
          }
          screen_ = Screen::List;
          fullRefreshPending_ = true;
          return true;
        }
        default:
          return false;
      }

    case Screen::Details:
      switch (event) {
        case InputEvent::Previous:
          if (detailPage_ == 0) return false;
          --detailPage_;
          return true;
        case InputEvent::Next:
          if (detailPage_ + 1 >= detailPages_.size()) return false;
          ++detailPage_;
          return true;
        case InputEvent::Back:
          screen_ = Screen::Menu;
          fullRefreshPending_ = true;
          return true;
        default:
          return false;
      }
  }
  return false;
}

bool TasksApp::tick() {
  bool redraw = false;
  switch (phase_) {
    case Phase::Loading:
      return loadStep();
    case Phase::Idle:
      break;
    default:
      return networkStep();
  }
  // Download again every refresh_minutes.
  if (refreshMinutes_ > 0 && attempted_ &&
      clock_.uptimeMs() - lastAttemptMs_ >= uint32_t(refreshMinutes_) * 60000u) {
    startNetwork(Operation::Refresh);
    redraw = true;
  }
  // Rebuild the lists when the day changes or the time becomes known.
  const int64_t now = clock_.now();
  if (loaded_ && ((now >= 0) != listsTimeKnown_ || (now >= 0 && now >= listsValidUntil_))) {
    rebuildRows();
    redraw = true;
  }
  return redraw;
}

// --- The list -------------------------------------------------------------------

const mindwtr::Task* TasksApp::selectedTask() const {
  if (selectedRow_ < 0 || size_t(selectedRow_) >= rows_.size()) return nullptr;
  return &data_.tasks[rows_[size_t(selectedRow_)].task];
}

std::string TasksApp::dateLabel(int64_t utc, bool hasTime) const {
  const time_t seconds = time_t(utc);
  struct tm local;
  localtime_r(&seconds, &local);
  char text[32];
  const int64_t now = clock_.now();
  const int64_t day = localMidnight(utc);
  std::string label;
  if (now >= 0 && day == localMidnight(now)) {
    label = "Today";
  } else if (now >= 0 && day == nextLocalMidnight(now)) {
    label = "Tomorrow";
  } else {
    strftime(text, sizeof(text), "%a %d %b", &local);
    label = text[4] == '0' ? std::string(text, 4) + (text + 5) : text;  // "Wed 4 Nov"
  }
  if (hasTime) {
    strftime(text, sizeof(text), " %H:%M", &local);
    label += text;
  }
  return label;
}

std::string TasksApp::taskDetail(const mindwtr::Task& task, bool upcoming) const {
  if (task.id == doneId_) return "Marking done...";
  std::vector<std::string> parts;
  const int64_t now = clock_.now();
  if (upcoming && now >= 0) {
    const int64_t when = mindwtr::upcomingTime(task, now);
    const bool isStart = when == task.start;
    parts.push_back((isStart ? "Starts " : "Due ") +
                    dateLabel(when, isStart ? task.startHasTime : task.dueHasTime));
  } else if (task.due != mindwtr::kNoTime) {
    const bool overdue =
        now >= 0 && (task.dueHasTime ? task.due < now : task.due < localMidnight(now));
    parts.push_back((overdue ? "Overdue, due " : "Due ") + dateLabel(task.due, task.dueHasTime));
  }
  parts.push_back(paperos::foldUtf8ToAscii(data_.projectTitle(task.projectId)));
  parts.push_back(paperos::foldUtf8ToAscii(join(task.contexts, " ")));
  return join(parts, " - ");
}

void TasksApp::rebuildRows() {
  std::string selectedId;
  if (const mindwtr::Task* task = selectedTask()) selectedId = task->id;
  const int previousRow = selectedRow_;

  const int64_t now = clock_.now();
  listsTimeKnown_ = now >= 0;
  listsValidUntil_ = now >= 0 ? nextLocalMidnight(now) : 0;
  const mindwtr::Lists lists = mindwtr::buildLists(data_, now, days_);

  rows_.clear();
  auto addSection = [&](const char* heading, const std::vector<size_t>& tasks, bool upcoming,
                        const char* empty) {
    Row head;
    head.kind = Row::Kind::Heading;
    head.text = heading;
    rows_.push_back(head);
    if (tasks.empty()) {
      Row none;
      none.kind = Row::Kind::Empty;
      none.text = empty;
      rows_.push_back(none);
    }
    for (const size_t i : tasks) {
      Row row;
      row.kind = Row::Kind::Task;
      row.task = i;
      row.text = paperos::foldUtf8ToAscii(data_.tasks[i].title);
      if (row.text.empty()) row.text = "(untitled)";
      row.detail = taskDetail(data_.tasks[i], upcoming);
      rows_.push_back(row);
    }
  };
  if (!loaded_) {
    pages_.clear();
    selectedRow_ = -1;
    return;
  }
  addSection("Today's Focus", lists.focus, false, "Nothing in focus today");
  addSection("Next Actions", lists.next, false, "No next actions");
  const std::string upcoming = "Upcoming, " + std::to_string(days_) + " days";
  addSection(upcoming.c_str(), lists.upcoming, true,
             now < 0 ? "Date unknown" : "Nothing scheduled");

  // Keep the same task selected, or the one now in its place.
  selectedRow_ = -1;
  int firstTask = -1;
  for (size_t i = 0; i < rows_.size(); ++i) {
    if (rows_[i].kind != Row::Kind::Task) continue;
    if (firstTask < 0) firstTask = int(i);
    if (!selectedId.empty() && data_.tasks[rows_[i].task].id == selectedId) selectedRow_ = int(i);
  }
  if (selectedRow_ < 0 && previousRow >= 0) {
    for (int i = std::min(previousRow, int(rows_.size()) - 1); i >= 0; --i) {
      if (rows_[size_t(i)].kind == Row::Kind::Task) {
        selectedRow_ = i;
        break;
      }
    }
  }
  if (selectedRow_ < 0) selectedRow_ = firstTask;
  buildPages();
}

int TasksApp::bodyTop() const {
  return kMargin * 2 + display_.textHeight(TextSize::Title) + kMargin / 2;
}

int TasksApp::bodyBottom() const {
  const int smallH = display_.textHeight(TextSize::Small);
  return display_.height() - kMargin - 2 * smallH - 12 - 8 - kRowPad;
}

int TasksApp::rowHeight(const Row& row) const {
  switch (row.kind) {
    case Row::Kind::Heading:
      return display_.textHeight(TextSize::Large) + 2 * kRowPad;
    case Row::Kind::Empty:
      return display_.textHeight(TextSize::Small) + 2 * kRowPad;
    case Row::Kind::Task:
      return display_.textHeight(TextSize::Medium) +
             (row.detail.empty() ? 0 : display_.textHeight(TextSize::Small) + 4) + 2 * kRowPad;
  }
  return 0;
}

void TasksApp::buildPages() {
  pages_.clear();
  std::vector<size_t> page;
  int y = bodyTop();
  for (size_t i = 0; i < rows_.size(); ++i) {
    int h = rowHeight(rows_[i]);
    // Keep a heading with the row after it.
    if (rows_[i].kind == Row::Kind::Heading && i + 1 < rows_.size()) h += rowHeight(rows_[i + 1]);
    if (!page.empty() && y + h > bodyBottom()) {
      pages_.push_back(page);
      page.clear();
      y = bodyTop();
    }
    page.push_back(i);
    y += rowHeight(rows_[i]);
  }
  if (!page.empty()) pages_.push_back(page);
  if (listPage_ >= pages_.size()) listPage_ = pages_.empty() ? 0 : pages_.size() - 1;
}

void TasksApp::moveSelection(int step) {
  std::vector<int> tasks;
  for (size_t i = 0; i < rows_.size(); ++i) {
    if (rows_[i].kind == Row::Kind::Task) tasks.push_back(int(i));
  }
  if (tasks.empty()) {
    // Nothing to select: turn pages instead.
    if (pages_.empty()) return;
    listPage_ = (listPage_ + pages_.size() + size_t(step)) % pages_.size();
    return;
  }
  auto at = std::find(tasks.begin(), tasks.end(), selectedRow_);
  int index = at == tasks.end() ? 0 : int(at - tasks.begin()) + step;
  index = (index + int(tasks.size())) % int(tasks.size());
  selectedRow_ = tasks[size_t(index)];
}

// --- Details --------------------------------------------------------------------

std::vector<std::string> TasksApp::wrap(const std::string& text, TextSize size,
                                        int maxWidth) const {
  std::vector<std::string> lines;
  // Split into lines before folding to ASCII, which drops line breaks.
  size_t start = 0;
  while (start <= text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    std::string paragraph = text.substr(start, end - start);
    if (!paragraph.empty() && paragraph.back() == '\r') paragraph.pop_back();
    paragraph = paperos::foldUtf8ToAscii(paragraph);
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
      // Break a word too long for a line.
      while (display_.textWidth(word.c_str(), size) > maxWidth && word.size() > 1) {
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

void TasksApp::buildDetails() {
  detailLines_.clear();
  detailPages_.clear();
  const mindwtr::Task* task = data_.find(menuTaskId_);
  if (task == nullptr) return;
  const int maxW = display_.width() - 2 * kMargin;
  auto add = [&](const std::string& text, TextSize size, Color color, int gap) {
    bool first = true;
    for (const std::string& line : wrap(text, size, maxW)) {
      detailLines_.push_back({line, size, color, first ? gap : 0});
      first = false;
    }
  };
  auto field = [&](const char* label, const std::string& value) {
    if (!value.empty()) add(std::string(label) + ": " + value, TextSize::Medium, Color::Black, 6);
  };

  add(task->title.empty() ? "(untitled)" : task->title, TextSize::Large, Color::Black, 0);
  field("Status", statusName(task->status));
  if (task->focused) add("In today's focus", TextSize::Medium, Color::Black, 6);
  field("Project", data_.projectTitle(task->projectId));
  if (task->start != mindwtr::kNoTime) field("Starts", dateLabel(task->start, task->startHasTime));
  if (task->due != mindwtr::kNoTime) field("Due", dateLabel(task->due, task->dueHasTime));
  if (task->recurring) field("Repeats", task->recurrence);
  field("Priority", capitalized(task->priority));
  field("Time", task->timeEstimate);
  field("Contexts", join(task->contexts, " "));
  field("Tags", join(task->tags, " "));
  field("Location", task->location);
  if (!task->checklist.empty()) {
    size_t done = 0;
    for (const auto& item : task->checklist) done += item.done ? 1 : 0;
    add("Checklist (" + std::to_string(done) + " of " + std::to_string(task->checklist.size()) +
            " done)",
        TextSize::Large, Color::Black, 24);
    for (const auto& item : task->checklist) {
      add(std::string(item.done ? "[x] " : "[ ] ") + item.title, TextSize::Medium,
          item.done ? Color::DarkGray : Color::Black, 6);
    }
  }
  if (!task->description.empty()) {
    add("Notes", TextSize::Large, Color::Black, 24);
    add(task->description, TextSize::Medium, Color::Black, 6);
  }

  // Pages.
  int y = bodyTop();
  for (size_t i = 0; i < detailLines_.size(); ++i) {
    const Line& line = detailLines_[i];
    const int h = line.gapBefore + display_.textHeight(line.size);
    if (i == 0 || y + h > bodyBottom()) {
      detailPages_.push_back(i);
      y = bodyTop();
    }
    y += (y == bodyTop() ? 0 : line.gapBefore) + display_.textHeight(line.size) + 4;
  }
  if (detailPage_ >= detailPages_.size()) detailPage_ = 0;
}

// --- Drawing --------------------------------------------------------------------

std::string TasksApp::statusText() const {
  switch (phase_) {
    case Phase::Loading:
      return "Loading...";
    case Phase::Connecting:
    case Phase::SyncingTime:
    case Phase::Downloading:
      return operation_ == Operation::MarkDone ? "Getting the latest tasks..." : "Updating...";
    case Phase::Saving:
      return "Saving...";
    case Phase::Idle:
      break;
  }
  if (!error_.empty()) return error_;
  if (!notice_.empty()) return notice_;
  if (lastSync_ >= 0) {
    const time_t seconds = time_t(lastSync_);
    struct tm local;
    localtime_r(&seconds, &local);
    char text[16];
    strftime(text, sizeof(text), "%H:%M", &local);
    return std::string("Updated ") + text;
  }
  if (!dav_.configured()) return "Set [sync] in config.toml";
  if (!network_.configured()) return "Set [wifi] in config.toml";
  return loaded_ ? "Saved copy" : "";
}

paperos::NetworkState TasksApp::wifiState() const {
  switch (phase_) {
    case Phase::Connecting:
      return NetworkState::Connecting;
    case Phase::SyncingTime:
    case Phase::Downloading:
    case Phase::Saving:
      return NetworkState::Connected;
    default:
      return NetworkState::Off;
  }
}

void TasksApp::drawHeader(paperos::Display& display) const {
  const int w = display.width();
  const int titleH = display.textHeight(TextSize::Title);
  const int smallH = display.textHeight(TextSize::Small);
  display.drawText("Tasks", kMargin, kMargin, TextSize::Title);
  const int batteryY = kMargin + (titleH - smallH) / 2;
  const int batteryW =
      paperos::drawBatteryLevel(display, battery_, w - kMargin, batteryY, TextAlign::Right);
  const int wifiH = smallH * 2 / 3;
  paperos::drawWifiStatus(display, wifiState(), w - kMargin - batteryW - (batteryW > 0 ? 12 : 0),
                          batteryY + (smallH - wifiH) / 2, wifiH);
  const int headerBottom = kMargin * 2 + titleH;
  display.fillRect(0, headerBottom - 4, w, 4, Color::Black);
}

void TasksApp::drawFooter(paperos::Display& display, const char* legend,
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

void TasksApp::drawList(paperos::Display& display) {
  const int w = display.width();
  const int maxW = w - 2 * kMargin;
  if (!loaded_) {
    const char* title = "No tasks yet";
    std::string detail;
    if (!dav_.configured()) {
      detail = "Set [sync] in config.toml";
    } else if (!network_.configured()) {
      detail = "Set [wifi] in config.toml";
    } else if (phase_ != Phase::Idle) {
      title = "Getting your tasks...";
    } else {
      detail = "Hold A to try again";
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

  // The page with the selected task on it.
  size_t pageIndex = listPage_;
  if (selectedRow_ >= 0) {
    for (size_t p = 0; p < pages_.size(); ++p) {
      if (std::find(pages_[p].begin(), pages_[p].end(), size_t(selectedRow_)) !=
          pages_[p].end()) {
        pageIndex = p;
      }
    }
  }
  int y = bodyTop();
  if (pageIndex < pages_.size()) {
    for (const size_t i : pages_[pageIndex]) {
      const Row& row = rows_[i];
      const int h = rowHeight(row);
      switch (row.kind) {
        case Row::Kind::Heading:
          display.drawText(row.text.c_str(), kMargin, y + kRowPad, TextSize::Large);
          display.fillRect(kMargin, y + h - 4, maxW, 2, Color::DarkGray);
          break;
        case Row::Kind::Empty:
          display.drawText(row.text.c_str(), kMargin, y + kRowPad, TextSize::Small,
                           TextAlign::Left, Color::DarkGray);
          break;
        case Row::Kind::Task: {
          const bool selected = int(i) == selectedRow_;
          if (selected) display.fillRect(kMargin / 2, y + 2, w - kMargin, h - 4, Color::Black);
          const Color text = selected ? Color::White : Color::Black;
          const Color dim = selected ? Color::White : Color::DarkGray;
          display.drawText(fitToWidth(display, row.text, TextSize::Medium, maxW - 8).c_str(),
                           kMargin + 8, y + kRowPad, TextSize::Medium, TextAlign::Left, text);
          if (!row.detail.empty()) {
            display.drawText(fitToWidth(display, row.detail, TextSize::Small, maxW - 8).c_str(),
                             kMargin + 8, y + kRowPad + display.textHeight(TextSize::Medium) + 4,
                             TextSize::Small, TextAlign::Left, dim);
          }
          break;
        }
      }
      y += h;
    }
  }
  const std::string page = pages_.size() > 1 ? std::to_string(pageIndex + 1) + " / " +
                                                   std::to_string(pages_.size())
                                             : "";
  drawFooter(display,
             selectedRow_ >= 0 ? "A / B: move    Hold A: menu    Hold B: back"
                               : "A / B: page    Hold A: update    Hold B: back",
             page);
}

void TasksApp::drawMenu(paperos::Display& display) {
  const mindwtr::Task* task = data_.find(menuTaskId_);
  if (task == nullptr) return;
  const int w = display.width();
  const int maxW = w - 2 * kMargin;
  int y = bodyTop();
  const std::vector<std::string> title =
      wrap(task->title.empty() ? "(untitled)" : task->title, TextSize::Large, maxW);
  for (size_t i = 0; i < title.size() && i < 3; ++i) {
    const std::string line =
        i == 2 && title.size() > 3 ? fitToWidth(display, title[i] + "...", TextSize::Large, maxW)
                                   : title[i];
    display.drawText(line.c_str(), kMargin, y, TextSize::Large);
    y += display.textHeight(TextSize::Large) + 4;
  }
  const std::string detail = taskDetail(*task, false);
  if (!detail.empty()) {
    display.drawText(fitToWidth(display, detail, TextSize::Small, maxW).c_str(), kMargin, y + 4,
                     TextSize::Small, TextAlign::Left, Color::DarkGray);
    y += display.textHeight(TextSize::Small) + 4;
  }
  y += kMargin;

  const int rowH = display.textHeight(TextSize::Large) + 24;
  for (size_t i = 0; i < kMenuItemCount; ++i) {
    const bool selected = i == menuIndex_;
    const bool disabled = MenuItem(i) == MenuItem::MarkDone && task->recurring;
    if (selected) display.fillRect(kMargin / 2, y, w - kMargin, rowH, Color::Black);
    display.drawText(kMenuItems[i], kMargin * 2, y + 12, TextSize::Large, TextAlign::Left,
                     selected ? Color::White : (disabled ? Color::DarkGray : Color::Black));
    y += rowH + 8;
  }
  if (task->recurring) {
    display.drawText("Repeating: complete it in Mindwtr", kMargin, y + 8, TextSize::Small,
                     TextAlign::Left, Color::DarkGray);
  }
  drawFooter(display, "A / B: move    Hold A: choose    Hold B: back", "");
}

void TasksApp::drawDetails(paperos::Display& display) {
  if (detailPages_.empty()) return;
  const size_t first = detailPages_[detailPage_];
  const size_t end =
      detailPage_ + 1 < detailPages_.size() ? detailPages_[detailPage_ + 1] : detailLines_.size();
  int y = bodyTop();
  for (size_t i = first; i < end; ++i) {
    const Line& line = detailLines_[i];
    if (i != first) y += line.gapBefore;
    display.drawText(line.text.c_str(), kMargin, y, line.size, TextAlign::Left, line.color);
    y += display.textHeight(line.size) + 4;
  }
  const std::string page = detailPages_.size() > 1 ? std::to_string(detailPage_ + 1) + " / " +
                                                         std::to_string(detailPages_.size())
                                                   : "";
  drawFooter(display,
             detailPages_.size() > 1 ? "A / B: page    Hold B: back" : "Hold B: back", page);
}

void TasksApp::draw(paperos::Display& display) {
  fullRefreshPending_ = false;
  drawHeader(display);
  switch (screen_) {
    case Screen::List:
      drawList(display);
      break;
    case Screen::Menu:
      drawMenu(display);
      break;
    case Screen::Details:
      drawDetails(display);
      break;
  }
}

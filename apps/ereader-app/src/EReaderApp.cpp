#include "EReaderApp.h"

#include <algorithm>
#include <cctype>

using ereader::BookIndexer;
using ereader::BookPages;
using ereader::IndexFooter;
using ereader::PageMetrics;
using paperos::Color;
using paperos::DirEntry;
using paperos::InputEvent;
using paperos::TextAlign;
using paperos::TextSize;

namespace {

constexpr int kMargin = 24;
constexpr int kFooterGap = 16;
constexpr int kLibraryColumns = 3;
constexpr int kCoverGap = 16;         // between covers, both ways
constexpr int kSelectionBorder = 5;   // drawn in the gap around the cover
constexpr int kPlaceholderPadding = 6;
constexpr int kPreviewRowGap = 4;
// The ribbon on a bookmarked page, in the right margin at the top.
constexpr int kRibbonW = 14;
constexpr int kRibbonH = 40;
constexpr int kRibbonNotch = 7;
constexpr int kProgressRedrawStep = 10;  // percent; e-paper redraws are slow
// Fast updates ghost, so clear the panel every so often.
constexpr int kFastPageTurnsPerFullRefresh = 8;

struct FontChoice {
  const char* name;
  TextSize body;
  TextSize heading;
};

// In increasing size. Saved by index, so add new sizes at the end.
const FontChoice kFontChoices[] = {
    {"Small", TextSize::Small, TextSize::Large},
    {"Medium", TextSize::Medium, TextSize::Large},
    {"Large", TextSize::ExtraLarge, TextSize::Title},
};
constexpr size_t kFontChoiceCount = sizeof(kFontChoices) / sizeof(kFontChoices[0]);

struct LineChoice {
  const char* name;
  int percent;  // of the font's own line height
};

// In increasing size. Saved by index, so add new heights at the end.
const LineChoice kLineChoices[] = {
    {"100%", 100},
    {"115%", 115},
    {"130%", 130},
    {"150%", 150},
};
constexpr size_t kLineChoiceCount = sizeof(kLineChoices) / sizeof(kLineChoices[0]);

bool endsWithEpub(const std::string& name) {
  static const char kExtension[] = ".epub";
  const size_t size = sizeof(kExtension) - 1;
  if (name.size() <= size) return false;
  return std::equal(name.end() - size, name.end(), kExtension, [](char a, char b) {
    return std::tolower(uint8_t(a)) == b;
  });
}

std::string withoutExtension(const std::string& name) { return name.substr(0, name.rfind('.')); }

bool titleLess(const EReaderApp::Book& a, const EReaderApp::Book& b) {
  return std::lexicographical_compare(
      a.title.begin(), a.title.end(), b.title.begin(), b.title.end(),
      [](char x, char y) { return std::tolower(uint8_t(x)) < std::tolower(uint8_t(y)); });
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

// Breaks `text` between words into at most `maxLines` lines that fit in
// `maxWidth`, ending with "..." if it doesn't all fit.
std::vector<std::string> wrapToLines(const paperos::Display& display, const std::string& text,
                                     TextSize size, int maxWidth, size_t maxLines) {
  std::vector<std::string> lines;
  std::string line;
  size_t pos = 0;
  while (pos < text.size() && lines.size() < maxLines) {
    size_t end = text.find(' ', pos);
    if (end == std::string::npos) end = text.size();
    const std::string word = text.substr(pos, end - pos);
    const std::string longer = line.empty() ? word : line + " " + word;
    // A word too wide for a line of its own is shortened below.
    if (line.empty() || display.textWidth(longer.c_str(), size) <= maxWidth) {
      line = longer;
      pos = end + 1;
    } else {
      lines.push_back(line);
      line.clear();
    }
  }
  if (!line.empty()) lines.push_back(line);
  if (pos < text.size() && !lines.empty()) lines.back() += "...";
  for (std::string& fitted : lines) fitted = fitToWidth(display, fitted, size, maxWidth);
  return lines;
}

std::string progressText(const EReaderApp::Book& book) {
  if (book.pageCount == 0) return "Not indexed yet";
  const uint32_t left = ereader::pagesRemaining(book.page, book.pageCount);
  if (left == 0) return "Finished, " + std::to_string(book.pageCount) + " pages";
  return std::to_string(left) + " of " + std::to_string(book.pageCount) + " pages left";
}

// The first line of text on a laid-out page, for the bookmarks list.
std::string firstLine(const std::string& page) {
  size_t start = 0;
  while (start < page.size()) {
    size_t end = page.find('\n', start);
    if (end == std::string::npos) end = page.size();
    if (page[start] == ereader::kHeadingMarker) ++start;
    if (end > start) return page.substr(start, end - start);
    start = end + 1;
  }
  return "";
}

// Draws the screen's title with the battery level opposite, and a rule
// under them. Returns the bottom of the header.
int drawHeader(paperos::Display& display, const paperos::Battery& battery, const char* title) {
  const int w = display.width();
  const int titleH = display.textHeight(TextSize::Title);
  display.drawText(title, kMargin, kMargin, TextSize::Title);
  const int batteryY = kMargin + (titleH - display.textHeight(TextSize::Small)) / 2;
  drawBatteryLevel(display, battery, w - kMargin, batteryY, TextAlign::Right);
  const int headerBottom = kMargin * 2 + titleH;
  display.fillRect(0, headerBottom - 4, w, 4, Color::Black);
  return headerBottom;
}

void drawFooterLegend(paperos::Display& display, const char* legend) {
  const int footerH = display.textHeight(TextSize::Small);
  const int footerY = display.height() - kMargin - footerH;
  display.drawLine(0, footerY - 12, display.width(), footerY - 12, Color::DarkGray);
  display.drawText(legend, display.width() / 2, footerY, TextSize::Small, TextAlign::Center,
                   Color::DarkGray);
}

}  // namespace

EReaderApp::EReaderApp(paperos::FileSystem& fs, paperos::Display& display,
                       const paperos::Battery& battery)
    : fs_(fs), display_(display), battery_(battery) {
  applyStyle(0, 0);
}

PageMetrics EReaderApp::metricsFor(size_t fontLevel, size_t lineLevel) const {
  const FontChoice& font = kFontChoices[fontLevel];
  const int percent = kLineChoices[lineLevel].percent;

  PageMetrics metrics;
  metrics.body.measure(display_, font.body);
  metrics.heading.measure(display_, font.heading);
  metrics.body.lineHeight = metrics.body.lineHeight * percent / 100;
  metrics.heading.lineHeight = metrics.heading.lineHeight * percent / 100;
  metrics.width = display_.width() - 2 * kMargin;
  // The bottom of the screen is reserved for the page count.
  metrics.height =
      display_.height() - 2 * kMargin - display_.textHeight(TextSize::Small) - kFooterGap;
  metrics.paragraphGap = metrics.body.lineHeight / 2;
  return metrics;
}

void EReaderApp::applyStyle(size_t fontLevel, size_t lineLevel) {
  fontLevel_ = fontLevel;
  lineLevel_ = lineLevel;
  metrics_ = metricsFor(fontLevel, lineLevel);
  fingerprint_ = ereader::layoutFingerprint(metrics_);
}

void EReaderApp::onOpen() {
  view_ = View::Library;
  selected_ = 0;
  const ereader::ReaderSettings settings = ereader::loadSettings(fs_);
  applyStyle(settings.fontLevel < kFontChoiceCount ? settings.fontLevel : 0,
             settings.lineLevel < kLineChoiceCount ? settings.lineLevel : 0);
  scanLibrary();
}

void EReaderApp::onClose() { closeBook(); }

void EReaderApp::scanLibrary() {
  books_.clear();
  coversPending_ = false;
  std::vector<DirEntry> entries;
  libraryAvailable_ = fs_.list(ereader::kBooksDir, entries);

  for (const DirEntry& entry : entries) {
    // Names starting with '.' include the "._x.epub" files macOS leaves behind.
    if (entry.isDirectory || entry.name[0] == '.' || !endsWithEpub(entry.name)) continue;

    Book book;
    book.fileName = entry.name;
    book.title = withoutExtension(entry.name);
    book.stem = ereader::cacheStem(entry.name, entry.size);

    BookPages pages;
    if (pages.open(fs_, book.stem, fingerprint_)) {
      if (pages.title()[0] != '\0') book.title = pages.title();
      book.pageCount = pages.pageCount();
      book.page = pages.pageAt(ereader::loadPosition(fs_, book.stem));
    }

    switch (ereader::coverState(fs_, book.stem)) {
      case ereader::CoverState::Ready: book.cover = ereader::coverPath(book.stem); break;
      case ereader::CoverState::None: break;
      case ereader::CoverState::Unknown: book.coverPending = coversPending_ = true; break;
    }
    books_.push_back(std::move(book));
  }
  std::sort(books_.begin(), books_.end(), titleLess);
}

EReaderApp::LibraryGrid EReaderApp::libraryGrid() const {
  const int smallH = display_.textHeight(TextSize::Small);
  LibraryGrid grid;
  grid.top = kMargin * 3 + display_.textHeight(TextSize::Title);
  // Under the grid: the selected book's title and progress, then the legend.
  const int legendTop = display_.height() - kMargin - smallH - 12;
  grid.detailY = legendTop - 12 - display_.textHeight(TextSize::Medium) - 4 - smallH;

  grid.columnW = (display_.width() - 2 * kMargin - (kLibraryColumns - 1) * kCoverGap) /
                 kLibraryColumns;
  // Covers are 2:3, like most books. As many rows as come closest to filling
  // the screen with covers a column wide, which are then shrunk to fit.
  const int idealH = grid.columnW * 3 / 2;
  const int space = grid.detailY - grid.top;
  const int rows = std::max(1, (space + idealH / 2) / (idealH + kCoverGap));
  grid.coverH = std::max(1, std::min(idealH, space / rows - kCoverGap));
  grid.coverW = std::max(1, std::min(grid.columnW, grid.coverH * 2 / 3));
  grid.perScreen = size_t(rows) * kLibraryColumns;
  return grid;
}

// Looks for the cover of one book that hasn't been searched yet, starting
// with those on screen. Returns true when the screen should be redrawn: once
// every book on it has been searched, so the covers appear together.
bool EReaderApp::loadNextCover() {
  if (!coversPending_) return false;
  const LibraryGrid grid = libraryGrid();
  const size_t first = selected_ / grid.perScreen * grid.perScreen;
  const size_t last = std::min(first + grid.perScreen, books_.size());
  const auto nextPending = [this](size_t from, size_t to) {
    while (from < to && !books_[from].coverPending) ++from;
    return from;
  };

  size_t index = nextPending(first, last);
  const bool onScreen = index < last;
  if (!onScreen) index = nextPending(0, books_.size());
  if (index >= books_.size()) {
    coversPending_ = false;
    return false;
  }

  Book& book = books_[index];
  book.coverPending = false;
  const std::string path = std::string(ereader::kBooksDir) + "/" + book.fileName;
  if (ereader::buildCover(fs_, display_, path, book.stem, grid.coverW, grid.coverH)) {
    book.cover = ereader::coverPath(book.stem);
    if (onScreen) coversChanged_ = true;
  }
  return onScreen && coversChanged_ && nextPending(first, last) == last;
}

void EReaderApp::openSelected() {
  const Book& book = books_[selected_];
  IndexFooter footer;
  if (ereader::readIndexFooter(fs_, book.stem, fingerprint_, footer)) {
    startReading();
    return;
  }

  indexer_.reset(new BookIndexer(fs_, metrics_));
  drawnPercent_ = 0;
  const std::string path = std::string(ereader::kBooksDir) + "/" + book.fileName;
  if (indexer_->start(path, book.stem)) {
    view_ = View::Indexing;
  } else {
    showError(indexer_->error());
  }
}

void EReaderApp::startReading() {
  const Book& book = books_[selected_];
  fullRefreshPending_ = true;
  fastPageTurns_ = 0;
  if (!pages_.open(fs_, book.stem, fingerprint_)) {
    showError("Could not open the book's index");
    return;
  }
  page_ = pages_.pageAt(ereader::loadPosition(fs_, book.stem));
  bookmarks_ = ereader::loadBookmarks(fs_, book.stem);
  if (loadPage()) view_ = View::Reading;
}

void EReaderApp::showError(const char* message) {
  error_ = message;
  closeBook();
  view_ = View::Error;
  fullRefreshPending_ = true;
}

void EReaderApp::closeBook() {
  indexer_.reset();
  pages_.close();
  pageText_.clear();
  bookmarks_.clear();
  bookmarkPages_.clear();
}

void EReaderApp::backToLibrary() {
  const std::string fileName = books_[selected_].fileName;
  closeBook();
  view_ = View::Library;
  fullRefreshPending_ = true;

  // Indexing may have replaced the title, moving the book in the sort order.
  scanLibrary();
  selected_ = 0;
  for (size_t i = 0; i < books_.size(); ++i) {
    if (books_[i].fileName == fileName) selected_ = i;
  }
}

bool EReaderApp::loadPage() {
  if (pages_.readPage(page_, pageText_)) return true;
  showError("Could not read this page");
  return false;
}

bool EReaderApp::turnPage(int delta) {
  if (delta < 0 && page_ == 0) return false;
  if (delta > 0 && page_ + 1 >= pages_.pageCount()) return false;

  page_ += delta;
  if (loadPage()) {
    ereader::savePosition(fs_, books_[selected_].stem, pages_.positionOf(page_));
    if (++fastPageTurns_ >= kFastPageTurnsPerFullRefresh) fullRefreshPending_ = true;
  }
  return true;
}

std::vector<uint32_t> EReaderApp::pagesWithBookmarks() const {
  // Bookmarks are in order of position, so their pages are too. Two made at
  // another font size can share a page at this one.
  std::vector<uint32_t> pages;
  for (const uint32_t position : bookmarks_) {
    const uint32_t page = pages_.pageAt(position);
    if (pages.empty() || pages.back() != page) pages.push_back(page);
  }
  return pages;
}

bool EReaderApp::pageBookmarked() const {
  return std::any_of(bookmarks_.begin(), bookmarks_.end(),
                     [this](uint32_t position) { return pages_.pageAt(position) == page_; });
}

// A bookmark made at another font size may fall anywhere on this page, so
// removing takes away every bookmark on it.
void EReaderApp::toggleBookmark() {
  const auto onPage = [this](uint32_t position) { return pages_.pageAt(position) == page_; };
  if (pageBookmarked()) {
    bookmarks_.erase(std::remove_if(bookmarks_.begin(), bookmarks_.end(), onPage),
                     bookmarks_.end());
  } else {
    const uint32_t position = pages_.positionOf(page_);
    bookmarks_.insert(std::lower_bound(bookmarks_.begin(), bookmarks_.end(), position),
                      position);
  }
  ereader::saveBookmarks(fs_, books_[selected_].stem, bookmarks_);
}

void EReaderApp::openBookmarks() {
  view_ = View::Bookmarks;
  fullRefreshPending_ = true;
  bookmarkPages_ = pagesWithBookmarks();
  // Starts at the nearest bookmark before the page being read.
  selectedBookmark_ = 0;
  for (size_t i = 0; i < bookmarkPages_.size() && bookmarkPages_[i] <= page_; ++i) {
    selectedBookmark_ = i;
  }
}

bool EReaderApp::onBookmarksInput(InputEvent event) {
  const size_t count = bookmarkPages_.size();
  switch (event) {
    case InputEvent::Previous:
      if (count == 0) return false;
      selectedBookmark_ = (selectedBookmark_ + count - 1) % count;
      return true;
    case InputEvent::Next:
      if (count == 0) return false;
      selectedBookmark_ = (selectedBookmark_ + 1) % count;
      return true;
    case InputEvent::Select:
      if (count == 0) return false;
      page_ = bookmarkPages_[selectedBookmark_];
      if (!loadPage()) return true;
      ereader::savePosition(fs_, books_[selected_].stem, pages_.positionOf(page_));
      closeMenu();
      return true;
    case InputEvent::Back:
      view_ = View::Menu;
      fullRefreshPending_ = true;
      return true;
    default:
      return false;
  }
}

void EReaderApp::openMenu() {
  view_ = View::Menu;
  menuItem_ = MenuItem::FontSize;
  fullRefreshPending_ = true;
}

void EReaderApp::closeMenu() {
  view_ = View::Reading;
  fullRefreshPending_ = true;
}

bool EReaderApp::onMenuInput(InputEvent event) {
  const int count = int(MenuItem::Count);
  switch (event) {
    case InputEvent::Previous:
      menuItem_ = MenuItem((int(menuItem_) + count - 1) % count);
      return true;
    case InputEvent::Next:
      menuItem_ = MenuItem((int(menuItem_) + 1) % count);
      return true;
    case InputEvent::Back:
      closeMenu();
      return true;
    case InputEvent::Select:
      switch (menuItem_) {
        case MenuItem::BackToLibrary: backToLibrary(); break;
        case MenuItem::Bookmarks: openBookmarks(); break;
        default: openPreview(); break;
      }
      return true;
    default:
      return false;
  }
}

void EReaderApp::openPreview() {
  view_ = View::Preview;
  previewFontLevel_ = fontLevel_;
  previewLineLevel_ = lineLevel_;
  layoutPreview();
  fullRefreshPending_ = true;
}

bool EReaderApp::onPreviewInput(InputEvent event) {
  switch (event) {
    case InputEvent::Previous:
      return adjustPreview(1);
    case InputEvent::Next:
      return adjustPreview(-1);
    case InputEvent::Select:
      confirmPreview();
      return true;
    case InputEvent::Back:
      view_ = View::Menu;
      fullRefreshPending_ = true;
      return true;
    default:
      return false;
  }
}

bool EReaderApp::adjustPreview(int delta) {
  const bool font = menuItem_ == MenuItem::FontSize;
  size_t& level = font ? previewFontLevel_ : previewLineLevel_;
  const int next = int(level) + delta;
  if (next < 0 || next >= int(font ? kFontChoiceCount : kLineChoiceCount)) return false;
  level = size_t(next);
  layoutPreview();
  return true;
}

void EReaderApp::confirmPreview() {
  if (previewFontLevel_ == fontLevel_ && previewLineLevel_ == lineLevel_) {
    closeMenu();
    return;
  }

  // The saved position works at any layout, so reopening the book finds the
  // same text, indexing it for the new style first if needed.
  applyStyle(previewFontLevel_, previewLineLevel_);
  ereader::ReaderSettings settings;
  settings.fontLevel = uint32_t(fontLevel_);
  settings.lineLevel = uint32_t(lineLevel_);
  ereader::saveSettings(fs_, settings);
  closeBook();
  openSelected();
  fullRefreshPending_ = true;
}

void EReaderApp::layoutPreview() {
  previewMetrics_ = metricsFor(previewFontLevel_, previewLineLevel_);
  previewMetrics_.height -= display_.textHeight(TextSize::Small) + kPreviewRowGap;

  // Lays out from the current page, reading on until one full page is ready.
  previewText_.clear();
  ereader::PageLayout layout(previewMetrics_, [this](const std::string& page) {
    if (previewText_.empty()) previewText_ = page;
  });
  std::string text = pageText_;
  uint32_t page = page_;
  for (;;) {
    ereader::feedLaidOutPage(text, layout);
    if (!previewText_.empty() || ++page >= pages_.pageCount()) break;
    if (!pages_.readPage(page, text)) break;
  }
  layout.chapterBreak();
}

bool EReaderApp::onInput(InputEvent event) {
  switch (view_) {
    case View::Library:
      if (books_.empty()) return false;
      switch (event) {
        case InputEvent::Previous:
          selected_ = (selected_ + books_.size() - 1) % books_.size();
          return true;
        case InputEvent::Next:
          selected_ = (selected_ + 1) % books_.size();
          return true;
        case InputEvent::Select:
          openSelected();
          return true;
        default:
          return false;  // Back falls through to the shell, which closes the app.
      }

    case View::Reading:
      if (event == InputEvent::Previous) return turnPage(-1);
      if (event == InputEvent::Next) return turnPage(1);
      if (event == InputEvent::Select) {
        toggleBookmark();
        return true;
      }
      if (event == InputEvent::Back) {
        openMenu();
        return true;
      }
      return false;

    case View::Menu:
      return onMenuInput(event);

    case View::Preview:
      return onPreviewInput(event);

    case View::Bookmarks:
      return onBookmarksInput(event);

    case View::Indexing:
    case View::Error:
      break;
  }

  if (event == InputEvent::Back) {
    backToLibrary();
    return true;
  }
  return false;
}

bool EReaderApp::tick() {
  if (view_ == View::Library) return loadNextCover();
  if (view_ != View::Indexing) return false;

  switch (indexer_->step()) {
    case BookIndexer::Status::Working:
      if (indexer_->percent() < drawnPercent_ + kProgressRedrawStep) return false;
      drawnPercent_ = indexer_->percent();
      return true;
    case BookIndexer::Status::Done:
      indexer_.reset();
      startReading();
      return true;
    case BookIndexer::Status::Failed:
      showError(indexer_->error());
      return true;
  }
  return false;
}

bool EReaderApp::wantsFullRefresh() const { return fullRefreshPending_; }

void EReaderApp::draw(paperos::Display& display) {
  switch (view_) {
    case View::Library: drawLibrary(display); break;
    case View::Indexing: drawIndexing(display); break;
    case View::Reading: drawReading(display); break;
    case View::Menu: drawMenu(display); break;
    case View::Preview: drawPreview(display); break;
    case View::Bookmarks: drawBookmarks(display); break;
    case View::Error: drawError(display); break;
  }
  if (fullRefreshPending_) fastPageTurns_ = 0;
  fullRefreshPending_ = false;
}

void EReaderApp::drawLibrary(paperos::Display& display) {
  const int w = display.width();

  const int titleH = display.textHeight(TextSize::Title);
  display.drawText("Books", kMargin, kMargin, TextSize::Title);
  const int batteryY = kMargin + (titleH - display.textHeight(TextSize::Small)) / 2;
  drawBatteryLevel(display, battery_, w - kMargin, batteryY, TextAlign::Right);
  const int headerBottom = kMargin * 2 + titleH;
  display.fillRect(0, headerBottom - 4, w, 4, Color::Black);

  drawFooterLegend(display, "A / B: move    Hold A: open    Hold B: back");

  const int top = headerBottom + kMargin;
  if (books_.empty()) {
    display.drawText(libraryAvailable_ ? "No books found" : "No /books folder", w / 2, top,
                     TextSize::Large, TextAlign::Center);
    display.drawText("Copy .epub files to /books", w / 2,
                     top + display.textHeight(TextSize::Large) + 16, TextSize::Small,
                     TextAlign::Center, Color::DarkGray);
    display.drawText("on the SD card, then reopen this app", w / 2,
                     top + display.textHeight(TextSize::Large) + 16 +
                         display.textHeight(TextSize::Small) + 4,
                     TextSize::Small, TextAlign::Center, Color::DarkGray);
    return;
  }

  // One screenful at a time, which keeps redraws to a minimum on e-paper.
  const LibraryGrid grid = libraryGrid();
  const size_t first = selected_ / grid.perScreen * grid.perScreen;
  const size_t last = std::min(first + grid.perScreen, books_.size());
  for (size_t i = first; i < last; ++i) {
    const int column = int(i - first) % kLibraryColumns;
    const int row = int(i - first) / kLibraryColumns;
    const int x = kMargin + column * (grid.columnW + kCoverGap) + (grid.columnW - grid.coverW) / 2;
    const int y = grid.top + row * (grid.coverH + kCoverGap);
    if (i == selected_) {
      display.fillRect(x - kSelectionBorder, y - kSelectionBorder,
                       grid.coverW + 2 * kSelectionBorder, grid.coverH + 2 * kSelectionBorder,
                       Color::Black);
    }
    drawCover(display, books_[i], x, y, grid.coverW, grid.coverH);
    if (i != selected_) {
      display.drawRect(x - 1, y - 1, grid.coverW + 2, grid.coverH + 2, Color::DarkGray);
    }
  }
  coversChanged_ = false;

  const Book& book = books_[selected_];
  const std::string title = fitToWidth(display, book.title, TextSize::Medium, w - 2 * kMargin);
  display.drawText(title.c_str(), w / 2, grid.detailY, TextSize::Medium, TextAlign::Center);
  display.drawText(progressText(book).c_str(), w / 2,
                   grid.detailY + display.textHeight(TextSize::Medium) + 4, TextSize::Small,
                   TextAlign::Center, Color::DarkGray);
}

// The book's cover, or its title in a blank box if it has none that can be
// drawn.
void EReaderApp::drawCover(paperos::Display& display, const Book& book, int x, int y, int w,
                           int h) const {
  if (!book.cover.empty() && display.drawImageAt(book.cover.c_str(), x, y, w, h)) return;

  display.fillRect(x, y, w, h, Color::White);
  const int lineH = display.textHeight(TextSize::Small);
  const std::vector<std::string> lines =
      wrapToLines(display, book.title, TextSize::Small, w - 2 * kPlaceholderPadding,
                  size_t(std::max(1, (h - 2 * kPlaceholderPadding) / lineH)));
  int lineY = y + (h - int(lines.size()) * lineH) / 2;
  for (const std::string& line : lines) {
    display.drawText(line.c_str(), x + w / 2, lineY, TextSize::Small, TextAlign::Center);
    lineY += lineH;
  }
}

void EReaderApp::drawIndexing(paperos::Display& display) {
  const int w = display.width();
  const int largeH = display.textHeight(TextSize::Large);
  const int mediumH = display.textHeight(TextSize::Medium);

  int y = display.height() / 3;
  display.drawText("Indexing book", w / 2, y, TextSize::Large, TextAlign::Center);
  y += largeH + 16;
  const std::string title =
      fitToWidth(display, books_[selected_].title, TextSize::Medium, w - 2 * kMargin);
  display.drawText(title.c_str(), w / 2, y, TextSize::Medium, TextAlign::Center);
  y += mediumH + 32;

  const int barW = w - 4 * kMargin;
  const int barH = 24;
  display.drawRect(2 * kMargin, y, barW, barH, Color::Black);
  display.fillRect(2 * kMargin, y, barW * indexer_->percent() / 100, barH, Color::Black);
  y += barH + 16;
  display.drawText("Only needed once per font size", w / 2, y, TextSize::Small,
                   TextAlign::Center, Color::DarkGray);

  drawFooterLegend(display, "Hold B: cancel");
}

void EReaderApp::drawPageText(paperos::Display& display, const std::string& text,
                              const PageMetrics& metrics) const {
  int y = kMargin;
  size_t start = 0;
  while (start < text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();

    if (end == start) {
      y += metrics.paragraphGap;
    } else if (text[start] == ereader::kHeadingMarker) {
      const std::string line = text.substr(start + 1, end - start - 1);
      display.drawText(line.c_str(), kMargin, y, metrics.heading.size);
      y += metrics.heading.lineHeight;
    } else {
      const std::string line = text.substr(start, end - start);
      display.drawText(line.c_str(), kMargin, y, metrics.body.size);
      y += metrics.body.lineHeight;
    }
    start = end + 1;
  }
}

void EReaderApp::drawReading(paperos::Display& display) {
  drawPageText(display, pageText_, metrics_);

  const int footerY = display.height() - kMargin - display.textHeight(TextSize::Small);
  const std::string position =
      "Page " + std::to_string(page_ + 1) + " of " + std::to_string(pageCount());
  const uint32_t left = pagesLeft();
  const std::string remaining =
      left == 0 ? "Last page" : std::to_string(left) + (left == 1 ? " page left" : " pages left");
  display.drawText(position.c_str(), kMargin, footerY, TextSize::Small, TextAlign::Left,
                   Color::DarkGray);
  drawBatteryLevel(display, battery_, display.width() / 2, footerY, TextAlign::Center);
  display.drawText(remaining.c_str(), display.width() - kMargin, footerY, TextSize::Small,
                   TextAlign::Right, Color::DarkGray);
  if (pageBookmarked()) drawBookmarkRibbon(display);
}

// A ribbon hanging from the top of the page, with a notch cut in its end.
// It sits in the right margin, clear of the text.
void EReaderApp::drawBookmarkRibbon(paperos::Display& display) const {
  const int x = display.width() - kMargin + (kMargin - kRibbonW) / 2;
  display.fillRect(x, 0, kRibbonW, kRibbonH, Color::Black);
  for (int row = 0; row < kRibbonNotch; ++row) {
    const int half = row * (kRibbonW / 2) / kRibbonNotch;
    display.fillRect(x + kRibbonW / 2 - half, kRibbonH - kRibbonNotch + row, 2 * half + 1, 1,
                     Color::White);
  }
}

void EReaderApp::drawMenu(paperos::Display& display) {
  const int w = display.width();
  const int headerBottom = drawHeader(display, battery_, "Menu");

  const std::string labels[] = {
      std::string("Font size: ") + kFontChoices[fontLevel_].name,
      std::string("Line height: ") + kLineChoices[lineLevel_].name,
      bookmarks_.empty() ? std::string("Bookmarks")
                         : "Bookmarks (" + std::to_string(pagesWithBookmarks().size()) + ")",
      "Back to library",
  };
  int y = headerBottom + kMargin;
  const int rowH = display.textHeight(TextSize::Large) + 24;
  for (int i = 0; i < int(MenuItem::Count); ++i) {
    const bool selected = i == int(menuItem_);
    if (selected) display.fillRect(kMargin / 2, y, w - kMargin, rowH, Color::Black);
    display.drawText(labels[i].c_str(), kMargin * 2, y + 12, TextSize::Large, TextAlign::Left,
                     selected ? Color::White : Color::Black);
    y += rowH + 8;
  }

  drawFooterLegend(display, "A / B: move    Hold A: select    Hold B: close");
}

size_t EReaderApp::bookmarkRowsPerScreen() const {
  const int top = kMargin * 3 + display_.textHeight(TextSize::Title);
  // Leaves room for the legend, and the count under the list.
  const int bottom = display_.height() - kMargin - 2 * display_.textHeight(TextSize::Small) - 32;
  const int rowH = display_.textHeight(TextSize::Large) + display_.textHeight(TextSize::Small) + 24;
  return size_t(std::max(1, (bottom - top) / (rowH + 8)));
}

void EReaderApp::drawBookmarks(paperos::Display& display) {
  const int w = display.width();
  const int headerBottom = drawHeader(display, battery_, "Bookmarks");
  int y = headerBottom + kMargin;

  if (bookmarkPages_.empty()) {
    drawFooterLegend(display, "Hold B: back");
    display.drawText("No bookmarks yet", w / 2, y, TextSize::Large, TextAlign::Center);
    display.drawText("Hold A on a page to bookmark it", w / 2,
                     y + display.textHeight(TextSize::Large) + 16, TextSize::Small,
                     TextAlign::Center, Color::DarkGray);
    return;
  }
  drawFooterLegend(display, "A / B: move    Hold A: go to page    Hold B: back");

  // One screenful at a time, like the library.
  const size_t perScreen = bookmarkRowsPerScreen();
  const size_t first = selectedBookmark_ / perScreen * perScreen;
  const size_t last = std::min(first + perScreen, bookmarkPages_.size());
  const int largeH = display.textHeight(TextSize::Large);
  const int rowH = largeH + display.textHeight(TextSize::Small) + 24;
  std::string text;
  for (size_t i = first; i < last; ++i) {
    const bool selected = i == selectedBookmark_;
    const Color ink = selected ? Color::White : Color::Black;
    if (selected) display.fillRect(kMargin / 2, y, w - kMargin, rowH, Color::Black);
    const uint32_t page = bookmarkPages_[i];
    const std::string label = "Page " + std::to_string(page + 1);
    display.drawText(label.c_str(), kMargin * 2, y + 8, TextSize::Large, TextAlign::Left, ink);
    if (pages_.readPage(page, text)) {
      const std::string line =
          fitToWidth(display, firstLine(text), TextSize::Small, w - 4 * kMargin);
      display.drawText(line.c_str(), kMargin * 2, y + 12 + largeH, TextSize::Small,
                       TextAlign::Left, selected ? Color::White : Color::DarkGray);
    }
    y += rowH + 8;
  }
  if (bookmarkPages_.size() > perScreen) {
    const std::string count =
        std::to_string(selectedBookmark_ + 1) + " of " + std::to_string(bookmarkPages_.size());
    display.drawText(count.c_str(), w / 2, y, TextSize::Small, TextAlign::Center,
                     Color::DarkGray);
  }
}

void EReaderApp::drawPreview(paperos::Display& display) {
  drawPageText(display, previewText_, previewMetrics_);

  const int smallH = display.textHeight(TextSize::Small);
  const int legendY = display.height() - kMargin - smallH;
  const int labelY = legendY - smallH - kPreviewRowGap;
  display.drawLine(0, labelY - kFooterGap / 2, display.width(), labelY - kFooterGap / 2,
                   Color::DarkGray);
  const std::string label =
      menuItem_ == MenuItem::FontSize
          ? std::string("Font size: ") + kFontChoices[previewFontLevel_].name
          : std::string("Line height: ") + kLineChoices[previewLineLevel_].name;
  display.drawText(label.c_str(), display.width() / 2, labelY, TextSize::Small,
                   TextAlign::Center);
  display.drawText("A: +    B: -    Hold A: set    Hold B: cancel", display.width() / 2, legendY,
                   TextSize::Small, TextAlign::Center, Color::DarkGray);
}

void EReaderApp::drawError(paperos::Display& display) {
  const int w = display.width();
  int y = display.height() / 3;
  display.drawText("Can't open book", w / 2, y, TextSize::Large, TextAlign::Center);
  y += display.textHeight(TextSize::Large) + 16;
  const std::string message = fitToWidth(display, error_, TextSize::Small, w - 2 * kMargin);
  display.drawText(message.c_str(), w / 2, y, TextSize::Small, TextAlign::Center);
  drawFooterLegend(display, "Hold B: back");
}

// An open book: two pages with lines of text.
void EReaderApp::drawIcon(paperos::Display& display, int x, int y, int s, Color color) const {
  const int t = paperos::iconStroke(s);
  const int top = y + s / 8;
  const int h = s * 3 / 4;
  const int half = s / 2 + t / 2;
  paperos::strokeRect(display, x, top, half, h, t, color);
  paperos::strokeRect(display, x + s - half, top, half, h, t, color);
  const int line = t > 2 ? t - 1 : 2;
  for (int i = 1; i <= 3; ++i) {
    const int ly = top + h * i / 4;
    paperos::strokeLine(display, x + s / 8, ly, x + s / 2 - s / 8, ly, line, color);
    paperos::strokeLine(display, x + s / 2 + s / 8, ly, x + s - s / 8, ly, line, color);
  }
}

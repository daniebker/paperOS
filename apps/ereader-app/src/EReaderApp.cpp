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
constexpr int kPreviewRowGap = 4;
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

std::string progressText(const EReaderApp::Book& book) {
  if (book.pageCount == 0) return "Not indexed yet";
  const uint32_t left = ereader::pagesRemaining(book.page, book.pageCount);
  if (left == 0) return "Finished, " + std::to_string(book.pageCount) + " pages";
  return std::to_string(left) + " of " + std::to_string(book.pageCount) + " pages left";
}

void drawFooterLegend(paperos::Display& display, const char* legend) {
  const int footerH = display.textHeight(TextSize::Small);
  const int footerY = display.height() - kMargin - footerH;
  display.drawLine(0, footerY - 12, display.width(), footerY - 12, Color::DarkGray);
  display.drawText(legend, display.width() / 2, footerY, TextSize::Small, TextAlign::Center,
                   Color::DarkGray);
}

}  // namespace

EReaderApp::EReaderApp(paperos::FileSystem& fs, const paperos::Display& display,
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
    books_.push_back(std::move(book));
  }
  std::sort(books_.begin(), books_.end(), titleLess);
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
      if (menuItem_ == MenuItem::BackToLibrary) {
        backToLibrary();
      } else {
        openPreview();
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
      if (event == InputEvent::Back) {
        openMenu();
        return true;
      }
      return false;

    case View::Menu:
      return onMenuInput(event);

    case View::Preview:
      return onPreviewInput(event);

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
    case View::Error: drawError(display); break;
  }
  if (fullRefreshPending_) fastPageTurns_ = 0;
  fullRefreshPending_ = false;
}

void EReaderApp::drawLibrary(paperos::Display& display) {
  const int w = display.width();
  const int h = display.height();

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
  const int titleRowH = display.textHeight(TextSize::Medium);
  const int detailRowH = display.textHeight(TextSize::Small);
  const int rowH = titleRowH + detailRowH + 24;
  const int bottom = h - kMargin - detailRowH - 24;
  const size_t rowsPerScreen = size_t(std::max(1, (bottom - top) / (rowH + 8)));
  const size_t first = selected_ / rowsPerScreen * rowsPerScreen;
  const size_t last = std::min(first + rowsPerScreen, books_.size());

  int y = top;
  for (size_t i = first; i < last; ++i) {
    const bool selected = i == selected_;
    if (selected) display.fillRect(kMargin / 2, y, w - kMargin, rowH, Color::Black);
    const std::string title =
        fitToWidth(display, books_[i].title, TextSize::Medium, w - kMargin * 3);
    display.drawText(title.c_str(), kMargin * 3 / 2, y + 8, TextSize::Medium, TextAlign::Left,
                     selected ? Color::White : Color::Black);
    display.drawText(progressText(books_[i]).c_str(), kMargin * 3 / 2, y + 8 + titleRowH + 4,
                     TextSize::Small, TextAlign::Left,
                     selected ? Color::LightGray : Color::DarkGray);
    y += rowH + 8;
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
}

void EReaderApp::drawMenu(paperos::Display& display) {
  const int w = display.width();

  const int titleH = display.textHeight(TextSize::Title);
  display.drawText("Menu", kMargin, kMargin, TextSize::Title);
  const int batteryY = kMargin + (titleH - display.textHeight(TextSize::Small)) / 2;
  drawBatteryLevel(display, battery_, w - kMargin, batteryY, TextAlign::Right);
  const int headerBottom = kMargin * 2 + titleH;
  display.fillRect(0, headerBottom - 4, w, 4, Color::Black);

  const std::string labels[] = {
      std::string("Font size: ") + kFontChoices[fontLevel_].name,
      std::string("Line height: ") + kLineChoices[lineLevel_].name,
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

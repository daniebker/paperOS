#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <paperos.h>

#include "BookCache.h"

// Reads EPUB books from /books on the SD card.
//
// The first time a book is opened it is laid out for this screen and the
// result is cached on the card, so reopening it is instant. The last page
// read is saved on every page turn. See BookCache.h for the file formats.
//
// The library is a grid of covers, three to a row, in order of title. Covers
// are read from the books one at a time in tick() and cached on the card.
//
// In the library A / B move, hold A opens and hold B returns home. In a book
// A / B turn pages and hold B opens the menu, where A / B move, hold A selects
// and hold B returns to the book.
//
// The font size and line height items open a preview of the current page. A /
// B increase and decrease the value, hold A sets it and hold B leaves without
// saving. The style is remembered for every book.
class EReaderApp : public paperos::App {
 public:
  enum class View : uint8_t { Library, Indexing, Reading, Menu, Preview, Error };
  enum class MenuItem : uint8_t { FontSize, LineHeight, BackToLibrary, Count };

  struct Book {
    std::string fileName;
    std::string title;  // from the index, or the file name until indexed
    std::string stem;   // see ereader::cacheStem()
    std::string cover;  // image to draw in the library; "" for none, or not yet
    bool coverPending = false;  // the book hasn't been searched for a cover yet
    uint32_t page = 0;
    uint32_t pageCount = 0;  // 0 until indexed
  };

  // `display` is used to measure text, and to scale covers down.
  EReaderApp(paperos::FileSystem& fs, paperos::Display& display, const paperos::Battery& battery);

  const char* name() const override { return "E-Reader"; }
  void onOpen() override;
  void onClose() override;
  bool onInput(paperos::InputEvent event) override;
  bool tick() override;
  bool wantsFullRefresh() const override;
  void draw(paperos::Display& display) override;

  View view() const { return view_; }
  const std::vector<Book>& books() const { return books_; }
  size_t selectedIndex() const { return selected_; }
  MenuItem selectedMenuItem() const { return menuItem_; }
  size_t fontLevel() const { return fontLevel_; }
  size_t lineLevel() const { return lineLevel_; }
  size_t previewFontLevel() const { return previewFontLevel_; }
  size_t previewLineLevel() const { return previewLineLevel_; }
  const std::string& previewText() const { return previewText_; }
  uint32_t currentPage() const { return page_; }  // zero-based
  uint32_t pageCount() const { return pages_.pageCount(); }
  uint32_t pagesLeft() const { return ereader::pagesRemaining(page_, pageCount()); }
  uint32_t layoutFingerprint() const { return fingerprint_; }

  // Books on one screen of the library.
  size_t booksPerScreen() const { return libraryGrid().perScreen; }

 private:
  // Where the library's covers go on this screen.
  struct LibraryGrid {
    int top = 0;       // of the first row
    int columnW = 0;   // covers are centred in their column
    int coverW = 0;
    int coverH = 0;
    int detailY = 0;   // top of the selected book's title, under the grid
    size_t perScreen = 0;
  };

  LibraryGrid libraryGrid() const;
  bool loadNextCover();
  void drawCover(paperos::Display& display, const Book& book, int x, int y, int w, int h) const;

  ereader::PageMetrics metricsFor(size_t fontLevel, size_t lineLevel) const;
  void applyStyle(size_t fontLevel, size_t lineLevel);
  void scanLibrary();
  void openSelected();
  void startReading();
  void showError(const char* message);
  void closeBook();
  void backToLibrary();
  bool turnPage(int delta);
  void openMenu();
  void closeMenu();
  bool onMenuInput(paperos::InputEvent event);
  void openPreview();
  bool onPreviewInput(paperos::InputEvent event);
  bool adjustPreview(int delta);
  void confirmPreview();
  void layoutPreview();
  bool loadPage();

  void drawLibrary(paperos::Display& display);
  void drawIndexing(paperos::Display& display);
  void drawReading(paperos::Display& display);
  void drawMenu(paperos::Display& display);
  void drawPreview(paperos::Display& display);
  void drawPageText(paperos::Display& display, const std::string& text,
                    const ereader::PageMetrics& metrics) const;
  void drawError(paperos::Display& display);

  paperos::FileSystem& fs_;
  paperos::Display& display_;
  const paperos::Battery& battery_;
  size_t fontLevel_ = 0;
  size_t lineLevel_ = 0;
  ereader::PageMetrics metrics_;
  uint32_t fingerprint_ = 0;

  View view_ = View::Library;
  std::vector<Book> books_;
  bool libraryAvailable_ = false;
  size_t selected_ = 0;
  bool coversPending_ = false;  // some book still has coverPending set
  bool coversChanged_ = false;  // a cover on screen was found since the last draw
  MenuItem menuItem_ = MenuItem::FontSize;

  // Values being tried in the preview, applied only when confirmed.
  size_t previewFontLevel_ = 0;
  size_t previewLineLevel_ = 0;
  ereader::PageMetrics previewMetrics_;
  std::string previewText_;

  std::unique_ptr<ereader::BookIndexer> indexer_;
  int drawnPercent_ = 0;

  ereader::BookPages pages_;
  uint32_t page_ = 0;
  std::string pageText_;
  std::string error_;

  int fastPageTurns_ = 0;
  bool fullRefreshPending_ = false;
};

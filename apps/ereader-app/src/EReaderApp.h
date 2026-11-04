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
// In the library A / B move, hold A opens and hold B returns home. In a book
// A / B turn pages and hold B returns to the library.
class EReaderApp : public paperos::App {
 public:
  enum class View : uint8_t { Library, Indexing, Reading, Error };

  struct Book {
    std::string fileName;
    std::string title;  // from the index, or the file name until indexed
    std::string stem;   // see ereader::cacheStem()
    uint32_t page = 0;
    uint32_t pageCount = 0;  // 0 until indexed
  };

  EReaderApp(paperos::FileSystem& fs, const paperos::Display& display);

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
  uint32_t currentPage() const { return page_; }  // zero-based
  uint32_t pageCount() const { return pages_.pageCount(); }
  uint32_t pagesLeft() const { return ereader::pagesRemaining(page_, pageCount()); }

 private:
  void scanLibrary();
  void openSelected();
  void startReading();
  void showError(const char* message);
  void closeBook();
  void backToLibrary();
  bool turnPage(int delta);
  bool loadPage();

  void drawLibrary(paperos::Display& display);
  void drawIndexing(paperos::Display& display);
  void drawReading(paperos::Display& display);
  void drawError(paperos::Display& display);

  paperos::FileSystem& fs_;
  ereader::PageMetrics metrics_;
  uint32_t fingerprint_;

  View view_ = View::Library;
  std::vector<Book> books_;
  bool libraryAvailable_ = false;
  size_t selected_ = 0;

  std::unique_ptr<ereader::BookIndexer> indexer_;
  int drawnPercent_ = 0;

  ereader::BookPages pages_;
  uint32_t page_ = 0;
  std::string pageText_;
  std::string error_;

  int fastPageTurns_ = 0;
  bool fullRefreshPending_ = false;
};

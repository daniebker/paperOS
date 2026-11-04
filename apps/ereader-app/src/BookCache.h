#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <paperos.h>

#include "Epub.h"
#include "PageLayout.h"

namespace ereader {

constexpr const char* kBooksDir = "/books";
constexpr const char* kCacheDir = "/books/.cache";
constexpr const char* kSettingsPath = "/books/.cache/settings.bin";

// Each indexed book has these files in kCacheDir, named after cacheStem():
//   <stem>-<layout>.idx  the laid-out text of every page, then the tables
//                        described in IndexFooter, then the footer. One per
//                        layout, so going back to a font size already used
//                        needs no re-indexing.
//   <stem>.pos           the place last read, which applies to every layout
// Reading a page from the .idx is a single seek, so page turns never touch
// the EPUB. The index is written under a temporary name and renamed once it
// is complete, so a power cut can't leave a partial one behind.
// kSettingsPath is shared by all books.

// Stored at the very end of an .idx file.
struct IndexFooter {
  uint32_t magic = 0;
  uint32_t fingerprint = 0;  // layoutFingerprint() the index was built with
  uint32_t pageCount = 0;
  // File offset of two uint32_t tables: pageCount + 1 file offsets of the
  // pages, then the position of the start of each of the pageCount pages.
  uint32_t tableOffset = 0;
  char title[96] = {};
  char author[64] = {};
};

// Cache file path without extension. Depends on the file's name and size, so
// replacing a book with a different file re-indexes it.
std::string cacheStem(const std::string& fileName, uint32_t fileSize);

// Identifies a page layout. An index built for other metrics (a different
// font or screen size) doesn't match and is rebuilt.
uint32_t layoutFingerprint(const PageMetrics& metrics);

// Returns false if there is no complete index for this layout.
bool readIndexFooter(paperos::FileSystem& fs, const std::string& stem, uint32_t fingerprint,
                     IndexFooter& footer);

// A place in a book is the number of visible characters before it, ignoring
// the spaces and line breaks that layout changes. That makes it the same at
// every font size, unlike a page number.

// 0, the start of the book, if it has not been opened before.
uint32_t loadPosition(paperos::FileSystem& fs, const std::string& stem);
bool savePosition(paperos::FileSystem& fs, const std::string& stem, uint32_t position);

// Reader-wide font size choice, 0 if never saved.
uint32_t loadFontLevel(paperos::FileSystem& fs);
bool saveFontLevel(paperos::FileSystem& fs, uint32_t level);

// Pages left after `page` (zero-based), so 0 on the last page.
uint32_t pagesRemaining(uint32_t page, uint32_t pageCount);

// Builds the index one chapter at a time, so the app stays responsive.
class BookIndexer {
 public:
  enum class Status : uint8_t { Working, Done, Failed };

  BookIndexer(paperos::FileSystem& fs, const PageMetrics& metrics) : fs_(fs), metrics_(metrics) {}
  ~BookIndexer() { cancel(); }

  // Returns false if the book can't be read; see error().
  bool start(const std::string& bookPath, const std::string& stem);
  // Lays out the next chapter, or finishes the index after the last one.
  Status step();
  // Abandons the index and removes its temporary file.
  void cancel();

  int percent() const;
  const char* error() const { return error_; }

 private:
  Status fail(const char* message);
  Status finish();
  void writePage(const std::string& page);

  paperos::FileSystem& fs_;
  const PageMetrics& metrics_;

  Epub epub_;
  std::unique_ptr<PageLayout> layout_;
  std::unique_ptr<paperos::File> out_;
  std::vector<uint32_t> offsets_;
  std::vector<uint32_t> pagePositions_;
  uint32_t visibleChars_ = 0;
  std::string stem_;
  size_t nextChapter_ = 0;
  uint64_t totalBytes_ = 0;
  uint64_t doneBytes_ = 0;
  bool writeFailed_ = false;
  const char* error_ = "";
};

// Random access to the pages of an indexed book.
class BookPages {
 public:
  bool open(paperos::FileSystem& fs, const std::string& stem, uint32_t fingerprint);
  void close();

  uint32_t pageCount() const { return footer_.pageCount; }
  const char* title() const { return footer_.title; }  // "" if the book has none
  bool readPage(uint32_t page, std::string& text);

  // Zero-based page holding `position`, which may come from another layout.
  uint32_t pageAt(uint32_t position) const;
  uint32_t positionOf(uint32_t page) const { return pagePositions_[page]; }

 private:
  std::unique_ptr<paperos::File> file_;
  std::vector<uint32_t> offsets_;
  std::vector<uint32_t> pagePositions_;
  IndexFooter footer_;
};

}  // namespace ereader

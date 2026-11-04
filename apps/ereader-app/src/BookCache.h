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

// Each indexed book has two files in kCacheDir, named after cacheStem():
//   <stem>.idx  the laid-out text of every page, then a table of page offsets,
//               then an IndexFooter
//   <stem>.pos  the last page read
// Reading a page from the .idx is a single seek, so page turns never touch
// the EPUB. The index is written under a temporary name and renamed once it
// is complete, so a power cut can't leave a partial one behind.

// Stored at the very end of an .idx file.
struct IndexFooter {
  uint32_t magic = 0;
  uint32_t fingerprint = 0;  // layoutFingerprint() the index was built with
  uint32_t pageCount = 0;
  uint32_t tableOffset = 0;  // file offset of pageCount + 1 uint32_t offsets
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

// Zero-based page last read, 0 for a book not opened before.
uint32_t loadPosition(paperos::FileSystem& fs, const std::string& stem);
bool savePosition(paperos::FileSystem& fs, const std::string& stem, uint32_t page);

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
  bool readPage(uint32_t page, std::string& text);

 private:
  std::unique_ptr<paperos::File> file_;
  std::vector<uint32_t> offsets_;
  IndexFooter footer_;
};

}  // namespace ereader

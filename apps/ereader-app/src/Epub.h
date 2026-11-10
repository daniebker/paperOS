#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "ZipArchive.h"

namespace ereader {

// An EPUB (2 or 3) file: its title, author and chapters in reading order.
class Epub {
 public:
  // Follows META-INF/container.xml to the package document and reads it.
  // Returns false if this isn't a valid EPUB with at least one chapter.
  bool open(std::unique_ptr<paperos::File> file);

  const std::string& title() const { return title_; }    // "" if not set
  const std::string& author() const { return author_; }  // "" if not set

  size_t chapterCount() const { return chapters_.size(); }
  // Decompressed size of a chapter, for progress reporting.
  uint32_t chapterBytes(size_t index) const { return chapters_[index].uncompressedSize; }
  // Streams a chapter's XHTML to `sink`.
  bool readChapter(size_t index, const ByteSink& sink);

 private:
  bool parsePackage(const std::string& packagePath, const std::string& xml);

  ZipArchive zip_;
  std::vector<ZipEntry> chapters_;
  std::string title_;
  std::string author_;
};

// Resolves an href from a package document at `basePath` to a ZIP entry name:
// strips any #fragment, decodes %XX escapes and collapses "." and "..".
std::string resolveHref(const std::string& basePath, const std::string& href);

}  // namespace ereader

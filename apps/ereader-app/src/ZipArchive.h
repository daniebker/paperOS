#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Inflate.h"

namespace ereader {

struct ZipEntry {
  std::string name;
  uint16_t flags = 0;
  uint16_t method = 0;
  uint32_t compressedSize = 0;
  uint32_t uncompressedSize = 0;
  uint32_t headerOffset = 0;
};

// Read-only ZIP reader: stored and deflated entries, no ZIP64 or encryption.
class ZipArchive {
 public:
  // Reads the central directory. Returns false if this isn't a usable ZIP.
  bool open(std::unique_ptr<paperos::File> file);

  // Returns nullptr if there is no entry with that exact name.
  const ZipEntry* find(const std::string& name) const;

  // Streams the decompressed entry to `sink`.
  bool read(const ZipEntry& entry, const ByteSink& sink);
  // Reads a whole entry, refusing one larger than `maxBytes`.
  bool readToString(const ZipEntry& entry, std::string& out, size_t maxBytes);

 private:
  std::unique_ptr<paperos::File> file_;
  std::vector<ZipEntry> entries_;
};

}  // namespace ereader

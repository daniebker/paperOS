#include "ZipArchive.h"

#include <algorithm>
#include <cstring>

namespace ereader {

namespace {

constexpr uint32_t kEndRecordSignature = 0x06054B50;
constexpr uint32_t kCentralHeaderSignature = 0x02014B50;
constexpr uint32_t kLocalHeaderSignature = 0x04034B50;
constexpr size_t kEndRecordSize = 22;
constexpr size_t kCentralHeaderSize = 46;
constexpr size_t kLocalHeaderSize = 30;
constexpr size_t kMaxCommentSize = 65535;
constexpr uint32_t kMaxCentralDirectoryBytes = 1024 * 1024;
constexpr size_t kCopyChunkBytes = 4096;
constexpr uint16_t kFlagEncrypted = 1;
constexpr uint16_t kMethodStored = 0;
constexpr uint16_t kMethodDeflated = 8;

uint16_t readU16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }

uint32_t readU32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

bool readAt(paperos::File& file, uint32_t position, void* buffer, size_t size) {
  return file.seek(position) && file.read(buffer, size) == size;
}

}  // namespace

bool ZipArchive::open(std::unique_ptr<paperos::File> file) {
  entries_.clear();
  file_ = std::move(file);
  if (!file_) return false;

  // The end record is the last thing in the file, before an optional comment.
  const uint32_t fileSize = file_->size();
  if (fileSize < kEndRecordSize) return false;
  const uint32_t tailSize = uint32_t(std::min<size_t>(fileSize, kEndRecordSize + kMaxCommentSize));
  std::vector<uint8_t> tail(tailSize);
  if (!readAt(*file_, fileSize - tailSize, tail.data(), tailSize)) return false;

  const uint8_t* end = nullptr;
  for (size_t i = tailSize - kEndRecordSize + 1; i-- > 0;) {
    if (readU32(&tail[i]) == kEndRecordSignature) {
      end = &tail[i];
      break;
    }
  }
  if (end == nullptr) return false;

  const uint16_t entryCount = readU16(end + 10);
  const uint32_t directorySize = readU32(end + 12);
  const uint32_t directoryOffset = readU32(end + 16);
  if (directorySize > kMaxCentralDirectoryBytes) return false;

  std::vector<uint8_t> directory(directorySize);
  if (!readAt(*file_, directoryOffset, directory.data(), directorySize)) return false;

  entries_.reserve(entryCount);
  size_t pos = 0;
  for (uint16_t i = 0; i < entryCount; ++i) {
    if (pos + kCentralHeaderSize > directorySize) return false;
    const uint8_t* header = &directory[pos];
    if (readU32(header) != kCentralHeaderSignature) return false;

    const size_t nameSize = readU16(header + 28);
    const size_t extraSize = readU16(header + 30);
    const size_t commentSize = readU16(header + 32);
    if (pos + kCentralHeaderSize + nameSize > directorySize) return false;

    ZipEntry entry;
    entry.flags = readU16(header + 8);
    entry.method = readU16(header + 10);
    entry.compressedSize = readU32(header + 20);
    entry.uncompressedSize = readU32(header + 24);
    entry.headerOffset = readU32(header + 42);
    entry.name.assign(reinterpret_cast<const char*>(header + kCentralHeaderSize), nameSize);
    entries_.push_back(std::move(entry));

    pos += kCentralHeaderSize + nameSize + extraSize + commentSize;
  }
  return true;
}

const ZipEntry* ZipArchive::find(const std::string& name) const {
  for (const ZipEntry& entry : entries_) {
    if (entry.name == name) return &entry;
  }
  return nullptr;
}

bool ZipArchive::read(const ZipEntry& entry, const ByteSink& sink) {
  if (!file_ || (entry.flags & kFlagEncrypted) != 0) return false;

  // The local header's name and extra lengths can differ from the central
  // directory's, so the data offset has to be read from here.
  uint8_t header[kLocalHeaderSize];
  if (!readAt(*file_, entry.headerOffset, header, sizeof(header))) return false;
  if (readU32(header) != kLocalHeaderSignature) return false;
  const uint32_t dataOffset =
      entry.headerOffset + kLocalHeaderSize + readU16(header + 26) + readU16(header + 28);
  if (!file_->seek(dataOffset)) return false;

  size_t produced = 0;
  const ByteSink counting = [&](const uint8_t* data, size_t size) {
    produced += size;
    return sink(data, size);
  };

  bool ok = false;
  if (entry.method == kMethodDeflated) {
    ok = inflate(*file_, entry.compressedSize, counting);
  } else if (entry.method == kMethodStored) {
    std::vector<uint8_t> chunk(kCopyChunkBytes);
    size_t left = entry.compressedSize;
    ok = true;
    while (ok && left > 0) {
      const size_t got = file_->read(chunk.data(), std::min(left, chunk.size()));
      ok = got > 0 && counting(chunk.data(), got);
      left -= got;
    }
  }
  return ok && produced == entry.uncompressedSize;
}

bool ZipArchive::readToString(const ZipEntry& entry, std::string& out, size_t maxBytes) {
  out.clear();
  if (entry.uncompressedSize > maxBytes) return false;
  out.reserve(entry.uncompressedSize);
  return read(entry, [&](const uint8_t* data, size_t size) {
    out.append(reinterpret_cast<const char*>(data), size);
    return true;
  });
}

}  // namespace ereader

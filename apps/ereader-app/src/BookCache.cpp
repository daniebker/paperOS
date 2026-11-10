#include "BookCache.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace ereader {

namespace {

constexpr uint32_t kIndexMagic = 0x32494250;     // "PBI2"; bump when the format changes
constexpr uint32_t kPositionMagic = 0x32504250;  // "PBP2"
constexpr uint32_t kSettingsMagic = 0x31534250;  // "PBS1"

// FNV-1a.
uint32_t hashBytes(const void* data, size_t size, uint32_t hash = 2166136261u) {
  const uint8_t* bytes = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 16777619u;
  return hash;
}

uint32_t hashInt(int32_t value, uint32_t hash) { return hashBytes(&value, sizeof(value), hash); }

uint32_t hashFont(const FontMetrics& font, uint32_t hash) {
  hash = hashInt(font.lineHeight, hash);
  return hashBytes(font.widths, sizeof(font.widths), hash);
}

std::string indexPath(const std::string& stem, uint32_t fingerprint) {
  char suffix[16];
  snprintf(suffix, sizeof(suffix), "-%08x.idx", unsigned(fingerprint));
  return stem + suffix;
}
std::string temporaryPath(const std::string& stem) { return stem + ".tmp"; }
std::string positionPath(const std::string& stem) { return stem + ".pos"; }

void copyTruncated(char* destination, size_t capacity, const std::string& text) {
  strncpy(destination, text.c_str(), capacity - 1);
  destination[capacity - 1] = '\0';
}

}  // namespace

std::string cacheStem(const std::string& fileName, uint32_t fileSize) {
  const uint32_t hash = hashBytes(&fileSize, sizeof(fileSize),
                                  hashBytes(fileName.data(), fileName.size()));
  char name[16];
  snprintf(name, sizeof(name), "%08x", unsigned(hash));
  return std::string(kCacheDir) + "/" + name;
}

uint32_t layoutFingerprint(const PageMetrics& metrics) {
  uint32_t hash = hashInt(metrics.width, 2166136261u);
  hash = hashInt(metrics.height, hash);
  hash = hashInt(metrics.paragraphGap, hash);
  hash = hashFont(metrics.body, hash);
  return hashFont(metrics.heading, hash);
}

bool readIndexFooter(paperos::FileSystem& fs, const std::string& stem, uint32_t fingerprint,
                     IndexFooter& footer) {
  std::unique_ptr<paperos::File> file =
      fs.open(indexPath(stem, fingerprint).c_str(), paperos::FileMode::Read);
  if (!file || file->size() < sizeof(IndexFooter)) return false;
  if (!file->seek(file->size() - sizeof(IndexFooter))) return false;
  if (file->read(&footer, sizeof(footer)) != sizeof(footer)) return false;
  return footer.magic == kIndexMagic && footer.fingerprint == fingerprint && footer.pageCount > 0;
}

uint32_t loadPosition(paperos::FileSystem& fs, const std::string& stem) {
  std::unique_ptr<paperos::File> file =
      fs.open(positionPath(stem).c_str(), paperos::FileMode::Read);
  uint32_t record[2] = {0, 0};
  if (!file || file->read(record, sizeof(record)) != sizeof(record)) return 0;
  return record[0] == kPositionMagic ? record[1] : 0;
}

bool savePosition(paperos::FileSystem& fs, const std::string& stem, uint32_t position) {
  std::unique_ptr<paperos::File> file =
      fs.open(positionPath(stem).c_str(), paperos::FileMode::Write);
  const uint32_t record[2] = {kPositionMagic, position};
  return file && file->write(record, sizeof(record)) == sizeof(record);
}

ReaderSettings loadSettings(paperos::FileSystem& fs) {
  // New fields go at the end: older files simply stop early, leaving them 0.
  ReaderSettings settings;
  std::unique_ptr<paperos::File> file = fs.open(kSettingsPath, paperos::FileMode::Read);
  uint32_t record[3] = {0, 0, 0};
  if (!file) return settings;
  const size_t size = file->read(record, sizeof(record));
  if (size < 2 * sizeof(uint32_t) || record[0] != kSettingsMagic) return settings;
  settings.fontLevel = record[1];
  settings.lineLevel = record[2];
  return settings;
}

bool saveSettings(paperos::FileSystem& fs, const ReaderSettings& settings) {
  fs.makeDir(kCacheDir);  // fails harmlessly if it exists
  std::unique_ptr<paperos::File> file = fs.open(kSettingsPath, paperos::FileMode::Write);
  const uint32_t record[3] = {kSettingsMagic, settings.fontLevel, settings.lineLevel};
  return file && file->write(record, sizeof(record)) == sizeof(record);
}

uint32_t pagesRemaining(uint32_t page, uint32_t pageCount) {
  return page + 1 < pageCount ? pageCount - page - 1 : 0;
}

bool BookIndexer::start(const std::string& bookPath, const std::string& stem) {
  stem_ = stem;
  if (!epub_.open(fs_.open(bookPath.c_str(), paperos::FileMode::Read))) {
    error_ = "This file is not a readable EPUB";
    return false;
  }

  fs_.makeDir(kCacheDir);  // fails harmlessly if it exists
  out_ = fs_.open(temporaryPath(stem_).c_str(), paperos::FileMode::Write);
  if (!out_) {
    error_ = "Could not write to the SD card";
    return false;
  }

  totalBytes_ = 0;
  for (size_t i = 0; i < epub_.chapterCount(); ++i) totalBytes_ += epub_.chapterBytes(i);
  offsets_.assign(1, 0);
  pagePositions_.clear();
  visibleChars_ = 0;
  layout_.reset(new PageLayout(metrics_, [this](const std::string& page) { writePage(page); }));
  return true;
}

void BookIndexer::writePage(const std::string& page) {
  if (out_->write(page.data(), page.size()) != page.size()) writeFailed_ = true;
  offsets_.push_back(offsets_.back() + uint32_t(page.size()));

  pagePositions_.push_back(visibleChars_);
  for (const char c : page) {
    if (c != ' ' && c != '\n' && c != kHeadingMarker) ++visibleChars_;
  }
}

BookIndexer::Status BookIndexer::step() {
  if (nextChapter_ >= epub_.chapterCount()) return finish();

  XhtmlParser parser(*layout_);
  const bool ok = epub_.readChapter(nextChapter_, [&](const uint8_t* data, size_t size) {
    parser.feed(reinterpret_cast<const char*>(data), size);
    return !writeFailed_;
  });
  parser.finish();
  layout_->chapterBreak();
  if (writeFailed_) return fail("Could not write to the SD card");
  if (!ok) return fail("This book is damaged or DRM protected");

  doneBytes_ += epub_.chapterBytes(nextChapter_);
  ++nextChapter_;
  return Status::Working;
}

BookIndexer::Status BookIndexer::finish() {
  if (layout_->pageCount() == 0) return fail("This book has no readable text");

  IndexFooter footer;
  footer.magic = kIndexMagic;
  footer.fingerprint = layoutFingerprint(metrics_);
  footer.pageCount = layout_->pageCount();
  footer.tableOffset = offsets_.back();
  copyTruncated(footer.title, sizeof(footer.title), epub_.title());
  copyTruncated(footer.author, sizeof(footer.author), epub_.author());

  const size_t offsetBytes = offsets_.size() * sizeof(uint32_t);
  const size_t positionBytes = pagePositions_.size() * sizeof(uint32_t);
  const bool written = out_->write(offsets_.data(), offsetBytes) == offsetBytes &&
                       out_->write(pagePositions_.data(), positionBytes) == positionBytes &&
                       out_->write(&footer, sizeof(footer)) == sizeof(footer);
  if (!written) return fail("Could not write to the SD card");
  out_.reset();

  const std::string finalPath = indexPath(stem_, footer.fingerprint);
  fs_.remove(finalPath.c_str());  // a damaged index being replaced
  if (!fs_.rename(temporaryPath(stem_).c_str(), finalPath.c_str())) {
    return fail("Could not write to the SD card");
  }
  layout_.reset();
  stem_.clear();
  return Status::Done;
}

BookIndexer::Status BookIndexer::fail(const char* message) {
  error_ = message;
  cancel();
  return Status::Failed;
}

void BookIndexer::cancel() {
  out_.reset();
  layout_.reset();
  if (!stem_.empty()) fs_.remove(temporaryPath(stem_).c_str());
  stem_.clear();
}

int BookIndexer::percent() const {
  return totalBytes_ == 0 ? 0 : int(doneBytes_ * 100 / totalBytes_);
}

bool BookPages::open(paperos::FileSystem& fs, const std::string& stem, uint32_t fingerprint) {
  close();
  if (!readIndexFooter(fs, stem, fingerprint, footer_)) return false;
  file_ = fs.open(indexPath(stem, fingerprint).c_str(), paperos::FileMode::Read);
  if (!file_) return false;

  offsets_.resize(size_t(footer_.pageCount) + 1);
  pagePositions_.resize(footer_.pageCount);
  const size_t offsetBytes = offsets_.size() * sizeof(uint32_t);
  const size_t positionBytes = pagePositions_.size() * sizeof(uint32_t);
  if (!file_->seek(footer_.tableOffset) ||
      file_->read(offsets_.data(), offsetBytes) != offsetBytes ||
      file_->read(pagePositions_.data(), positionBytes) != positionBytes) {
    close();
    return false;
  }
  return true;
}

void BookPages::close() {
  file_.reset();
  offsets_.clear();
  pagePositions_.clear();
  footer_ = IndexFooter();
}

uint32_t BookPages::pageAt(uint32_t position) const {
  if (pagePositions_.empty()) return 0;
  const auto after = std::upper_bound(pagePositions_.begin(), pagePositions_.end(), position);
  return uint32_t(after - pagePositions_.begin()) - 1;
}

bool BookPages::readPage(uint32_t page, std::string& text) {
  if (!file_ || page >= footer_.pageCount) return false;
  const size_t size = offsets_[page + 1] - offsets_[page];
  text.resize(size);
  return file_->seek(offsets_[page]) && file_->read(&text[0], size) == size;
}

}  // namespace ereader

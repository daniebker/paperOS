#include "SdFileSystem.h"

#include <SD.h>

namespace {

class SdFile : public paperos::File {
 public:
  explicit SdFile(fs::File file) : file_(file) {}
  ~SdFile() override { file_.close(); }

  size_t read(void* buffer, size_t size) override {
    return file_.read(static_cast<uint8_t*>(buffer), size);
  }
  size_t write(const void* data, size_t size) override {
    return file_.write(static_cast<const uint8_t*>(data), size);
  }
  bool seek(uint32_t position) override { return file_.seek(position); }
  uint32_t size() const override { return file_.size(); }

 private:
  mutable fs::File file_;
};

}  // namespace

std::unique_ptr<paperos::File> SdFileSystem::open(const char* path, paperos::FileMode mode) {
  fs::File file = SD.open(path, mode == paperos::FileMode::Write ? FILE_WRITE : FILE_READ);
  if (!file || file.isDirectory()) return nullptr;
  return std::unique_ptr<paperos::File>(new SdFile(file));
}

bool SdFileSystem::list(const char* path, std::vector<paperos::DirEntry>& entries) {
  entries.clear();
  fs::File dir = SD.open(path, FILE_READ);
  if (!dir || !dir.isDirectory()) return false;
  while (fs::File entry = dir.openNextFile()) {
    paperos::DirEntry item;
    item.name = entry.name();
    item.size = entry.size();
    item.isDirectory = entry.isDirectory();
    entries.push_back(item);
  }
  return true;
}

bool SdFileSystem::makeDir(const char* path) { return SD.mkdir(path); }

bool SdFileSystem::remove(const char* path) { return SD.remove(path); }

bool SdFileSystem::rename(const char* from, const char* to) { return SD.rename(from, to); }

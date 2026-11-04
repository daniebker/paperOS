#pragma once

#include <paperos.h>

// paperos::FileSystem backed by the SD card. The card is mounted by
// loadConfigFromSd(); without a card every call simply fails.
class SdFileSystem : public paperos::FileSystem {
 public:
  std::unique_ptr<paperos::File> open(const char* path, paperos::FileMode mode) override;
  bool list(const char* path, std::vector<paperos::DirEntry>& entries) override;
  bool makeDir(const char* path) override;
  bool remove(const char* path) override;
  bool rename(const char* from, const char* to) override;
};

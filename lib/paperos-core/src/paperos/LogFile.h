#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "paperos/Config.h"
#include "paperos/FileSystem.h"

namespace paperos {

// True when debug is on in config.toml:
//
//   [Developer]
//     debug = "true"
//
// The section may also be written [developer].
bool debugEnabled(const Config& config);

// Log lines saved to numbered files at the root of the SD card:
// /debug.0.log, then /debug.1.log once that is full, and so on. Lines are
// kept in memory until flush(), so logging stays cheap and can happen while
// the card is busy.
class LogFile {
 public:
  static constexpr size_t kDefaultMaxBytes = 256 * 1024;
  // Lines kept before flush() if the card can't be written, so a missing
  // card can't use up the memory.
  static constexpr size_t kMaxPendingBytes = 16 * 1024;

  explicit LogFile(FileSystem& fs, size_t maxBytes = kDefaultMaxBytes)
      : fs_(fs), maxBytes_(maxBytes) {}

  // Carries on in the newest existing log file, or starts /debug.0.log.
  void begin();

  // Adds one line; a trailing newline is added if missing.
  void add(const std::string& line);

  // Appends the lines added since the last flush, starting a new file when
  // the current one would grow past maxBytes. Returns false if the card
  // couldn't be written; the lines are then kept for the next try.
  bool flush();

  bool hasPending() const { return !pending_.empty(); }
  int fileNumber() const { return number_; }
  std::string path() const { return pathFor(number_); }
  static std::string pathFor(int number);

 private:
  FileSystem& fs_;
  size_t maxBytes_;
  int number_ = 0;
  size_t size_ = 0;  // bytes in the current file
  std::string pending_;
};

}  // namespace paperos

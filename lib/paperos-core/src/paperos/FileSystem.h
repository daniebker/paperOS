#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace paperos {

// An open file. Closed when destroyed.
class File {
 public:
  virtual ~File() = default;

  // Both return the number of bytes transferred.
  virtual size_t read(void* buffer, size_t size) = 0;
  virtual size_t write(const void* data, size_t size) = 0;

  virtual bool seek(uint32_t position) = 0;
  virtual uint32_t size() const = 0;
};

enum class FileMode : uint8_t {
  Read,
  Write,   // creates the file, or truncates it if it exists
  Append,  // creates the file, or writes at the end of it if it exists
};

struct DirEntry {
  std::string name;  // without the directory
  uint32_t size = 0;
  bool isDirectory = false;
};

// Storage used by apps. Keeping apps on this interface (instead of the SD
// library directly) lets them be unit-tested on the host, like Display.
// Paths are absolute, e.g. "/books/moby-dick.epub".
class FileSystem {
 public:
  virtual ~FileSystem() = default;

  // Returns nullptr if the file cannot be opened.
  virtual std::unique_ptr<File> open(const char* path, FileMode mode) = 0;
  // Replaces `entries` with the contents of `path`. Returns false if it is
  // not a readable directory.
  virtual bool list(const char* path, std::vector<DirEntry>& entries) = 0;
  // Returns false if the directory could not be made, including if it exists.
  virtual bool makeDir(const char* path) = 0;
  virtual bool remove(const char* path) = 0;
  // `to` must not exist.
  virtual bool rename(const char* from, const char* to) = 0;
};

}  // namespace paperos

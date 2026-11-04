#pragma once

#include <string>
#include <vector>

namespace paperos {

// Settings read from config.toml on the SD card.
//
// Supports the subset of TOML paperOS needs:
//   # comments, [section] headers, key = "basic string", key = 'literal string',
//   and bare values (numbers, true/false), which are kept as their raw text.
// Indentation, blank lines, CRLF line endings and a UTF-8 BOM are accepted.
// Lines that cannot be parsed are skipped and counted in errorCount().
class Config {
 public:
  // Replaces the current contents with the parsed text. Returns true when
  // every line parsed cleanly.
  bool parse(const char* text);
  void clear();

  // Returns the value of `key` in `[section]` (use "" for keys before any
  // section header), or `fallback` if it is not set.
  const char* getString(const char* section, const char* key, const char* fallback = "") const;
  bool has(const char* section, const char* key) const;

  size_t size() const { return entries_.size(); }
  int errorCount() const { return errorCount_; }
  // 1-based line number of the first unparsable line, 0 if none.
  int firstErrorLine() const { return firstErrorLine_; }

 private:
  struct Entry {
    std::string section;
    std::string key;
    std::string value;
  };

  const Entry* find(const char* section, const char* key) const;
  void set(const std::string& section, const std::string& key, const std::string& value);
  void recordError(int line);

  std::vector<Entry> entries_;
  int errorCount_ = 0;
  int firstErrorLine_ = 0;
};

}  // namespace paperos

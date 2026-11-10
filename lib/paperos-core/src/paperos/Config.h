#pragma once

#include <string>
#include <vector>

namespace paperos {

// Settings read from config.toml on the SD card.
//
// Supports the subset of TOML paperOS needs:
//   # comments, [section] headers, [[array]] tables, key = "basic string",
//   key = 'literal string', and bare values (numbers, true/false), which are
//   kept as their raw text. Header names may be dotted, e.g. [[calendar.feed]],
//   and are matched as written.
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
  // Returns the value as a whole number, or `fallback` if it is not set or
  // not a whole number.
  long getInt(const char* section, const char* key, long fallback) const;

  // Number of [[table]] headers with this name.
  size_t tableCount(const char* table) const;
  // Returns the value of `key` in the `index`th [[table]], or `fallback`.
  const char* getTableString(const char* table, size_t index, const char* key,
                             const char* fallback = "") const;

  size_t size() const { return entries_.size(); }
  int errorCount() const { return errorCount_; }
  // 1-based line number of the first unparsable line, 0 if none.
  int firstErrorLine() const { return firstErrorLine_; }

 private:
  struct Entry {
    std::string section;
    std::string key;
    std::string value;
    int index;  // position among [[section]] tables, or -1 for [section]
  };

  const Entry* find(const char* section, int index, const char* key) const;
  void set(const std::string& section, int index, const std::string& key,
           const std::string& value);
  void recordError(int line);

  std::vector<Entry> entries_;
  std::vector<std::string> tables_;  // name of every [[table]] header, in order
  int errorCount_ = 0;
  int firstErrorLine_ = 0;
};

}  // namespace paperos

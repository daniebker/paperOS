#include "paperos/Config.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace paperos {

namespace {

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r'; }

bool isBareKeyChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
         c == '_' || c == '-';
}

void skipSpace(const std::string& s, size_t& i) {
  while (i < s.size() && isSpace(s[i])) ++i;
}

// True if only whitespace or a comment remains from position i.
bool atLineEnd(const std::string& s, size_t i) {
  skipSpace(s, i);
  return i >= s.size() || s[i] == '#';
}

bool parseBareKey(const std::string& s, size_t& i, std::string& out) {
  const size_t start = i;
  while (i < s.size() && isBareKeyChar(s[i])) ++i;
  out.assign(s, start, i - start);
  return !out.empty();
}

// A header name: bare keys joined by dots, e.g. "calendar.feed".
bool parseHeaderName(const std::string& s, size_t& i, std::string& out) {
  out.clear();
  std::string part;
  do {
    if (!out.empty()) out += '.';
    skipSpace(s, i);
    if (!parseBareKey(s, i, part)) return false;
    out += part;
    skipSpace(s, i);
  } while (i < s.size() && s[i] == '.' && ++i);
  return true;
}

// Parses a "basic string" starting at the opening quote.
bool parseBasicString(const std::string& s, size_t& i, std::string& out) {
  out.clear();
  ++i;  // opening quote
  while (i < s.size()) {
    char c = s[i++];
    if (c == '"') return true;
    if (c != '\\') {
      out += c;
      continue;
    }
    if (i >= s.size()) return false;
    switch (s[i++]) {
      case '"': out += '"'; break;
      case '\\': out += '\\'; break;
      case 'n': out += '\n'; break;
      case 't': out += '\t'; break;
      case 'r': out += '\r'; break;
      default: return false;  // \uXXXX etc. not supported
    }
  }
  return false;  // unterminated
}

// Parses a 'literal string' starting at the opening quote.
bool parseLiteralString(const std::string& s, size_t& i, std::string& out) {
  const size_t start = ++i;
  while (i < s.size() && s[i] != '\'') ++i;
  if (i >= s.size()) return false;
  out.assign(s, start, i - start);
  ++i;
  return true;
}

// Bare value (number, boolean, ...) up to a comment or end of line.
bool parseBareValue(const std::string& s, size_t& i, std::string& out) {
  const size_t start = i;
  while (i < s.size() && s[i] != '#') ++i;
  size_t end = i;
  while (end > start && isSpace(s[end - 1])) --end;
  out.assign(s, start, end - start);
  return !out.empty();
}

}  // namespace

void Config::clear() {
  entries_.clear();
  tables_.clear();
  errorCount_ = 0;
  firstErrorLine_ = 0;
}

void Config::recordError(int line) {
  if (errorCount_++ == 0) firstErrorLine_ = line;
}

bool Config::parse(const char* text) {
  clear();
  if (text == nullptr) return true;
  if (std::strncmp(text, "\xEF\xBB\xBF", 3) == 0) text += 3;  // UTF-8 BOM

  std::string section;
  int index = -1;
  int lineNo = 0;
  const char* p = text;
  while (*p != '\0') {
    const char* eol = std::strchr(p, '\n');
    const size_t len = eol ? size_t(eol - p) : std::strlen(p);
    const std::string line(p, len);
    p += len + (eol ? 1 : 0);
    ++lineNo;

    size_t i = 0;
    skipSpace(line, i);
    if (atLineEnd(line, i)) continue;  // blank or comment

    if (line[i] == '[') {  // [section] or [[table]]
      const bool isTable = i + 1 < line.size() && line[i + 1] == '[';
      i += isTable ? 2 : 1;
      std::string name;
      if (!parseHeaderName(line, i, name)) {
        recordError(lineNo);
        continue;
      }
      const char* close = isTable ? "]]" : "]";
      const size_t closeLen = isTable ? 2 : 1;
      if (line.compare(i, closeLen, close) != 0 || !atLineEnd(line, i + closeLen)) {
        recordError(lineNo);
        continue;
      }
      section = name;
      index = isTable ? int(tableCount(name.c_str())) : -1;
      if (isTable) tables_.push_back(name);
      continue;
    }

    std::string key;  // key = value
    if (!parseBareKey(line, i, key)) {
      recordError(lineNo);
      continue;
    }
    skipSpace(line, i);
    if (i >= line.size() || line[i] != '=') {
      recordError(lineNo);
      continue;
    }
    ++i;
    skipSpace(line, i);
    if (i >= line.size()) {
      recordError(lineNo);
      continue;
    }

    std::string value;
    bool ok;
    if (line[i] == '"') {
      ok = parseBasicString(line, i, value) && atLineEnd(line, i);
    } else if (line[i] == '\'') {
      ok = parseLiteralString(line, i, value) && atLineEnd(line, i);
    } else {
      ok = parseBareValue(line, i, value);
    }
    if (!ok) {
      recordError(lineNo);
      continue;
    }
    set(section, index, key, value);
  }
  return errorCount_ == 0;
}

void Config::set(const std::string& section, int index, const std::string& key,
                 const std::string& value) {
  for (auto& e : entries_) {
    if (e.section == section && e.index == index && e.key == key) {
      e.value = value;  // last definition wins
      return;
    }
  }
  entries_.push_back(Entry{section, key, value, index});
}

const Config::Entry* Config::find(const char* section, int index, const char* key) const {
  if (section == nullptr) section = "";
  if (key == nullptr) return nullptr;
  for (const auto& e : entries_) {
    if (e.section == section && e.index == index && e.key == key) return &e;
  }
  return nullptr;
}

const char* Config::getString(const char* section, const char* key, const char* fallback) const {
  const Entry* e = find(section, -1, key);
  return e ? e->value.c_str() : fallback;
}

bool Config::has(const char* section, const char* key) const {
  return find(section, -1, key) != nullptr;
}

long Config::getInt(const char* section, const char* key, long fallback) const {
  const Entry* e = find(section, -1, key);
  if (e == nullptr || e->value.empty()) return fallback;
  char* end = nullptr;
  errno = 0;
  const long value = std::strtol(e->value.c_str(), &end, 10);
  return errno == 0 && *end == '\0' ? value : fallback;
}

bool Config::getBool(const char* section, const char* key, bool fallback) const {
  const Entry* e = find(section, -1, key);
  if (e == nullptr) return fallback;
  std::string value = e->value;
  for (char& c : value) {
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  }
  if (value == "true" || value == "yes" || value == "on" || value == "1") return true;
  if (value == "false" || value == "no" || value == "off" || value == "0") return false;
  return fallback;
}

size_t Config::tableCount(const char* table) const {
  size_t count = 0;
  for (const auto& name : tables_) count += table != nullptr && name == table;
  return count;
}

const char* Config::getTableString(const char* table, size_t index, const char* key,
                                   const char* fallback) const {
  const Entry* e = find(table, int(index), key);
  return e ? e->value.c_str() : fallback;
}

}  // namespace paperos

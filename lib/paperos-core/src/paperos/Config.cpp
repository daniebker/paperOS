#include "paperos/Config.h"

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

    if (line[i] == '[') {  // [section]
      ++i;
      skipSpace(line, i);
      std::string name;
      if (!parseBareKey(line, i, name)) {
        recordError(lineNo);
        continue;
      }
      skipSpace(line, i);
      if (i >= line.size() || line[i] != ']' || !atLineEnd(line, i + 1)) {
        recordError(lineNo);
        continue;
      }
      section = name;
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
    set(section, key, value);
  }
  return errorCount_ == 0;
}

void Config::set(const std::string& section, const std::string& key, const std::string& value) {
  for (auto& e : entries_) {
    if (e.section == section && e.key == key) {
      e.value = value;  // last definition wins
      return;
    }
  }
  entries_.push_back(Entry{section, key, value});
}

const Config::Entry* Config::find(const char* section, const char* key) const {
  if (section == nullptr) section = "";
  if (key == nullptr) return nullptr;
  for (const auto& e : entries_) {
    if (e.section == section && e.key == key) return &e;
  }
  return nullptr;
}

const char* Config::getString(const char* section, const char* key, const char* fallback) const {
  const Entry* e = find(section, key);
  return e ? e->value.c_str() : fallback;
}

bool Config::has(const char* section, const char* key) const { return find(section, key) != nullptr; }

}  // namespace paperos

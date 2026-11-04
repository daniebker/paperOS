#include "XhtmlParser.h"

#include <cstdlib>
#include <cstring>

#include "AsciiFold.h"

namespace ereader {

namespace {

constexpr size_t kMaxEntityChars = 12;
constexpr size_t kMaxTagNameChars = 16;  // longer names can't be tags we handle
constexpr uint32_t kReplacementCharacter = 0xFFFD;

bool isOneOf(const std::string& name, const char* const* names, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    if (name == names[i]) return true;
  }
  return false;
}

// Content inside these is never shown.
bool isSkippedTag(const std::string& name) {
  static const char* const kNames[] = {"head", "script", "style", "svg", "math", "template"};
  return isOneOf(name, kNames, sizeof(kNames) / sizeof(kNames[0]));
}

bool isBlockTag(const std::string& name) {
  static const char* const kNames[] = {
      "p",      "div",     "ul",     "ol",    "dl",         "dt",     "dd",    "blockquote",
      "section", "article", "aside", "header", "footer",    "nav",    "figure", "figcaption",
      "table",  "tr",      "pre",    "hr",    "address",    "center", "li",    "body"};
  return isOneOf(name, kNames, sizeof(kNames) / sizeof(kNames[0]));
}

bool isHeadingTag(const std::string& name) {
  return name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6';
}

struct NamedEntity {
  const char* name;
  uint32_t codePoint;
};

const NamedEntity kEntities[] = {
    {"amp", '&'},       {"lt", '<'},        {"gt", '>'},        {"quot", '"'},
    {"apos", '\''},     {"nbsp", 0xA0},     {"shy", 0xAD},      {"copy", 0xA9},
    {"reg", 0xAE},      {"pound", 0xA3},    {"euro", 0x20AC},   {"trade", 0x2122},
    {"ndash", 0x2013},  {"mdash", 0x2014},  {"hellip", 0x2026}, {"lsquo", 0x2018},
    {"rsquo", 0x2019},  {"ldquo", 0x201C},  {"rdquo", 0x201D},  {"laquo", 0xAB},
    {"raquo", 0xBB},    {"bull", 0x2022},   {"middot", 0xB7},   {"eacute", 0xE9},
    {"egrave", 0xE8},   {"agrave", 0xE0},   {"ccedil", 0xE7},   {"uuml", 0xFC},
    {"ouml", 0xF6},     {"auml", 0xE4},     {"szlig", 0xDF},
};

}  // namespace

void XhtmlParser::feed(const char* data, size_t size) {
  for (size_t i = 0; i < size; ++i) {
    const char c = data[i];
    switch (state_) {
      case State::Text: feedText(c); break;
      case State::Entity: feedEntity(c); break;
      case State::Tag: feedTag(c); break;
      case State::Comment:
        if (c == '>' && previous_ == '-' && beforePrevious_ == '-') state_ = State::Text;
        break;
      case State::CData:
        if (c == '>' && previous_ == ']' && beforePrevious_ == ']') state_ = State::Text;
        break;
    }
    beforePrevious_ = previous_;
    previous_ = c;
  }
}

void XhtmlParser::finish() {
  if (state_ != State::Entity) return;
  state_ = State::Text;
  emit('&');
  for (const char text : buffer_) emit(uint8_t(text));
}

void XhtmlParser::emit(uint32_t codePoint) {
  if (skipDepth_ > 0) return;
  if (codePoint == ' ' || codePoint == '\n' || codePoint == '\r' || codePoint == '\t') {
    sink_.onChar(' ');
    return;
  }
  for (const char* text = foldToAscii(codePoint); *text != '\0'; ++text) sink_.onChar(*text);
}

void XhtmlParser::feedText(char c) {
  const uint8_t byte = uint8_t(c);
  if (byte < 0x80) {
    continuationBytes_ = 0;
    if (c == '<') {
      state_ = State::Tag;
      buffer_.clear();
      quote_ = 0;
      nameDone_ = false;
    } else if (c == '&') {
      state_ = State::Entity;
      buffer_.clear();
    } else {
      emit(byte);
    }
    return;
  }

  if ((byte & 0xC0) == 0x80) {
    if (continuationBytes_ == 0) {  // stray continuation byte
      emit(kReplacementCharacter);
      return;
    }
    codePoint_ = (codePoint_ << 6) | (byte & 0x3F);
    if (--continuationBytes_ == 0) emit(codePoint_);
  } else if ((byte & 0xE0) == 0xC0) {
    codePoint_ = byte & 0x1F;
    continuationBytes_ = 1;
  } else if ((byte & 0xF0) == 0xE0) {
    codePoint_ = byte & 0x0F;
    continuationBytes_ = 2;
  } else if ((byte & 0xF8) == 0xF0) {
    codePoint_ = byte & 0x07;
    continuationBytes_ = 3;
  } else {
    continuationBytes_ = 0;
    emit(kReplacementCharacter);
  }
}

void XhtmlParser::feedEntity(char c) {
  if (c == ';') {
    state_ = State::Text;
    resolveEntity();
    return;
  }
  const bool nameChar = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '#';
  if (nameChar && buffer_.size() < kMaxEntityChars) {
    buffer_ += c;
    return;
  }

  // A bare '&': show it as written.
  finish();
  feedText(c);
}

void XhtmlParser::resolveEntity() {
  if (buffer_.size() > 1 && buffer_[0] == '#') {
    const bool hex = buffer_[1] == 'x' || buffer_[1] == 'X';
    const char* digits = buffer_.c_str() + (hex ? 2 : 1);
    emit(uint32_t(strtoul(digits, nullptr, hex ? 16 : 10)));
    return;
  }
  for (const NamedEntity& entity : kEntities) {
    if (buffer_ == entity.name) {
      emit(entity.codePoint);
      return;
    }
  }
  emit(kReplacementCharacter);
}

void XhtmlParser::feedTag(char c) {
  if (quote_ != 0) {
    if (c == quote_) quote_ = 0;
    return;
  }
  if (c == '>') {
    state_ = State::Text;
    handleTag();
    return;
  }
  if (nameDone_) {
    if (c == '"' || c == '\'') quote_ = c;
    return;
  }
  if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
    nameDone_ = true;
    return;
  }

  if (buffer_.size() < kMaxTagNameChars) buffer_ += c;
  if (buffer_ == "!--") {
    state_ = State::Comment;
    previous_ = 0;  // so "<!-->" doesn't count as closed
  } else if (buffer_ == "![CDATA[") {
    state_ = State::CData;
  }
}

void XhtmlParser::handleTag() {
  std::string name = buffer_;
  const bool selfClosing = previous_ == '/' || (!name.empty() && name.back() == '/');
  if (!name.empty() && name.back() == '/') name.pop_back();
  if (name.empty() || name[0] == '!' || name[0] == '?') return;

  const bool closing = name[0] == '/';
  if (closing) name.erase(0, 1);
  for (char& c : name) {
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  }
  const size_t colon = name.find(':');
  if (colon != std::string::npos) name.erase(0, colon + 1);

  if (isSkippedTag(name)) {
    if (selfClosing) return;
    if (closing) {
      if (skipDepth_ > 0) --skipDepth_;
    } else {
      ++skipDepth_;
    }
    return;
  }
  if (skipDepth_ > 0) return;

  if (name == "br") {
    sink_.onLineBreak();
  } else if (isHeadingTag(name)) {
    sink_.onHeading(!closing);
  } else if (name == "td" || name == "th") {
    sink_.onChar(' ');
  } else if (isBlockTag(name)) {
    sink_.onParagraphEnd();
    if (name == "li" && !closing) {
      sink_.onChar('-');
      sink_.onChar(' ');
    }
  }
}

namespace {

class StringSink : public TextSink {
 public:
  void onChar(char c) override {
    if (c == ' ' && (text.empty() || text.back() == ' ')) return;
    text += c;
  }

  std::string text;
};

}  // namespace

std::string xmlTextToAscii(const std::string& xml) {
  StringSink sink;
  XhtmlParser parser(sink);
  parser.feed(xml.data(), xml.size());
  parser.finish();
  if (!sink.text.empty() && sink.text.back() == ' ') sink.text.pop_back();
  return sink.text;
}

}  // namespace ereader

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace ereader {

// Receives the readable content of an XHTML document.
class TextSink {
 public:
  virtual ~TextSink() = default;

  // One printable ASCII character. Whitespace always arrives as ' ', possibly
  // several in a row.
  virtual void onChar(char c) = 0;
  virtual void onLineBreak() {}
  virtual void onParagraphEnd() {}
  virtual void onHeading(bool active) { (void)active; }
};

// Streaming XHTML-to-text converter. Data can be fed in chunks of any size,
// splitting tags and multi-byte characters anywhere, so a chapter never has
// to fit in memory. It is lenient rather than validating: unknown tags are
// ignored, and images and other non-text content are dropped.
class XhtmlParser {
 public:
  explicit XhtmlParser(TextSink& sink) : sink_(sink) {}

  void feed(const char* data, size_t size);
  // Call after the last feed(), to flush text still being assembled.
  void finish();

 private:
  enum class State : uint8_t { Text, Entity, Tag, Comment, CData };

  void feedText(char c);
  void feedEntity(char c);
  void feedTag(char c);
  void emit(uint32_t codePoint);
  void resolveEntity();
  void handleTag();

  TextSink& sink_;
  State state_ = State::Text;

  uint32_t codePoint_ = 0;  // UTF-8 character being assembled
  int continuationBytes_ = 0;

  std::string buffer_;  // entity or tag name being read
  char quote_ = 0;      // inside this attribute quote
  bool nameDone_ = false;
  char previous_ = 0;  // last two characters, to spot "-->", "]]>" and "/>"
  char beforePrevious_ = 0;

  int skipDepth_ = 0;  // inside <head>, <style>, ...
};

// Plain ASCII text of an XML fragment such as "Fish &amp; Chips".
std::string xmlTextToAscii(const std::string& xml);

}  // namespace ereader

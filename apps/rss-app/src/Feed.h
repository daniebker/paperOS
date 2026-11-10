#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Reads RSS 2.0, RSS 1.0 (RDF) and Atom feeds.
namespace rss {

// Seconds since 1970 UTC, or kNoTime.
constexpr int64_t kNoTime = INT64_MIN;

struct Article {
  std::string id;     // guid / id, or the link, or the title: unique within the feed
  std::string title;  // plain ASCII
  std::string link;
  std::string text;   // plain ASCII; paragraphs end in "\n\n", line breaks in "\n"
  int64_t published = kNoTime;
};

// Streaming feed parser: feed() it the file in pieces of any size, then
// finish(). Articles become available from take() as each item ends, so
// their text never has to be held for a whole feed. Lenient: unknown
// elements are skipped, and broken markup reads as far as it makes sense.
class FeedParser {
 public:
  // Keeps the first `maxArticles` items; feeds list the newest first.
  // Each article's text is cut to about `maxTextChars`.
  FeedParser(size_t maxArticles, size_t maxTextChars);

  void feed(const char* data, size_t size);
  void finish();

  // Articles completed since the last call.
  std::vector<Article> take();
  // The feed's own title, plain ASCII.
  const std::string& feedTitle() const { return feedTitle_; }
  // True once an <rss>, <feed> or <rdf:RDF> element was seen.
  bool isFeed() const { return isFeed_; }
  // True once maxArticles have been read; the rest can be skipped.
  bool full() const { return count_ >= maxArticles_; }

 private:
  enum class State : uint8_t { Text, Tag, Comment, CData, Declaration };
  enum class Field : uint8_t {
    None, Title, Link, Id, Content, Summary, Published, Updated, FeedTitle
  };

  void flushText();
  void handleTag();
  void append(const std::string& text);
  void endItem();
  static size_t limitOf(Field field);

  size_t maxArticles_;
  size_t maxTextChars_;
  State state_ = State::Text;
  std::string text_;  // text since the last tag, while capturing
  std::string tag_;   // tag being read, without < >
  char quote_ = 0;
  int bracketDepth_ = 0;  // inside <!DOCTYPE [...]>
  std::string tail_;      // last characters, to spot --> and ]]>

  int depth_ = 0;
  bool isFeed_ = false;
  bool inItem_ = false;
  int itemDepth_ = 0;
  Field field_ = Field::None;
  int fieldDepth_ = 0;
  std::string value_;  // the field being captured
  bool sawItem_ = false;

  // The item being read.
  std::string title_, link_, id_, content_, summary_, published_, updated_;

  std::string feedTitle_;
  std::vector<Article> done_;
  size_t count_ = 0;
};

// Parses an RFC 822 date ("Tue, 10 Nov 2026 09:00:00 +0000", or GMT, EST, ...)
// or an ISO 8601 one ("2026-11-10T09:00:00Z"). Without a zone, UTC. Returns
// kNoTime if it can't be read.
int64_t parseDate(const std::string& text);

// Plain ASCII text of an HTML fragment, cut to about `maxChars` at a word.
std::string htmlToText(const std::string& html, size_t maxChars);

// True if the start of a download looks like a feed rather than, say, an
// HTML page.
bool looksLikeFeed(const std::string& head);

}  // namespace rss

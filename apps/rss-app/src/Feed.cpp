#include "Feed.h"

#include <cstdio>
#include <cstring>

#include <paperos/XhtmlParser.h>

namespace rss {

namespace {

constexpr size_t kMaxTagChars = 4096;
constexpr size_t kMaxTextSegment = 256 * 1024;

std::string lower(std::string text) {
  for (char& c : text) {
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  }
  return text;
}

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

std::string trim(const std::string& text) {
  size_t start = 0;
  size_t end = text.size();
  while (start < end && isSpace(text[start])) ++start;
  while (end > start && isSpace(text[end - 1])) --end;
  return text.substr(start, end - start);
}

void appendUtf8(std::string& out, uint32_t cp) {
  if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp < 0xE000)) cp = 0xFFFD;
  if (cp < 0x80) {
    out += char(cp);
  } else if (cp < 0x800) {
    out += char(0xC0 | cp >> 6);
    out += char(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += char(0xE0 | cp >> 12);
    out += char(0x80 | (cp >> 6 & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  } else {
    out += char(0xF0 | cp >> 18);
    out += char(0x80 | (cp >> 12 & 0x3F));
    out += char(0x80 | (cp >> 6 & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  }
}

// Decodes XML's five entities and character references. Others, such as
// HTML's &nbsp;, are left for the HTML converter.
std::string decodeXml(const std::string& text) {
  if (text.find('&') == std::string::npos) return text;
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] != '&') {
      out += text[i];
      continue;
    }
    const size_t semi = text.find(';', i);
    if (semi == std::string::npos || semi - i > 10) {
      out += '&';
      continue;
    }
    const std::string name = text.substr(i + 1, semi - i - 1);
    if (name == "amp") {
      out += '&';
    } else if (name == "lt") {
      out += '<';
    } else if (name == "gt") {
      out += '>';
    } else if (name == "quot") {
      out += '"';
    } else if (name == "apos") {
      out += '\'';
    } else if (name.size() > 1 && name[0] == '#') {
      const bool hex = name[1] == 'x' || name[1] == 'X';
      char* end = nullptr;
      const unsigned long cp = std::strtoul(name.c_str() + (hex ? 2 : 1), &end, hex ? 16 : 10);
      if (end == nullptr || *end != '\0') {
        out += '&';
        continue;
      }
      appendUtf8(out, uint32_t(cp));
    } else {
      out += '&';
      continue;
    }
    i = semi;
  }
  return out;
}

// The value of attribute `name` in a tag's text, decoded.
std::string attribute(const std::string& tag, const char* name) {
  const size_t n = std::strlen(name);
  size_t i = tag.find_first_of(" \t\r\n");
  while (i != std::string::npos && i < tag.size()) {
    while (i < tag.size() && (isSpace(tag[i]) || tag[i] == '/')) ++i;
    const size_t nameStart = i;
    while (i < tag.size() && tag[i] != '=' && !isSpace(tag[i]) && tag[i] != '/') ++i;
    const std::string key = tag.substr(nameStart, i - nameStart);
    while (i < tag.size() && isSpace(tag[i])) ++i;
    if (i >= tag.size() || tag[i] != '=') {
      if (key.empty()) break;
      continue;
    }
    ++i;
    while (i < tag.size() && isSpace(tag[i])) ++i;
    std::string value;
    if (i < tag.size() && (tag[i] == '"' || tag[i] == '\'')) {
      const char q = tag[i++];
      const size_t end = tag.find(q, i);
      value = tag.substr(i, end == std::string::npos ? std::string::npos : end - i);
      i = end == std::string::npos ? tag.size() : end + 1;
    } else {
      const size_t start = i;
      while (i < tag.size() && !isSpace(tag[i])) ++i;
      value = tag.substr(start, i - start);
    }
    if (key.size() == n && lower(key) == name) return decodeXml(value);
  }
  return {};
}

int64_t daysFromCivil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = unsigned(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + int64_t(doe) - 719468;
}

int64_t toUtc(int year, int month, int day, int hour, int minute, int second, int offset) {
  if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 ||
      second > 60) {
    return kNoTime;
  }
  return daysFromCivil(year, unsigned(month), unsigned(day)) * 86400 + hour * 3600 +
         minute * 60 + second - offset;
}

bool readNumber(const std::string& s, size_t& i, int minDigits, int maxDigits, int& out) {
  int digits = 0;
  out = 0;
  while (i < s.size() && digits < maxDigits && s[i] >= '0' && s[i] <= '9') {
    out = out * 10 + (s[i++] - '0');
    ++digits;
  }
  return digits >= minDigits;
}

// "+0100", "-05:00", "Z", "GMT", "EST", ...: seconds east of UTC. False if
// it isn't a zone.
bool readZone(const std::string& s, size_t i, int& offset) {
  while (i < s.size() && isSpace(s[i])) ++i;
  offset = 0;
  if (i >= s.size()) return true;
  if (s[i] == '+' || s[i] == '-') {
    const int sign = s[i] == '-' ? -1 : 1;
    ++i;
    int hours = 0;
    int minutes = 0;
    if (!readNumber(s, i, 2, 2, hours)) return false;
    if (i < s.size() && s[i] == ':') ++i;
    readNumber(s, i, 2, 2, minutes);
    offset = sign * (hours * 3600 + minutes * 60);
    return true;
  }
  static const struct {
    const char* name;
    int hours;
  } kZones[] = {{"z", 0},   {"gmt", 0},  {"ut", 0},   {"utc", 0},  {"est", -5},
                {"edt", -4}, {"cst", -6}, {"cdt", -5}, {"mst", -7}, {"mdt", -6},
                {"pst", -8}, {"pdt", -7}};
  size_t end = i;
  while (end < s.size() && ((s[end] >= 'a' && s[end] <= 'z') || (s[end] >= 'A' && s[end] <= 'Z'))) {
    ++end;
  }
  const std::string name = lower(s.substr(i, end - i));
  for (const auto& zone : kZones) {
    if (name == zone.name) {
      offset = zone.hours * 3600;
      return true;
    }
  }
  return false;
}

int64_t parseIso(const std::string& s) {
  size_t i = 0;
  int year, month, day, hour = 0, minute = 0, second = 0;
  if (!readNumber(s, i, 4, 4, year) || i >= s.size() || s[i++] != '-' ||
      !readNumber(s, i, 2, 2, month) || i >= s.size() || s[i++] != '-' ||
      !readNumber(s, i, 2, 2, day)) {
    return kNoTime;
  }
  if (i < s.size() && (s[i] == 'T' || s[i] == 't' || s[i] == ' ')) {
    ++i;
    if (!readNumber(s, i, 2, 2, hour) || i >= s.size() || s[i++] != ':' ||
        !readNumber(s, i, 2, 2, minute)) {
      return kNoTime;
    }
    if (i < s.size() && s[i] == ':') {
      ++i;
      if (!readNumber(s, i, 2, 2, second)) return kNoTime;
      if (i < s.size() && s[i] == '.') {
        ++i;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
      }
    }
  }
  int offset = 0;
  if (!readZone(s, i, offset)) return kNoTime;
  return toUtc(year, month, day, hour, minute, second, offset);
}

int64_t parseRfc822(const std::string& s) {
  static const char* const kMonths[] = {"jan", "feb", "mar", "apr", "may", "jun",
                                        "jul", "aug", "sep", "oct", "nov", "dec"};
  size_t i = 0;
  // An optional weekday, "Tue,".
  const size_t comma = s.find(',');
  if (comma != std::string::npos && comma < 12) i = comma + 1;
  while (i < s.size() && isSpace(s[i])) ++i;
  int day;
  if (!readNumber(s, i, 1, 2, day)) return kNoTime;
  while (i < s.size() && (isSpace(s[i]) || s[i] == '-')) ++i;
  if (i + 3 > s.size()) return kNoTime;
  const std::string monthName = lower(s.substr(i, 3));
  int month = 0;
  for (int m = 0; m < 12; ++m) {
    if (monthName == kMonths[m]) month = m + 1;
  }
  if (month == 0) return kNoTime;
  i += 3;
  while (i < s.size() && !isSpace(s[i]) && s[i] != '-') ++i;  // "November"
  while (i < s.size() && (isSpace(s[i]) || s[i] == '-')) ++i;
  int year;
  const size_t yearStart = i;
  if (!readNumber(s, i, 2, 4, year)) return kNoTime;
  if (i - yearStart == 2) year += year < 50 ? 2000 : 1900;
  while (i < s.size() && isSpace(s[i])) ++i;
  int hour = 0, minute = 0, second = 0;
  if (i < s.size() && s[i] >= '0' && s[i] <= '9') {
    if (!readNumber(s, i, 1, 2, hour) || i >= s.size() || s[i++] != ':' ||
        !readNumber(s, i, 2, 2, minute)) {
      return kNoTime;
    }
    if (i < s.size() && s[i] == ':') {
      ++i;
      if (!readNumber(s, i, 2, 2, second)) return kNoTime;
    }
  }
  int offset = 0;
  if (!readZone(s, i, offset)) offset = 0;  // an unknown zone: take it as UTC
  return toUtc(year, month, day, hour, minute, second, offset);
}

// Collects the text from XhtmlParser into paragraphs.
class ParagraphSink : public paperos::TextSink {
 public:
  explicit ParagraphSink(size_t maxChars) : maxChars_(maxChars) {}

  void onChar(char c) override {
    if (full_) return;
    if (c == ' ' && (text.empty() || text.back() == ' ' || text.back() == '\n')) return;
    text += c;
    if (text.size() >= maxChars_ && c == ' ') stop();
  }
  void onLineBreak() override {
    if (full_) return;
    trimSpace();
    if (!text.empty() && !endsWith("\n\n")) text += '\n';
  }
  void onParagraphEnd() override {
    if (full_) return;
    trimSpace();
    if (text.empty() || endsWith("\n\n")) return;
    text += endsWith("\n") ? "\n" : "\n\n";
  }
  void onHeading(bool active) override {
    (void)active;
    onParagraphEnd();
  }

  bool cut() const { return full_; }

  void finish() {
    trimSpace();
    while (!text.empty() && text.back() == '\n') text.pop_back();
    if (full_) text += "...";
  }

  std::string text;

 private:
  bool endsWith(const char* suffix) const {
    const size_t n = std::strlen(suffix);
    return text.size() >= n && text.compare(text.size() - n, n, suffix) == 0;
  }
  void trimSpace() {
    while (!text.empty() && text.back() == ' ') text.pop_back();
  }
  void stop() {
    trimSpace();
    full_ = true;
  }

  size_t maxChars_;
  bool full_ = false;
};

}  // namespace

std::string htmlToText(const std::string& html, size_t maxChars) {
  ParagraphSink sink(maxChars);
  paperos::XhtmlParser parser(sink);
  parser.feed(html.data(), html.size());
  parser.finish();
  sink.finish();
  // Text with no space to stop at: cut it hard.
  if (!sink.cut() && sink.text.size() > maxChars) sink.text = sink.text.substr(0, maxChars) + "...";
  return sink.text;
}

int64_t parseDate(const std::string& text) {
  const std::string s = trim(text);
  if (s.size() >= 10 && s[4] == '-') return parseIso(s);
  return parseRfc822(s);
}

bool looksLikeFeed(const std::string& head) {
  size_t i = head.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
  while (i < head.size() && isSpace(head[i])) ++i;
  const std::string start = lower(head.substr(i, 16));
  return start.compare(0, 5, "<?xml") == 0 || start.compare(0, 4, "<rss") == 0 ||
         start.compare(0, 5, "<feed") == 0 || start.compare(0, 8, "<rdf:rdf") == 0 ||
         start.compare(0, 4, "<!--") == 0;
}

FeedParser::FeedParser(size_t maxArticles, size_t maxTextChars)
    : maxArticles_(maxArticles), maxTextChars_(maxTextChars) {}

size_t FeedParser::limitOf(Field field) {
  switch (field) {
    case Field::Content:
    case Field::Summary:
      return 128 * 1024;
    case Field::Link:
      return 2048;
    case Field::Published:
    case Field::Updated:
      return 64;
    default:
      return 1024;
  }
}

void FeedParser::append(const std::string& text) {
  if (field_ == Field::None) return;
  const size_t limit = limitOf(field_);
  if (value_.size() >= limit) return;
  value_.append(text, 0, limit - value_.size());
}

void FeedParser::feed(const char* data, size_t size) {
  for (size_t k = 0; k < size; ++k) {
    const char c = data[k];
    switch (state_) {
      case State::Text:
        if (c == '<') {
          flushText();
          tag_.clear();
          quote_ = 0;
          state_ = State::Tag;
        } else if (field_ != Field::None && text_.size() < kMaxTextSegment) {
          text_ += c;
        }
        break;

      case State::Tag:
        if (quote_ != 0) {
          if (c == quote_) quote_ = 0;
          if (tag_.size() < kMaxTagChars) tag_ += c;
          break;
        }
        if (c == '>') {
          state_ = State::Text;
          handleTag();
          break;
        }
        if ((c == '"' || c == '\'') && !tag_.empty() && tag_[0] != '!') quote_ = c;
        if (tag_.size() < kMaxTagChars) tag_ += c;
        if (tag_ == "!--") {
          state_ = State::Comment;
          tail_.clear();
        } else if (tag_ == "![CDATA[") {
          state_ = State::CData;
          tail_.clear();
        } else if (tag_.size() == 2 && tag_[0] == '!' && tag_[1] != '-' && tag_[1] != '[') {
          state_ = State::Declaration;  // <!DOCTYPE ...>
          bracketDepth_ = 0;
        }
        break;

      case State::Comment:
        tail_ += c;
        if (tail_.size() > 3) tail_.erase(0, tail_.size() - 3);
        if (tail_ == "-->") state_ = State::Text;
        break;

      case State::CData:
        // Kept as it is: no entities to decode.
        tail_ += c;
        if (tail_.size() > 3) {
          if (field_ != Field::None) append(std::string(1, tail_[0]));
          tail_.erase(0, 1);
        }
        if (tail_ == "]]>") {
          tail_.clear();
          state_ = State::Text;
        }
        break;

      case State::Declaration:
        if (c == '[') {
          ++bracketDepth_;
        } else if (c == ']') {
          --bracketDepth_;
        } else if (c == '>' && bracketDepth_ <= 0) {
          state_ = State::Text;
        }
        break;
    }
  }
}

void FeedParser::flushText() {
  if (text_.empty()) return;
  if (field_ != Field::None) append(decodeXml(text_));
  text_.clear();
}

void FeedParser::finish() {
  flushText();
  if (inItem_) endItem();
}

void FeedParser::handleTag() {
  if (tag_.empty() || tag_[0] == '?' || tag_[0] == '!') return;
  const bool closing = tag_[0] == '/';
  const bool selfClosing = !closing && tag_.back() == '/';
  size_t nameEnd = tag_.find_first_of(" \t\r\n/", closing ? 1 : 0);
  if (closing) nameEnd = tag_.find_first_of(" \t\r\n", 1);
  const std::string qname =
      tag_.substr(closing ? 1 : 0, nameEnd == std::string::npos
                                       ? std::string::npos
                                       : nameEnd - (closing ? 1 : 0));
  const std::string name = lower(qname);

  if (closing) {
    if (field_ != Field::None && depth_ == fieldDepth_) {
      // The field ends.
      std::string value = std::move(value_);
      value_.clear();
      switch (field_) {
        case Field::Title: title_ = value; break;
        case Field::Link: if (link_.empty()) link_ = trim(value); break;
        case Field::Id: id_ = trim(value); break;
        case Field::Content: content_ = value; break;
        case Field::Summary: summary_ = value; break;
        case Field::Published: published_ = value; break;
        case Field::Updated: updated_ = value; break;
        case Field::FeedTitle:
          if (feedTitle_.empty()) feedTitle_ = paperos::xmlTextToAscii(value);
          break;
        case Field::None: break;
      }
      field_ = Field::None;
    } else if (field_ == Field::Content || field_ == Field::Summary) {
      append("</" + name + ">");  // inline XHTML content
    }
    if (inItem_ && depth_ == itemDepth_ && (name == "item" || name == "entry")) endItem();
    if (depth_ > 0) --depth_;
    return;
  }

  if (field_ == Field::Content || field_ == Field::Summary) {
    append("<" + name + (selfClosing ? "/>" : ">"));
    if (!selfClosing) ++depth_;
    return;
  }
  if (field_ != Field::None) {  // markup inside a title, say: just count it
    if (!selfClosing) ++depth_;
    return;
  }

  if (depth_ == 0 && (name == "rss" || name == "feed" || name == "rdf:rdf")) isFeed_ = true;

  const int depth = depth_ + 1;  // of this element
  if (!inItem_ && (name == "item" || name == "entry")) {
    if (!selfClosing) {
      inItem_ = true;
      sawItem_ = true;
      itemDepth_ = depth;
      title_.clear();
      link_.clear();
      id_.clear();
      content_.clear();
      summary_.clear();
      published_.clear();
      updated_.clear();
    }
  } else if (inItem_ && depth == itemDepth_ + 1) {
    Field field = Field::None;
    if (name == "title" || name == "atom:title") {
      field = Field::Title;
    } else if (name == "link" || name == "atom:link") {
      const std::string href = attribute(tag_, "href");
      if (!href.empty()) {
        const std::string rel = attribute(tag_, "rel");
        if ((rel.empty() || rel == "alternate") && link_.empty()) link_ = href;
      } else {
        field = Field::Link;
      }
    } else if (name == "guid" || name == "id" || name == "atom:id") {
      field = Field::Id;
    } else if (name == "content:encoded" || name == "content" || name == "atom:content") {
      field = Field::Content;
    } else if (name == "description" || name == "summary" || name == "atom:summary") {
      field = Field::Summary;
    } else if (name == "pubdate" || name == "published" || name == "dc:date" ||
               name == "atom:published" || name == "issued") {
      field = Field::Published;
    } else if (name == "updated" || name == "atom:updated" || name == "modified") {
      field = Field::Updated;
    }
    if (field != Field::None && !selfClosing) {
      field_ = field;
      fieldDepth_ = depth;
      value_.clear();
    }
  } else if (!inItem_ && !sawItem_ && feedTitle_.empty() && (name == "title") && depth <= 3) {
    // <rss><channel><title> or <feed><title>
    if (!selfClosing) {
      field_ = Field::FeedTitle;
      fieldDepth_ = depth;
      value_.clear();
    }
  }
  if (!selfClosing) ++depth_;
}

void FeedParser::endItem() {
  inItem_ = false;
  if (field_ != Field::None && field_ != Field::FeedTitle) field_ = Field::None;
  if (count_ >= maxArticles_) return;
  Article article;
  article.title = paperos::xmlTextToAscii(title_);
  article.link = link_;
  article.text = htmlToText(content_.empty() ? summary_ : content_, maxTextChars_);
  if (article.text.empty() && !content_.empty()) article.text = htmlToText(summary_, maxTextChars_);
  article.published = parseDate(published_);
  if (article.published == kNoTime) article.published = parseDate(updated_);
  article.id = !id_.empty() ? id_ : !link_.empty() ? link_ : title_ + published_;
  if (article.title.empty() && article.text.empty()) return;
  if (article.title.empty()) {
    // Untitled posts, as on microblogs: the start of the text.
    article.title = article.text.substr(0, article.text.find('\n'));
    if (article.title.size() > 80) article.title = article.title.substr(0, 77) + "...";
  }
  done_.push_back(std::move(article));
  ++count_;
}

std::vector<Article> FeedParser::take() {
  std::vector<Article> out;
  out.swap(done_);
  return out;
}

}  // namespace rss

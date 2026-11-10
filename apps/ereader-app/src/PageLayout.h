#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include <paperos.h>

#include <paperos/XhtmlParser.h>

namespace ereader {

// A laid-out page is plain text with one line per '\n'. A line starting with
// this marker is drawn in the heading font; an empty line is a paragraph gap.
constexpr char kHeadingMarker = '\x01';

// Advance widths of the printable ASCII characters in one font. Looked up per
// character because measuring every word through the display is too slow when
// laying out a whole book.
struct FontMetrics {
  paperos::TextSize size = paperos::TextSize::Small;
  int lineHeight = 0;
  uint16_t widths[128] = {};

  void measure(const paperos::Display& display, paperos::TextSize textSize);
  int width(char c) const { return widths[uint8_t(c) & 0x7F]; }
  int width(const std::string& text) const;
};

struct PageMetrics {
  int width = 0;   // of a line
  int height = 0;  // of the text area
  int paragraphGap = 0;
  FontMetrics body;
  FontMetrics heading;
};

// Word-wraps a stream of text into pages. `onPage` receives each finished page.
class PageLayout : public paperos::TextSink {
 public:
  using PageCallback = std::function<void(const std::string& page)>;

  PageLayout(const PageMetrics& metrics, PageCallback onPage)
      : metrics_(metrics), onPage_(std::move(onPage)) {}

  void onChar(char c) override;
  void onLineBreak() override;
  void onParagraphEnd() override;
  void onHeading(bool active) override;

  // Finishes the current page, so the next chapter starts on a fresh one.
  // Does nothing if the page is empty, so image-only chapters take no pages.
  void chapterBreak();

  uint32_t pageCount() const { return pageCount_; }

 private:
  const FontMetrics& font() const { return heading_ ? metrics_.heading : metrics_.body; }
  void flushWord();
  void commitLine();
  void finishPage();

  const PageMetrics& metrics_;
  PageCallback onPage_;

  std::string word_;
  std::string line_;
  std::string page_;
  int lineWidth_ = 0;
  int usedHeight_ = 0;
  bool pendingGap_ = false;
  bool heading_ = false;
  uint32_t pageCount_ = 0;
};

// Feeds a page in the format above back into `sink`, without its line wrapping,
// so it can be laid out again with other metrics.
void feedLaidOutPage(const std::string& page, paperos::TextSink& sink);

}  // namespace ereader

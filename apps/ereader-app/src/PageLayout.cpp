#include "PageLayout.h"

namespace ereader {

void FontMetrics::measure(const paperos::Display& display, paperos::TextSize textSize) {
  size = textSize;
  lineHeight = display.textHeight(textSize);
  char text[2] = {0, 0};
  for (int c = 0x20; c < 0x7F; ++c) {
    text[0] = char(c);
    widths[c] = uint16_t(display.textWidth(text, textSize));
  }
}

int FontMetrics::width(const std::string& text) const {
  int total = 0;
  for (const char c : text) total += width(c);
  return total;
}

void PageLayout::onChar(char c) {
  if (c == ' ') {
    flushWord();
  } else {
    word_ += c;
  }
}

void PageLayout::onLineBreak() {
  flushWord();
  commitLine();
}

void PageLayout::onParagraphEnd() {
  onLineBreak();
  pendingGap_ = true;
}

void PageLayout::onHeading(bool active) {
  onParagraphEnd();
  heading_ = active;
}

void PageLayout::chapterBreak() {
  onParagraphEnd();
  finishPage();
  pendingGap_ = false;
  heading_ = false;
}

void PageLayout::flushWord() {
  if (word_.empty()) return;
  const FontMetrics& f = font();

  // A word wider than the whole line has to be cut.
  int wordWidth = f.width(word_);
  while (wordWidth > metrics_.width) {
    commitLine();
    size_t fit = 0;
    int fitWidth = 0;
    while (fit < word_.size() && fitWidth + f.width(word_[fit]) <= metrics_.width) {
      fitWidth += f.width(word_[fit++]);
    }
    if (fit == 0) fit = 1;  // narrower than one glyph: still make progress
    line_ = word_.substr(0, fit);
    commitLine();
    word_.erase(0, fit);
    wordWidth = f.width(word_);
  }
  if (word_.empty()) return;

  int spaceWidth = line_.empty() ? 0 : f.width(' ');
  if (lineWidth_ + spaceWidth + wordWidth > metrics_.width) {
    commitLine();
    spaceWidth = 0;
  }
  if (spaceWidth > 0) line_ += ' ';
  line_ += word_;
  lineWidth_ += spaceWidth + wordWidth;
  word_.clear();
}

void PageLayout::commitLine() {
  if (line_.empty()) return;

  const int advance = font().lineHeight;
  int gap = pendingGap_ && !page_.empty() ? metrics_.paragraphGap : 0;
  // A heading must not be the last thing on a page.
  const int keepWithNext = heading_ ? metrics_.body.lineHeight : 0;
  if (usedHeight_ + gap + advance + keepWithNext > metrics_.height) {
    finishPage();
    gap = 0;
  }
  if (gap > 0) {
    page_ += '\n';
    usedHeight_ += gap;
  }
  pendingGap_ = false;

  if (heading_) page_ += kHeadingMarker;
  page_ += line_;
  page_ += '\n';
  usedHeight_ += advance;
  line_.clear();
  lineWidth_ = 0;
}

void PageLayout::finishPage() {
  if (page_.empty()) return;
  onPage_(page_);
  ++pageCount_;
  page_.clear();
  usedHeight_ = 0;
}

}  // namespace ereader

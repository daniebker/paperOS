// SD.h first so M5GFX adds its file-drawing functions for it.
#include <SD.h>

#include "M5Display.h"

#include <cstring>

using paperos::Color;
using paperos::Refresh;
using paperos::TextAlign;
using paperos::TextSize;

namespace {

uint32_t toRgb888(Color color) {
  uint8_t v = 0;
  switch (color) {
    case Color::White: v = 0xFF; break;
    case Color::LightGray: v = 0xC0; break;
    case Color::DarkGray: v = 0x60; break;
    case Color::Black: v = 0x00; break;
  }
  return (uint32_t(v) << 16) | (uint32_t(v) << 8) | v;
}

const lgfx::IFont* fontFor(TextSize size) {
  switch (size) {
    case TextSize::Small: return &fonts::FreeSans12pt7b;
    case TextSize::Medium: return &fonts::FreeSans18pt7b;
    case TextSize::Large: return &fonts::FreeSansBold18pt7b;
    case TextSize::Title: return &fonts::FreeSansBold24pt7b;
    case TextSize::ExtraLarge: return &fonts::FreeSans24pt7b;
  }
  return &fonts::FreeSans12pt7b;
}

}  // namespace

void M5Display::beginFrame(Color background) {
  gfx_.startWrite();
  gfx_.fillScreen(toRgb888(background));
}

void M5Display::endFrame(Refresh refresh) {
  gfx_.setEpdMode(refresh == Refresh::Quality ? epd_mode_t::epd_quality : epd_mode_t::epd_fast);
  gfx_.endWrite();
  gfx_.display();
}

bool M5Display::drawImage(const char* path) {
  const char* dot = std::strrchr(path, '.');
  if (dot == nullptr) return false;
  const int w = gfx_.width();
  const int h = gfx_.height();
  // Scale so the image covers the screen; centring then crops the overflow
  // evenly. If the size can't be read, 0 fits the whole image instead.
  float scale = 0.0f;
  int imageW = 0;
  int imageH = 0;
  std::unique_ptr<paperos::File> file = fs_.open(path, paperos::FileMode::Read);
  if (file && paperos::readImageSize(*file, imageW, imageH)) {
    scale = paperos::coverScale(imageW, imageH, w, h);
  }
  file.reset();
  const auto centre = textdatum_t::middle_center;
  if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0) {
    return gfx_.drawJpgFile(SD, path, 0, 0, w, h, 0, 0, scale, scale, centre);
  }
  if (strcasecmp(dot, ".png") == 0) {
    return gfx_.drawPngFile(SD, path, 0, 0, w, h, 0, 0, scale, scale, centre);
  }
  if (strcasecmp(dot, ".bmp") == 0) {
    return gfx_.drawBmpFile(SD, path, 0, 0, w, h, 0, 0, scale, scale, centre);
  }
  return false;
}

void M5Display::fillRect(int x, int y, int w, int h, Color color) {
  gfx_.fillRect(x, y, w, h, toRgb888(color));
}

void M5Display::drawRect(int x, int y, int w, int h, Color color) {
  gfx_.drawRect(x, y, w, h, toRgb888(color));
}

void M5Display::drawLine(int x0, int y0, int x1, int y1, Color color) {
  gfx_.drawLine(x0, y0, x1, y1, toRgb888(color));
}

void M5Display::drawText(const char* text, int x, int y, TextSize size, TextAlign align,
                         Color color) {
  gfx_.setFont(fontFor(size));
  gfx_.setTextSize(1);
  switch (align) {
    case TextAlign::Left: gfx_.setTextDatum(textdatum_t::top_left); break;
    case TextAlign::Center: gfx_.setTextDatum(textdatum_t::top_center); break;
    case TextAlign::Right: gfx_.setTextDatum(textdatum_t::top_right); break;
  }
  gfx_.setTextColor(toRgb888(color));  // transparent background
  gfx_.drawString(text, x, y);
}

int M5Display::textHeight(TextSize size) const { return gfx_.fontHeight(fontFor(size)); }

int M5Display::textWidth(const char* text, TextSize size) const {
  return gfx_.textWidth(text, fontFor(size));
}

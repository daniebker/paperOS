#pragma once

#include <M5Unified.h>
#include <paperos.h>

// paperos::Display backed by the PaperDIY e-paper panel via M5GFX.
class M5Display : public paperos::Display {
 public:
  explicit M5Display(M5GFX& gfx) : gfx_(gfx) {}

  int width() const override { return gfx_.width(); }
  int height() const override { return gfx_.height(); }

  void beginFrame(paperos::Color background) override;
  void endFrame(paperos::Refresh refresh) override;

  void fillRect(int x, int y, int w, int h, paperos::Color color) override;
  void drawRect(int x, int y, int w, int h, paperos::Color color) override;
  void drawLine(int x0, int y0, int x1, int y1, paperos::Color color) override;
  void drawText(const char* text, int x, int y, paperos::TextSize size, paperos::TextAlign align,
                paperos::Color color) override;
  int textHeight(paperos::TextSize size) const override;
  int textWidth(const char* text, paperos::TextSize size) const override;

 private:
  M5GFX& gfx_;
};

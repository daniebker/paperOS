#include "paperos/Icon.h"

#include <cmath>
#include <cstdlib>

namespace paperos {

namespace {

constexpr double kPi = 3.14159265358979323846;

int roundToInt(double v) { return int(std::lround(v)); }

}  // namespace

void strokeLine(Display& display, int x0, int y0, int x1, int y1, int thickness, Color color) {
  if (thickness < 1) thickness = 1;
  const int half = thickness / 2;
  // Straight lines are a single rectangle.
  if (x0 == x1 || y0 == y1) {
    const int left = x0 < x1 ? x0 : x1;
    const int top = y0 < y1 ? y0 : y1;
    display.fillRect(left - half, top - half, std::abs(x1 - x0) + thickness,
                     std::abs(y1 - y0) + thickness, color);
    return;
  }
  // Otherwise a square stamped at every pixel step.
  const int dx = x1 - x0;
  const int dy = y1 - y0;
  const int steps = std::abs(dx) > std::abs(dy) ? std::abs(dx) : std::abs(dy);
  for (int i = 0; i <= steps; ++i) {
    const int x = x0 + roundToInt(double(dx) * i / steps);
    const int y = y0 + roundToInt(double(dy) * i / steps);
    display.fillRect(x - half, y - half, thickness, thickness, color);
  }
}

void strokeRect(Display& display, int x, int y, int w, int h, int thickness, Color color) {
  if (thickness * 2 >= w || thickness * 2 >= h) {
    display.fillRect(x, y, w, h, color);
    return;
  }
  display.fillRect(x, y, w, thickness, color);
  display.fillRect(x, y + h - thickness, w, thickness, color);
  display.fillRect(x, y + thickness, thickness, h - 2 * thickness, color);
  display.fillRect(x + w - thickness, y + thickness, thickness, h - 2 * thickness, color);
}

void strokeArc(Display& display, int cx, int cy, int r, int startDegrees, int endDegrees,
               int thickness, Color color) {
  if (thickness < 1) thickness = 1;
  const int half = thickness / 2;
  const double start = startDegrees * kPi / 180.0;
  const double sweep = (endDegrees - startDegrees) * kPi / 180.0;
  // About one stamp per pixel along the arc.
  int steps = roundToInt(std::fabs(sweep) * r);
  if (steps < 1) steps = 1;
  for (int i = 0; i <= steps; ++i) {
    const double a = start + sweep * i / steps;
    const int x = cx + roundToInt(r * std::cos(a));
    const int y = cy + roundToInt(r * std::sin(a));
    display.fillRect(x - half, y - half, thickness, thickness, color);
  }
}

int iconStroke(int size) {
  const int t = size / 14;
  return t < 2 ? 2 : t;
}

void drawLetterIcon(Display& display, const char* name, int x, int y, int size, Color color) {
  strokeRect(display, x, y, size, size, iconStroke(size), color);
  if (name == nullptr || name[0] == '\0') return;
  const char letter[2] = {name[0], '\0'};
  const int textY = y + (size - display.textHeight(TextSize::Large)) / 2;
  display.drawText(letter, x + size / 2, textY, TextSize::Large, TextAlign::Center, color);
}

void drawGearIcon(Display& display, int x, int y, int size, Color color) {
  const int t = iconStroke(size);
  const int cx = x + size / 2;
  const int cy = y + size / 2;
  const int ring = size * 27 / 100;
  strokeArc(display, cx, cy, ring, 0, 360, t + 1, color);
  // Eight teeth around the ring.
  const int toothW = size / 7;
  const int outer = size / 2 - toothW / 2;
  for (int i = 0; i < 8; ++i) {
    const double a = i * kPi / 4;
    strokeLine(display, cx + roundToInt(ring * std::cos(a)), cy + roundToInt(ring * std::sin(a)),
               cx + roundToInt(outer * std::cos(a)), cy + roundToInt(outer * std::sin(a)), toothW,
               color);
  }
}

void drawFolderIcon(Display& display, int x, int y, int size, Color color) {
  const int t = iconStroke(size);
  const int gap = size / 6;
  const int cell = (size - gap) / 2;
  for (int row = 0; row < 2; ++row) {
    for (int col = 0; col < 2; ++col) {
      strokeRect(display, x + col * (cell + gap), y + row * (cell + gap), cell, cell, t, color);
    }
  }
}

}  // namespace paperos

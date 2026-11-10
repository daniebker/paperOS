#pragma once

#include <cstdint>

namespace paperos {

// Greyscale levels used by the UI. The panel supports 16 levels; the UI
// sticks to a few so it stays crisp on e-paper.
enum class Color : uint8_t { White, LightGray, DarkGray, Black };

// Large and Title are bold; ExtraLarge is the biggest regular weight.
enum class TextSize : uint8_t { Small, Medium, Large, Title, ExtraLarge };

enum class TextAlign : uint8_t { Left, Center, Right };

// How the panel should be updated when a frame is presented.
//   Fast    – quick partial-style update, may leave slight ghosting (menu moves).
//   Quality – full refresh that clears ghosting (opening/closing apps).
enum class Refresh : uint8_t { Fast, Quality };

// Minimal drawing surface used by the shell and apps. Keeping apps on this
// interface (instead of M5GFX directly) lets the core and apps be built and
// unit-tested on the host. Extend it as apps need more.
class Display {
 public:
  virtual ~Display() = default;

  virtual int width() const = 0;
  virtual int height() const = 0;

  // Starts a new frame filled with `background`.
  virtual void beginFrame(Color background = Color::White) = 0;
  // Pushes the frame to the panel.
  virtual void endFrame(Refresh refresh) = 0;

  virtual void fillRect(int x, int y, int w, int h, Color color) = 0;
  virtual void drawRect(int x, int y, int w, int h, Color color) = 0;
  virtual void drawLine(int x0, int y0, int x1, int y1, Color color) = 0;

  // Draws text whose top edge is at `y`. `x` is the left edge, centre or
  // right edge depending on `align`.
  virtual void drawText(const char* text, int x, int y, TextSize size,
                        TextAlign align = TextAlign::Left,
                        Color color = Color::Black) = 0;
  // Line height in pixels for a given text size.
  virtual int textHeight(TextSize size) const = 0;
  // Width in pixels of `text` at a given text size.
  virtual int textWidth(const char* text, TextSize size) const = 0;

  // Draws an image file (JPEG, PNG or BMP) from storage, scaled to fill the
  // whole screen and centred, cropping what overflows. Returns false if it
  // can't be read or decoded, or the display can't show images.
  virtual bool drawImage(const char* path) {
    (void)path;
    return false;
  }
};

}  // namespace paperos

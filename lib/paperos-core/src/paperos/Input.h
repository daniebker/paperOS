#pragma once

#include <cstdint>

namespace paperos {

// The two physical side buttons.
enum class Button : uint8_t { A, B };

enum class Gesture : uint8_t { None, ShortPress, LongPress };

// High-level navigation events delivered to the shell and apps.
//   A short press -> Previous     A long hold -> Select (open)
//   B short press -> Next         B long hold -> Back   (exit)
enum class InputEvent : uint8_t { None, Previous, Next, Select, Back };

InputEvent toInputEvent(Button button, Gesture gesture);

// Turns a stream of (debounced) pressed/released samples for one button into
// gestures. A long press fires as soon as the hold threshold is reached, so
// the user gets feedback without having to release; the following release is
// then swallowed. A short press fires on release.
class GestureDetector {
 public:
  static constexpr uint32_t kDefaultLongPressMs = 600;

  explicit GestureDetector(uint32_t longPressMs = kDefaultLongPressMs)
      : longPressMs_(longPressMs) {}

  Gesture update(bool pressed, uint32_t nowMs);

 private:
  uint32_t longPressMs_;
  uint32_t pressedAtMs_ = 0;
  bool pressed_ = false;
  bool longFired_ = false;
};

}  // namespace paperos

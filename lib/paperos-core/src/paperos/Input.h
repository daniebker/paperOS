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

// Counts quick presses of a button in a row, from pressed/released samples:
// update() returns 2 once a double press is over, 1 for a single press, and 0
// otherwise. A series ends when the button stays up for kGapMs. A press held
// longer than kMaxPressMs isn't counted, and ends the series.
class PressCounter {
 public:
  static constexpr uint32_t kGapMs = 400;
  static constexpr uint32_t kMaxPressMs = 800;

  int update(bool pressed, uint32_t nowMs);

 private:
  bool pressed_ = false;
  uint32_t changedAtMs_ = 0;
  int count_ = 0;
};

}  // namespace paperos

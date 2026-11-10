#include "paperos/Input.h"

namespace paperos {

InputEvent toInputEvent(Button button, Gesture gesture) {
  switch (gesture) {
    case Gesture::ShortPress:
      return button == Button::A ? InputEvent::Previous : InputEvent::Next;
    case Gesture::LongPress:
      return button == Button::A ? InputEvent::Select : InputEvent::Back;
    case Gesture::None:
      break;
  }
  return InputEvent::None;
}

Gesture GestureDetector::update(bool pressed, uint32_t nowMs) {
  if (pressed && !pressed_) {  // press started
    pressed_ = true;
    longFired_ = false;
    pressedAtMs_ = nowMs;
    return Gesture::None;
  }
  if (pressed && pressed_) {  // still held
    if (!longFired_ && nowMs - pressedAtMs_ >= longPressMs_) {
      longFired_ = true;
      return Gesture::LongPress;
    }
    return Gesture::None;
  }
  if (!pressed && pressed_) {  // released
    pressed_ = false;
    return longFired_ ? Gesture::None : Gesture::ShortPress;
  }
  return Gesture::None;
}

int PressCounter::update(bool pressed, uint32_t nowMs) {
  const uint32_t held = nowMs - changedAtMs_;
  if (pressed != pressed_) {
    if (!pressed) {  // released
      if (held <= kMaxPressMs) {
        ++count_;
      } else {
        count_ = 0;  // a hold, not a press
      }
    }
    pressed_ = pressed;
    changedAtMs_ = nowMs;
    return 0;
  }
  if (!pressed && count_ > 0 && held >= kGapMs) {
    const int presses = count_;
    count_ = 0;
    return presses;
  }
  return 0;
}

}  // namespace paperos

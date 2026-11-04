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

}  // namespace paperos

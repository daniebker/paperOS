// paperOS firmware entry point for the M5Stack PaperDIY.
//
// Wires the hardware (e-paper display, side buttons A/B) to the
// hardware-independent shell in lib/paperos-core. To add an app, create a
// folder under apps/, then include it and register it below.

#include <M5Unified.h>
#include <paperos.h>

#include "M5Display.h"

// Apps
#include <HelloWorldApp.h>

namespace {

// Landscape (960x540). Change if the device is held in portrait.
constexpr uint8_t kDisplayRotation = 1;
constexpr uint32_t kLoopDelayMs = 10;

M5Display* display = nullptr;
paperos::Shell shell;
paperos::GestureDetector buttonA;
paperos::GestureDetector buttonB;

HelloWorldApp helloWorldApp;

void dispatch(paperos::Button button, paperos::Gesture gesture) {
  const paperos::InputEvent event = paperos::toInputEvent(button, gesture);
  if (shell.handle(event)) shell.draw(*display);
}

}  // namespace

void setup() {
  auto cfg = M5.config();
  cfg.serial_baudrate = 115200;
  M5.begin(cfg);
  M5.Display.setRotation(kDisplayRotation);

  static M5Display m5Display(M5.Display);
  display = &m5Display;

  shell.addApp(&helloWorldApp);

  shell.draw(*display);
}

void loop() {
  M5.update();
  const uint32_t now = millis();
  dispatch(paperos::Button::A, buttonA.update(M5.BtnA.isPressed(), now));
  dispatch(paperos::Button::B, buttonB.update(M5.BtnB.isPressed(), now));
  delay(kLoopDelayMs);
}

// paperOS firmware entry point for the M5Stack PaperDIY.
//
// Wires the hardware (e-paper display, side buttons A/B) to the
// hardware-independent shell in lib/paperos-core. To add an app, create a
// folder under apps/, then include it and register it below.

#include <M5Unified.h>
#include <paperos.h>

#include "M5Display.h"
#include "SdConfig.h"

// Apps
#include <HelloWorldApp.h>

namespace {

// Portrait (540x960). Use 2 for portrait turned upside down, or 1 / 3 for
// landscape (960x540).
constexpr uint8_t kDisplayRotation = 0;
constexpr uint32_t kLoopDelayMs = 10;

M5Display* display = nullptr;
paperos::Config config;  // loaded from /config.toml on the SD card in setup()
paperos::Shell shell;
paperos::GestureDetector buttonA;
paperos::GestureDetector buttonB;

HelloWorldApp helloWorldApp(config);

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

  loadConfigFromSd(config);

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

// paperOS firmware entry point for the M5Stack PaperDIY.
//
// Wires the hardware (e-paper display, side buttons A/B) to the
// hardware-independent shell in lib/paperos-core. To add an app, create a
// folder under apps/, then include it and register it below.

#include <M5Unified.h>
#include <paperos.h>

#include "EspClock.h"
#include "EspNetwork.h"
#include "M5Battery.h"
#include "M5Display.h"
#include "SdConfig.h"
#include "SdFileSystem.h"

// Apps
#include <CalendarApp.h>
#include <EReaderApp.h>
#include <HelloWorldApp.h>

namespace {

// Portrait (540x960). Use 2 for portrait turned upside down, or 1 / 3 for
// landscape (960x540).
constexpr uint8_t kDisplayRotation = 0;
constexpr uint32_t kLoopDelayMs = 10;

M5Display* display = nullptr;
paperos::Config config;  // loaded from /config.toml on the SD card in setup()
SdFileSystem sdFileSystem;
M5Battery battery;
EspClock systemClock;
EspNetwork network;
paperos::Shell shell;
paperos::GestureDetector buttonA;
paperos::GestureDetector buttonB;

HelloWorldApp helloWorldApp(config);

// Sets the local time zone from [time] timezone in config.toml.
void applyTimeZone() {
  const char* name = config.getString("time", "timezone", "UTC");
  const char* rule = paperos::posixTimeZone(name);
  if (rule == nullptr) {
    M5_LOGW("Unknown time zone \"%s\", using UTC. Set a POSIX TZ rule instead.", name);
    rule = "UTC0";
  }
  systemClock.begin(rule);
}

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
  applyTimeZone();
  network.begin(config);

  static EReaderApp eReaderApp(sdFileSystem, m5Display, battery);
  static CalendarApp calendarApp(config, sdFileSystem, network, systemClock, m5Display, battery);

  shell.setBattery(&battery);
  shell.addApp(&helloWorldApp);
  shell.addApp(&eReaderApp);
  shell.addApp(&calendarApp);

  shell.draw(*display);
}

void loop() {
  M5.update();
  const uint32_t now = millis();
  dispatch(paperos::Button::A, buttonA.update(M5.BtnA.isPressed(), now));
  dispatch(paperos::Button::B, buttonB.update(M5.BtnB.isPressed(), now));
  if (shell.tick()) shell.draw(*display);
  delay(kLoopDelayMs);
}

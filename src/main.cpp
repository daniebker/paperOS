// paperOS firmware entry point for the M5Stack PaperDIY.
//
// Wires the hardware (e-paper display, side buttons A/B) to the
// hardware-independent shell in lib/paperos-core. To add an app, create a
// folder under apps/, then include it and register it below.

#include <M5Unified.h>
#include <paperos.h>

#include <ctime>
#include <mutex>

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
// [power] sleep_minutes when config.toml doesn't set it.
constexpr long kDefaultSleepMinutes = 10;
// How long to wait for the power to go after asking the power chip to switch
// off. It stays on while USB is plugged in.
constexpr uint32_t kPowerOffWaitMs = 3000;
// M5PM1 registers. The power chip switches off by itself on a double press of
// the power button unless DOUBLE_OFF_DIS is set, which would leave no time to
// draw the sleep screen.
constexpr uint8_t kPm1WakeSource = 0x05;
constexpr uint8_t kPm1ButtonStatus = 0x48;  // bit 0: button down now
constexpr uint8_t kPm1ButtonConfig1 = 0x49;
constexpr uint8_t kPm1ButtonConfig2 = 0x4A;
constexpr uint8_t kPm1DoubleOffDisable = 1 << 0;
// How often the power button is read from the power chip. M5Unified's
// BtnPWR never reports presses on the PaperDIY, so it is read directly.
constexpr uint32_t kButtonPollMs = 20;
// With debug on: how often the log is written to the SD card.
constexpr uint32_t kLogFlushMs = 2000;

M5Display* display = nullptr;
paperos::Config config;  // loaded from /config.toml on the SD card in setup()
SdFileSystem sdFileSystem;
M5Battery battery;
EspClock systemClock;
EspNetwork network;
paperos::Shell shell;
paperos::GestureDetector buttonA;
paperos::GestureDetector buttonB;
paperos::IdleTimer idleTimer;

// [Developer] debug: log messages are also saved to /debug.N.log.
bool debug = false;
paperos::LogFile logFile(sdFileSystem);
std::mutex logMutex;  // log messages can come from other tasks
uint32_t lastLogFlushMs = 0;
uint32_t lastButtonPollMs = 0;
bool powerButtonDown = false;
paperos::PressCounter powerPresses;

HelloWorldApp helloWorldApp(config);

// Receives every M5_LOGx message and queues it for the log file, with the
// time it happened.
void logToFile(esp_log_level_t, bool, const char* text) {
  std::string line;
  const int64_t now = systemClock.now();
  char stamp[48];
  if (now >= 0) {
    const time_t seconds = time_t(now);
    struct tm local;
    localtime_r(&seconds, &local);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S ", &local);
    line = stamp;
  }
  std::snprintf(stamp, sizeof(stamp), "+%lu.%03lus ", (unsigned long)(millis() / 1000),
                (unsigned long)(millis() % 1000));
  line += stamp;
  // M5Unified sends each message's line ending as a call of its own.
  std::string message = text;
  while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) {
    message.pop_back();
  }
  if (message.empty()) return;
  line += message;
  std::lock_guard<std::mutex> lock(logMutex);
  logFile.add(line);
}

void flushLog() {
  if (!debug) return;
  std::lock_guard<std::mutex> lock(logMutex);
  logFile.flush();
}

void startLogging() {
  debug = paperos::debugEnabled(config);
  if (!debug) return;
  logFile.begin();
  M5.Log.setEnableColor(m5::log_target_callback, false);
  M5.Log.setLogLevel(m5::log_target_callback, ESP_LOG_INFO);
  M5.Log.setCallback(logToFile);
  M5_LOGI("paperOS started: reset reason %d, logging to %s", int(esp_reset_reason()),
          logFile.path().c_str());
}

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

// Reads the power button from the power chip every kButtonPollMs and
// returns how many quick presses in a row just ended (see PressCounter).
int pollPowerButton(uint32_t now) {
  if (now - lastButtonPollMs < kButtonPollMs) return 0;
  lastButtonPollMs = now;
  const bool down = M5.Power.M5pm1.readRegister8(kPm1ButtonStatus) & 1;
  if (debug && down != powerButtonDown) {
    M5_LOGI("Power button %s", down ? "down" : "up");
  }
  powerButtonDown = down;
  return powerPresses.update(down, now);
}

// Leaves an image from /wallpapers on the e-paper, which keeps it without
// power, and switches the device off. A single press of the power button
// starts it again from the home screen.
void enterSleep(const char* reason) {
  M5_LOGI("Going to sleep: %s", reason);
  shell.closeApp();  // lets the app save its state
  network.disconnect();

  if (!ensureSdMounted()) M5_LOGW("No SD card, so no wallpaper");
  display->beginFrame(paperos::Color::White);
  const std::string image = paperos::drawSleepScreen(*display, sdFileSystem, esp_random());
  display->endFrame(paperos::Refresh::Quality);
  M5.Display.waitDisplay();
  M5_LOGI("Sleep screen: %s. VIN %u mV, battery %u mV. Switching off",
          image.empty() ? "message, no wallpaper" : image.c_str(),
          M5.Power.M5pm1.getVBUSVoltage(), M5.Power.M5pm1.getBatteryVoltage());
  flushLog();

  Serial.flush();
  const bool off = M5.Power.M5pm1.powerOff();
  delay(kPowerOffWaitMs);

  // Still running: on USB power, or the power chip didn't respond. Idle with
  // the sleep screen up until a button is pressed, then start again as if
  // woken.
  M5_LOGW("Power stayed on (switch-off request %s), waiting for a button to wake",
          off ? "sent" : "failed");
  flushLog();
  for (;;) {
    M5.update();
    if (pollPowerButton(millis()) > 0 || M5.BtnA.wasPressed() || M5.BtnB.wasPressed()) break;
    delay(kButtonPollMs);
  }
  M5_LOGI("Woken by a button, restarting");
  flushLog();
  esp_restart();
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

  static M5Display m5Display(M5.Display, sdFileSystem);
  display = &m5Display;

  loadConfigFromSd(config);
  applyTimeZone();
  startLogging();
  network.begin(config);
  // Handle a double press of the power button here instead, to show the
  // sleep screen before switching off.
  if (!M5.Power.M5pm1.bitOn(kPm1ButtonConfig2, kPm1DoubleOffDisable)) {
    M5_LOGW("Could not take over the power button's double press");
  }
  M5_LOGI("Power chip: wake source 0x%02x, BTN_CFG_1 0x%02x, BTN_CFG_2 0x%02x",
          M5.Power.M5pm1.readRegister8(kPm1WakeSource),
          M5.Power.M5pm1.readRegister8(kPm1ButtonConfig1),
          M5.Power.M5pm1.readRegister8(kPm1ButtonConfig2));
  {
    const int64_t now = systemClock.now();
    char when[32] = "unknown";
    if (now >= 0) {
      const time_t seconds = time_t(now);
      struct tm utc;
      gmtime_r(&seconds, &utc);
      strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S UTC", &utc);
    }
    M5_LOGI("RTC %s, time at boot %s", M5.Rtc.isEnabled() ? "found" : "not found", when);
  }
  idleTimer.setTimeoutMinutes(config.getInt("power", "sleep_minutes", kDefaultSleepMinutes));
  idleTimer.reset(millis());
  M5_LOGI("Sleep after %u min without a button press", unsigned(idleTimer.timeoutMs() / 60000));

  static EReaderApp eReaderApp(sdFileSystem, m5Display, battery);
  static CalendarApp calendarApp(config, sdFileSystem, network, systemClock, m5Display, battery);

  shell.setBattery(&battery);
  shell.addApp(&helloWorldApp);
  shell.addApp(&eReaderApp);
  shell.addApp(&calendarApp);

  shell.draw(*display);
  flushLog();
}

void loop() {
  M5.update();
  const uint32_t now = millis();
  dispatch(paperos::Button::A, buttonA.update(M5.BtnA.isPressed(), now));
  dispatch(paperos::Button::B, buttonB.update(M5.BtnB.isPressed(), now));
  const int presses = pollPowerButton(now);
  if (presses > 0) {
    M5_LOGI("Power button pressed %d time(s)", presses);
    idleTimer.reset(now);
    if (presses == 2) enterSleep("power button pressed twice");
  }
  if (M5.BtnA.isPressed() || M5.BtnB.isPressed()) idleTimer.reset(now);
  if (idleTimer.expired(now)) enterSleep("no button pressed for a while");
  if (shell.tick()) shell.draw(*display);
  if (debug && now - lastLogFlushMs >= kLogFlushMs) {
    lastLogFlushMs = now;
    flushLog();
  }
  delay(kLoopDelayMs);
}

#pragma once

#include <cstdint>
#include <string>

#include "paperos/Display.h"
#include "paperos/FileSystem.h"

namespace paperos {

// Folder on the SD card the sleep screen picks an image from.
constexpr const char* kWallpaperDir = "/wallpapers";

// Counts down to sleep while the buttons are left alone, set from config.toml:
//
//   [power]
//     sleep_minutes = 10   # 0 never sleeps
class IdleTimer {
 public:
  void setTimeoutMinutes(long minutes);
  uint32_t timeoutMs() const { return timeoutMs_; }

  // Call on any button activity.
  void reset(uint32_t nowMs) { lastActivityMs_ = nowMs; }
  // True once the buttons have been left alone for the timeout. Safe across
  // millis() wraparound.
  bool expired(uint32_t nowMs) const {
    return timeoutMs_ > 0 && nowMs - lastActivityMs_ >= timeoutMs_;
  }

 private:
  uint32_t timeoutMs_ = 0;
  uint32_t lastActivityMs_ = 0;
};

// True for the image files the sleep screen can show: .jpg, .jpeg, .png and
// .bmp, in any case. Hidden files such as macOS's "._" copies are skipped.
bool isWallpaper(const std::string& name);

// Draws the screen left up while the device is off: a random image from
// kWallpaperDir filling the screen, or a short message if there is none or none
// can be drawn. `random` picks the image, e.g. esp_random(). The frame must
// already be begun. Returns the path of the image shown, or "" for the
// message.
std::string drawSleepScreen(Display& display, FileSystem& fs, uint32_t random);

}  // namespace paperos

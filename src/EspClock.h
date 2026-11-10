#pragma once

#include <string>

#include <paperos.h>

// paperos::Clock backed by the ESP32 system time. The time is set from the
// internet (SNTP) by sync() and saved to the board's RTC when it has one, which
// M5Unified reads back into the system time at boot.
class EspClock : public paperos::Clock {
 public:
  // Sets the local time zone, a POSIX TZ rule.
  void begin(const char* posixTimeZone);

  int64_t now() const override;
  uint32_t uptimeMs() const override;
  void sync() override;

 private:
  std::string timeZone_ = "UTC0";
};

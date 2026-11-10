#include "EspClock.h"

#include <M5Unified.h>
#include <esp_sntp.h>
#include <time.h>

namespace {

// Times before this mean the clock was never set (it starts at 1970).
constexpr time_t kEarliestValidTime = 1735689600;  // 2025-01-01

void saveToRtc(struct timeval* tv) {
  if (!M5.Rtc.isEnabled()) return;
  const time_t t = tv->tv_sec;
  tm utc;
  gmtime_r(&t, &utc);
  M5.Rtc.setDateTime(&utc);  // M5Unified reads it back as UTC at boot
  M5_LOGI("Clock set from the internet and saved to the RTC");
}

}  // namespace

void EspClock::begin(const char* posixTimeZone) {
  timeZone_ = posixTimeZone;
  setenv("TZ", timeZone_.c_str(), 1);
  tzset();
}

int64_t EspClock::now() const {
  const time_t t = time(nullptr);
  return t >= kEarliestValidTime ? int64_t(t) : -1;
}

uint32_t EspClock::uptimeMs() const { return millis(); }

void EspClock::sync() {
  sntp_set_time_sync_notification_cb(saveToRtc);
  configTzTime(timeZone_.c_str(), "pool.ntp.org", "time.google.com");
}

#pragma once

#include <cstdint>

namespace paperos {

// Wall-clock time, read through this interface so apps can be tested on the
// host, like Display.
//
// Local time is the C library's: the firmware sets the TZ environment variable
// from config.toml at boot, so apps convert with localtime_r() and mktime().
class Clock {
 public:
  virtual ~Clock() = default;

  // Seconds since 1970-01-01 UTC, or negative while the time is unknown, e.g.
  // before the first sync on a device without a battery-backed clock.
  virtual int64_t now() const = 0;

  // Milliseconds since boot, for timeouts. Wraps around after 49 days.
  virtual uint32_t uptimeMs() const = 0;

  // Starts setting the clock from the internet. Needs a network connection;
  // now() becomes valid once it completes.
  virtual void sync() {}
};

}  // namespace paperos

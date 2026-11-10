#pragma once

namespace paperos {

// POSIX TZ rule for a time zone from config.toml, as the C library needs it.
// Accepts common names such as "Europe/Berlin" or "America/New_York", and
// POSIX rules such as "CET-1CEST,M3.5.0,M10.5.0/3" as they are. Returns
// nullptr for a name it doesn't know.
const char* posixTimeZone(const char* name);

}  // namespace paperos

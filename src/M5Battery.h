#pragma once

#include <M5Unified.h>
#include <paperos.h>

// paperos::Battery backed by M5Unified. On the PaperDIY the level is estimated
// from the battery voltage, so it reads high while charging.
class M5Battery : public paperos::Battery {
 public:
  int level() const override { return M5.Power.getBatteryLevel(); }
};

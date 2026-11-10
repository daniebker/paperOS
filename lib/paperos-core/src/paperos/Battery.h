#pragma once

#include "paperos/Display.h"

namespace paperos {

// Battery charge, read through this interface so the shell and apps can be
// tested on the host, like Display.
class Battery {
 public:
  virtual ~Battery() = default;

  // Charge from 0 to 100, or negative if it can't be read.
  virtual int level() const = 0;
};

// Draws the charge as text such as "87%", or nothing if it can't be read.
// Returns the width drawn, 0 if nothing.
int drawBatteryLevel(Display& display, const Battery& battery, int x, int y, TextAlign align,
                      Color color = Color::DarkGray);

}  // namespace paperos

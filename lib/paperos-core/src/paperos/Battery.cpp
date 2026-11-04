#include "paperos/Battery.h"

#include <string>

namespace paperos {

void drawBatteryLevel(Display& display, const Battery& battery, int x, int y, TextAlign align,
                      Color color) {
  const int level = battery.level();
  if (level < 0) return;
  const std::string text = std::to_string(level) + "%";
  display.drawText(text.c_str(), x, y, TextSize::Small, align, color);
}

}  // namespace paperos

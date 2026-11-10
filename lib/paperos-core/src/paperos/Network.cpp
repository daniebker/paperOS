#include "paperos/Network.h"

namespace paperos {

int drawWifiStatus(Display& display, NetworkState state, int right, int y, int height,
                   Color color) {
  if (state != NetworkState::Connected && state != NetworkState::Connecting) return 0;
  constexpr int kBars = 3;
  const int barW = height / 4 > 3 ? height / 4 : 3;
  const int gap = barW / 2 > 2 ? barW / 2 : 2;
  const int width = kBars * barW + (kBars - 1) * gap;
  int x = right - width;
  for (int i = 1; i <= kBars; ++i) {
    const int barH = height * i / kBars;
    const int top = y + height - barH;
    if (state == NetworkState::Connected) {
      display.fillRect(x, top, barW, barH, color);
    } else {
      display.drawRect(x, top, barW, barH, color);
    }
    x += barW + gap;
  }
  return width;
}

}  // namespace paperos

#include "paperos/Sleep.h"

#include <algorithm>
#include <vector>

namespace paperos {

void IdleTimer::setTimeoutMinutes(long minutes) {
  // Up to a day, which keeps the milliseconds well inside 32 bits.
  minutes = std::min<long>(std::max<long>(minutes, 0), 24 * 60);
  timeoutMs_ = uint32_t(minutes) * 60u * 1000u;
}

bool isWallpaper(const std::string& name) {
  if (name.empty() || name[0] == '.') return false;
  const size_t dot = name.rfind('.');
  if (dot == std::string::npos) return false;
  std::string ext = name.substr(dot + 1);
  for (char& c : ext) {
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  }
  return ext == "jpg" || ext == "jpeg" || ext == "png" || ext == "bmp";
}

std::string drawSleepScreen(Display& display, FileSystem& fs, uint32_t random) {
  const int w = display.width();
  const int h = display.height();

  std::vector<DirEntry> entries;
  std::vector<std::string> images;
  if (fs.list(kWallpaperDir, entries)) {
    for (const DirEntry& entry : entries) {
      if (!entry.isDirectory && isWallpaper(entry.name)) images.push_back(entry.name);
    }
  }
  // Same order whatever order the card lists them in, so `random` alone
  // decides the pick.
  std::sort(images.begin(), images.end());

  // Start at a random image; if it can't be drawn, try the others in turn.
  for (size_t i = 0; i < images.size(); ++i) {
    const std::string path =
        std::string(kWallpaperDir) + "/" + images[(random + i) % images.size()];
    display.fillRect(0, 0, w, h, Color::White);
    if (display.drawImage(path.c_str())) return path;
  }

  display.fillRect(0, 0, w, h, Color::White);
  const int titleH = display.textHeight(TextSize::Title);
  const int y = h / 2 - titleH;
  display.drawText("paperOS", w / 2, y, TextSize::Title, TextAlign::Center);
  display.drawText("Press the power button to wake", w / 2, y + titleH + 24, TextSize::Small,
                   TextAlign::Center, Color::DarkGray);
  return "";
}

}  // namespace paperos

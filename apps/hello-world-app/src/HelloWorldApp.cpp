#include "HelloWorldApp.h"

namespace {

const char* const kGreetings[] = {
    "Hello, World!",        // English
    "Hallo, Welt!",         // German
    "Bonjour, le monde !",  // French
    "Hola, mundo!",         // Spanish
    "Ciao, mondo!",         // Italian
};
constexpr size_t kGreetingCount = sizeof(kGreetings) / sizeof(kGreetings[0]);

}  // namespace

using paperos::Color;
using paperos::TextAlign;
using paperos::TextSize;

size_t HelloWorldApp::greetingCount() { return kGreetingCount; }

const char* HelloWorldApp::greeting(size_t index) {
  return index < kGreetingCount ? kGreetings[index] : "";
}

const char* HelloWorldApp::ownerName() const { return config_.getString("owner", "name", ""); }

std::string HelloWorldApp::introduction() const {
  const char* owner = ownerName();
  if (*owner == '\0') return "My name is not set yet";
  return std::string("My name is ") + owner;
}

void HelloWorldApp::draw(paperos::Display& display) {
  const int w = display.width();
  const int h = display.height();

  const int greetingH = display.textHeight(TextSize::Medium);
  const int greetingGap = 10;
  const int titleH = display.textHeight(TextSize::Title);
  const int introGap = 36;

  const int blockH = int(kGreetingCount) * (greetingH + greetingGap) - greetingGap + introGap + titleH;
  int y = (h - blockH) / 2;
  for (size_t i = 0; i < kGreetingCount; ++i) {
    display.drawText(kGreetings[i], w / 2, y, TextSize::Medium, TextAlign::Center);
    y += greetingH + greetingGap;
  }
  y += introGap - greetingGap;
  display.drawText(introduction().c_str(), w / 2, y, TextSize::Title, TextAlign::Center);

  const int smallH = display.textHeight(TextSize::Small);
  const char* footer = *ownerName() == '\0'
                           ? "Add [owner] name = \"...\" to config.toml on the SD card    Hold B: back"
                           : "Hold B: back";
  display.drawText(footer, w / 2, h - 24 - smallH, TextSize::Small, TextAlign::Center,
                   Color::DarkGray);
}

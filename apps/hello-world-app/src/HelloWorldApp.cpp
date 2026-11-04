#include "HelloWorldApp.h"

namespace {

// Each language's greeting and the phrase that introduces the owner's name.
// Text stays ASCII because the display fonts don't include accented letters.
struct Language {
  const char* greeting;
  const char* introduction;  // followed by the owner's name
};

const Language kLanguages[] = {
    {"Hello, World!", "My name is "},             // English
    {"Hallo, Welt!", "Mein Name ist "},           // German
    {"Bonjour, le monde !", "Je m'appelle "},     // French
    {"Hola, mundo!", "Me llamo "},                // Spanish
    {"Ciao, mondo!", "Mi chiamo "},               // Italian
};
constexpr size_t kGreetingCount = sizeof(kLanguages) / sizeof(kLanguages[0]);

}  // namespace

using paperos::Color;
using paperos::InputEvent;
using paperos::TextAlign;
using paperos::TextSize;

size_t HelloWorldApp::greetingCount() { return kGreetingCount; }

const char* HelloWorldApp::greeting(size_t index) {
  return index < kGreetingCount ? kLanguages[index].greeting : "";
}

void HelloWorldApp::onOpen() { greetingIndex_ = 0; }

bool HelloWorldApp::onInput(InputEvent event) {
  switch (event) {
    case InputEvent::Previous:
      greetingIndex_ = (greetingIndex_ + kGreetingCount - 1) % kGreetingCount;
      return true;
    case InputEvent::Next:
      greetingIndex_ = (greetingIndex_ + 1) % kGreetingCount;
      return true;
    default:
      return false;  // Back falls through to the shell, which closes the app.
  }
}

const char* HelloWorldApp::ownerName() const { return config_.getString("owner", "name", ""); }

std::string HelloWorldApp::introduction() const {
  const char* owner = ownerName();
  if (*owner == '\0') return "Name is not set yet";
  return std::string(kLanguages[greetingIndex_].introduction) + owner;
}

void HelloWorldApp::draw(paperos::Display& display) {
  const int w = display.width();
  const int h = display.height();

  const int titleH = display.textHeight(TextSize::Title);
  const int mediumH = display.textHeight(TextSize::Medium);
  const int smallH = display.textHeight(TextSize::Small);
  const int gap = 28;

  // Greeting, owner introduction and language position, centred as a block.
  const int blockH = titleH + gap + mediumH + gap + smallH;
  int y = (h - blockH) / 2;
  display.drawText(greeting(), w / 2, y, TextSize::Title, TextAlign::Center);
  y += titleH + gap;
  display.drawText(introduction().c_str(), w / 2, y, TextSize::Medium, TextAlign::Center);
  y += mediumH + gap;
  const std::string position =
      std::to_string(greetingIndex_ + 1) + " / " + std::to_string(kGreetingCount);
  display.drawText(position.c_str(), w / 2, y, TextSize::Small, TextAlign::Center,
                   Color::DarkGray);

  // Footer, bottom up. Each line fits the 540 px portrait width.
  int footerY = h - 24 - smallH;
  display.drawText("A / B: change language    Hold B: back", w / 2, footerY, TextSize::Small,
                   TextAlign::Center, Color::DarkGray);
  if (*ownerName() == '\0') {
    footerY -= smallH + 8;
    display.drawText("Set [owner] name in config.toml", w / 2, footerY, TextSize::Small,
                     TextAlign::Center, Color::DarkGray);
  }
}

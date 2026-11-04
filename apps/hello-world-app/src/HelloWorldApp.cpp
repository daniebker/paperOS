#include "HelloWorldApp.h"

namespace {

const char* const kGreetings[] = {
    "Hello, World!",
    "Hallo, Welt!",
    "Bonjour, le monde !",
    "Hola, mundo!",
    "Ciao, mondo!",
};
constexpr size_t kGreetingCount = sizeof(kGreetings) / sizeof(kGreetings[0]);

}  // namespace

using paperos::Color;
using paperos::InputEvent;
using paperos::TextAlign;
using paperos::TextSize;

void HelloWorldApp::onOpen() { greetingIndex_ = 0; }

const char* HelloWorldApp::greeting() const { return kGreetings[greetingIndex_]; }

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

void HelloWorldApp::draw(paperos::Display& display) {
  const int w = display.width();
  const int h = display.height();

  const int titleH = display.textHeight(TextSize::Title);
  display.drawText(greeting(), w / 2, (h - titleH) / 2, TextSize::Title, TextAlign::Center);

  const int smallH = display.textHeight(TextSize::Small);
  display.drawText("A / B: change language    Hold B: back", w / 2, h - 24 - smallH,
                   TextSize::Small, TextAlign::Center, Color::DarkGray);
}

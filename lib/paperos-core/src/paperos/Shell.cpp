#include "paperos/Shell.h"

namespace paperos {

bool Shell::addApp(App* app) {
  if (app == nullptr || appCount_ >= kMaxApps) return false;
  apps_[appCount_++] = app;
  return true;
}

bool Shell::handle(InputEvent event) {
  if (event == InputEvent::None) return false;

  if (active_ != nullptr) {
    if (active_->onInput(event)) return true;
    if (event == InputEvent::Back) {
      close();
      return true;
    }
    return false;
  }

  // Home screen.
  if (appCount_ == 0) return false;
  switch (event) {
    case InputEvent::Previous:
      selected_ = (selected_ + appCount_ - 1) % appCount_;
      return true;
    case InputEvent::Next:
      selected_ = (selected_ + 1) % appCount_;
      return true;
    case InputEvent::Select:
      open(apps_[selected_]);
      return true;
    default:
      return false;
  }
}

bool Shell::tick() { return active_ != nullptr && active_->tick(); }

void Shell::open(App* app) {
  active_ = app;
  fullRefreshPending_ = true;
  active_->onOpen();
}

void Shell::close() {
  active_->onClose();
  active_ = nullptr;
  fullRefreshPending_ = true;
}

void Shell::draw(Display& display) {
  const Refresh refresh = nextRefresh();
  display.beginFrame(Color::White);
  if (active_ != nullptr) {
    active_->draw(display);
  } else {
    drawHome(display);
  }
  display.endFrame(refresh);
  fullRefreshPending_ = false;
}

void Shell::drawHome(Display& display) {
  const int w = display.width();
  const int h = display.height();
  const int margin = 24;

  // Header.
  const int titleH = display.textHeight(TextSize::Title);
  display.drawText("paperOS", margin, margin, TextSize::Title);
  const int headerBottom = margin * 2 + titleH;
  display.fillRect(0, headerBottom - 4, w, 4, Color::Black);

  // App list.
  const int rowH = display.textHeight(TextSize::Large) + 24;
  int y = headerBottom + margin;
  if (appCount_ == 0) {
    display.drawText("No apps installed", margin, y, TextSize::Large);
  }
  for (size_t i = 0; i < appCount_; ++i) {
    const bool selected = i == selected_;
    if (selected) display.fillRect(margin / 2, y, w - margin, rowH, Color::Black);
    display.drawText(apps_[i]->name(), margin * 2, y + 12, TextSize::Large, TextAlign::Left,
                     selected ? Color::White : Color::Black);
    y += rowH + 8;
  }

  // Footer with the button legend.
  const int footerH = display.textHeight(TextSize::Small);
  const int footerY = h - margin - footerH;
  display.drawLine(0, footerY - 12, w, footerY - 12, Color::DarkGray);
  display.drawText("A / B: move    Hold A: open    Hold B: back", w / 2, footerY,
                   TextSize::Small, TextAlign::Center, Color::DarkGray);
}

}  // namespace paperos

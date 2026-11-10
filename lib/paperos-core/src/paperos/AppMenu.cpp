#include "paperos/AppMenu.h"

namespace paperos {

void drawAppList(Display& display, const char* title, const Battery* battery, App* const* apps,
                 size_t count, size_t selected) {
  const int w = display.width();
  const int h = display.height();
  const int margin = 24;

  // Header.
  const int titleH = display.textHeight(TextSize::Title);
  display.drawText(title, margin, margin, TextSize::Title);
  if (battery != nullptr) {
    const int batteryY = margin + (titleH - display.textHeight(TextSize::Small)) / 2;
    drawBatteryLevel(display, *battery, w - margin, batteryY, TextAlign::Right);
  }
  const int headerBottom = margin * 2 + titleH;
  display.fillRect(0, headerBottom - 4, w, 4, Color::Black);

  // App list.
  const int rowH = display.textHeight(TextSize::Large) + 24;
  int y = headerBottom + margin;
  if (count == 0) {
    display.drawText("No apps installed", margin, y, TextSize::Large);
  }
  for (size_t i = 0; i < count; ++i) {
    const bool isSelected = i == selected;
    if (isSelected) display.fillRect(margin / 2, y, w - margin, rowH, Color::Black);
    display.drawText(apps[i]->name(), margin * 2, y + 12, TextSize::Large, TextAlign::Left,
                     isSelected ? Color::White : Color::Black);
    y += rowH + 8;
  }

  // Footer with the button legend.
  const int footerH = display.textHeight(TextSize::Small);
  const int footerY = h - margin - footerH;
  display.drawLine(0, footerY - 12, w, footerY - 12, Color::DarkGray);
  display.drawText("A / B: move    Hold A: open    Hold B: back", w / 2, footerY,
                   TextSize::Small, TextAlign::Center, Color::DarkGray);
}

bool AppMenu::addApp(App* app) {
  if (app == nullptr || appCount_ >= kMaxApps) return false;
  apps_[appCount_++] = app;
  return true;
}

void AppMenu::onOpen() {
  active_ = nullptr;
  selected_ = 0;
  fullRefreshPending_ = false;  // the shell already refreshes on opening
}

void AppMenu::onClose() {
  if (active_ != nullptr) closeActive();
}

void AppMenu::closeActive() {
  active_->onClose();
  active_ = nullptr;
  fullRefreshPending_ = true;
}

bool AppMenu::onInput(InputEvent event) {
  if (active_ != nullptr) {
    if (active_->onInput(event)) return true;
    if (event == InputEvent::Back) {
      closeActive();
      return true;
    }
    return false;
  }
  if (appCount_ == 0) return false;
  switch (event) {
    case InputEvent::Previous:
      selected_ = (selected_ + appCount_ - 1) % appCount_;
      return true;
    case InputEvent::Next:
      selected_ = (selected_ + 1) % appCount_;
      return true;
    case InputEvent::Select:
      active_ = apps_[selected_];
      fullRefreshPending_ = true;
      active_->onOpen();
      return true;
    default:
      return false;  // Back goes to the shell, which closes the menu.
  }
}

bool AppMenu::tick() { return active_ != nullptr && active_->tick(); }

bool AppMenu::wantsFullRefresh() const {
  return fullRefreshPending_ || (active_ != nullptr && active_->wantsFullRefresh());
}

void AppMenu::draw(Display& display) {
  fullRefreshPending_ = false;
  if (active_ != nullptr) {
    active_->draw(display);
  } else {
    drawAppList(display, name_, battery_, apps_, appCount_, selected_);
  }
}

}  // namespace paperos

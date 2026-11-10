#include "paperos/Shell.h"

#include "paperos/AppMenu.h"

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
  drawAppList(display, "paperOS", battery_, apps_, appCount_, selected_);
}

}  // namespace paperos

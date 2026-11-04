#pragma once

#include <cstddef>

#include "paperos/App.h"
#include "paperos/Battery.h"

namespace paperos {

// The home screen and app switcher. Owns no apps; they are registered by
// pointer and must outlive the shell (static instances in main.cpp).
class Shell {
 public:
  static constexpr size_t kMaxApps = 16;

  // Returns false if the app table is full.
  bool addApp(App* app);

  // Shows the charge on the home screen. Must outlive the shell.
  void setBattery(const Battery* battery) { battery_ = battery; }

  // Feeds one input event. Returns true if the screen needs redrawing.
  bool handle(InputEvent event);

  // Gives the open app time to work. Returns true if the screen needs redrawing.
  bool tick();

  // Renders the home screen or the open app.
  void draw(Display& display);

  // Refresh mode for the next frame: a quality refresh after switching
  // between home and an app to clear e-paper ghosting, or when the open app
  // asks for one; fast otherwise.
  Refresh nextRefresh() const {
    const bool appWantsFull = active_ != nullptr && active_->wantsFullRefresh();
    return fullRefreshPending_ || appWantsFull ? Refresh::Quality : Refresh::Fast;
  }

  size_t appCount() const { return appCount_; }
  size_t selectedIndex() const { return selected_; }
  App* activeApp() const { return active_; }

 private:
  void drawHome(Display& display);
  void open(App* app);
  void close();

  App* apps_[kMaxApps] = {};
  size_t appCount_ = 0;
  size_t selected_ = 0;
  App* active_ = nullptr;
  const Battery* battery_ = nullptr;
  bool fullRefreshPending_ = true;
};

}  // namespace paperos

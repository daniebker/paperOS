#pragma once

#include <cstddef>

#include "paperos/App.h"
#include "paperos/Battery.h"

namespace paperos {

// Draws a list screen like the home screen: `title` in the header with the
// battery level, then the names of `apps` with `selected` highlighted, and
// the button legend in the footer. `battery` may be null.
void drawAppList(Display& display, const char* title, const Battery* battery, App* const* apps,
                 size_t count, size_t selected);

// An app that holds other apps, shown on the home screen as one item, such as
// "Settings". Opening it lists its apps like the home screen does; hold A
// opens one, and hold B goes back to the list, then home. Like the shell, it
// owns no apps; they must outlive it.
class AppMenu : public App {
 public:
  static constexpr size_t kMaxApps = 16;

  // `name` must outlive the menu, e.g. a string literal.
  explicit AppMenu(const char* name, const Battery* battery = nullptr)
      : name_(name), battery_(battery) {}

  // Returns false if the menu is full.
  bool addApp(App* app);

  const char* name() const override { return name_; }

  // Sets the icon shown on the home screen, such as drawGearIcon. Without
  // one, the menu shows drawFolderIcon.
  void setIcon(IconFn icon) { icon_ = icon; }
  void drawIcon(Display& display, int x, int y, int size, Color color) const override {
    (icon_ != nullptr ? icon_ : drawFolderIcon)(display, x, y, size, color);
  }

  void onOpen() override;
  void onClose() override;
  bool onInput(InputEvent event) override;
  bool tick() override;
  bool wantsFullRefresh() const override;
  void draw(Display& display) override;

  size_t appCount() const { return appCount_; }
  size_t selectedIndex() const { return selected_; }
  App* activeApp() const { return active_; }

 private:
  void closeActive();

  const char* name_;
  const Battery* battery_;
  IconFn icon_ = nullptr;
  App* apps_[kMaxApps] = {};
  size_t appCount_ = 0;
  size_t selected_ = 0;
  App* active_ = nullptr;
  bool fullRefreshPending_ = false;
};

}  // namespace paperos

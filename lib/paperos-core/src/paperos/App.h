#pragma once

#include "paperos/Display.h"
#include "paperos/Input.h"

namespace paperos {

// Base class for every paperOS app. Apps live in their own folder under
// apps/ and are registered with the Shell in src/main.cpp.
//
// Lifecycle: the shell calls onOpen() when the user long-holds A on the app
// in the home screen, then draw(). Input events are forwarded to onInput();
// return true when the event changed what is on screen so the shell redraws.
//
// Back (long hold B) is offered to the app first. Return false (the default)
// to let the shell close the app and return home; return true to handle it
// yourself, e.g. to leave a sub-view.
//
// Long-running work belongs in tick(), which the main loop calls while the
// app is open. Do a small slice per call so buttons stay responsive.
class App {
 public:
  virtual ~App() = default;

  // Name shown on the home screen.
  virtual const char* name() const = 0;

  virtual void onOpen() {}
  virtual void onClose() {}

  virtual bool onInput(InputEvent event) {
    (void)event;
    return false;
  }

  // Called on every pass of the main loop while the app is open. Return true
  // when the screen needs redrawing.
  virtual bool tick() { return false; }

  // Return true to make the next frame a quality refresh, e.g. to clear the
  // ghosting that builds up over many fast updates.
  virtual bool wantsFullRefresh() const { return false; }

  virtual void draw(Display& display) = 0;
};

}  // namespace paperos

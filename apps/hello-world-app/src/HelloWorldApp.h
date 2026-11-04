#pragma once

#include <cstddef>

#include <paperos.h>

// Sample app and template for new paperOS apps.
// Shows "Hello, World!"; A / B cycle the greeting through a few languages to
// show how an app receives input. Hold B returns to the home screen (handled
// by the shell because onInput() does not consume Back).
class HelloWorldApp : public paperos::App {
 public:
  const char* name() const override { return "Hello World"; }

  void onOpen() override;
  bool onInput(paperos::InputEvent event) override;
  void draw(paperos::Display& display) override;

  const char* greeting() const;

 private:
  size_t greetingIndex_ = 0;
};

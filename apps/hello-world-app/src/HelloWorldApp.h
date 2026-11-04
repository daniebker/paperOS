#pragma once

#include <cstddef>
#include <string>

#include <paperos.h>

// Sample app and template for new paperOS apps.
// Prints "Hello, World!" followed by "My name is <name>", with the name read
// from the [owner] section of config.toml on the SD card:
//
//   [owner]
//     name = "Dan"
//
// A / B cycle the greeting through several languages, showing how an app
// receives input. Hold B returns to the home screen (handled by the shell, because onInput()
// does not consume Back).
class HelloWorldApp : public paperos::App {
 public:
  explicit HelloWorldApp(const paperos::Config& config) : config_(config) {}

  const char* name() const override { return "Hello World"; }
  void onOpen() override;
  bool onInput(paperos::InputEvent event) override;
  void draw(paperos::Display& display) override;

  // Greeting in the currently selected language.
  const char* greeting() const { return greeting(greetingIndex_); }

  static size_t greetingCount();
  static const char* greeting(size_t index);

  // Owner's name from the config, or "" if it is not set.
  const char* ownerName() const;
  // "My name is <name>", or a hint when no name is configured.
  std::string introduction() const;

 private:
  const paperos::Config& config_;
  size_t greetingIndex_ = 0;
};

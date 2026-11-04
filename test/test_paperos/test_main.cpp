// Host-side tests for the paperOS core and apps. Run with: pio test -e native
#include <unity.h>

#include <paperos.h>
#include <HelloWorldApp.h>

#include <cstring>
#include <string>
#include <vector>

using namespace paperos;

namespace {

// Records drawn text so tests can assert on what is on screen.
class FakeDisplay : public Display {
 public:
  int width() const override { return 540; }  // portrait, like the device
  int height() const override { return 960; }
  void beginFrame(Color) override {
    texts.clear();
    ++frames;
  }
  void endFrame(Refresh refresh) override { lastRefresh = refresh; }
  void fillRect(int, int, int, int, Color) override {}
  void drawRect(int, int, int, int, Color) override {}
  void drawLine(int, int, int, int, Color) override {}
  void drawText(const char* text, int, int, TextSize, TextAlign, Color) override {
    texts.emplace_back(text);
  }
  int textHeight(TextSize) const override { return 30; }
  int textWidth(const char* text, TextSize) const override { return 10 * int(strlen(text)); }

  bool shows(const char* text) const {
    for (const auto& t : texts)
      if (t == text) return true;
    return false;
  }

  std::vector<std::string> texts;
  int frames = 0;
  Refresh lastRefresh = Refresh::Fast;
};

class CountingApp : public App {
 public:
  explicit CountingApp(const char* n) : name_(n) {}
  const char* name() const override { return name_; }
  void onOpen() override { ++opened; }
  void onClose() override { ++closed; }
  void draw(Display& d) override { d.drawText(name_, 0, 0, TextSize::Title); }
  const char* name_;
  int opened = 0;
  int closed = 0;
};

class WorkingApp : public App {
 public:
  const char* name() const override { return "Working"; }
  bool tick() override {
    ++ticks;
    return redraw;
  }
  bool wantsFullRefresh() const override { return full; }
  void draw(Display&) override {}
  int ticks = 0;
  bool redraw = false;
  bool full = false;
};

#define ASSERT_ENUM(expected, actual) TEST_ASSERT_EQUAL_INT((int)(expected), (int)(actual))

}  // namespace

void setUp() {}
void tearDown() {}

// --- GestureDetector -------------------------------------------------------

void test_short_press_fires_on_release() {
  GestureDetector g(600);
  ASSERT_ENUM(Gesture::None, g.update(true, 1000));
  ASSERT_ENUM(Gesture::None, g.update(true, 1200));
  ASSERT_ENUM(Gesture::ShortPress, g.update(false, 1300));
  ASSERT_ENUM(Gesture::None, g.update(false, 1400));
}

void test_long_press_fires_once_while_held_and_release_is_swallowed() {
  GestureDetector g(600);
  g.update(true, 1000);
  ASSERT_ENUM(Gesture::None, g.update(true, 1599));
  ASSERT_ENUM(Gesture::LongPress, g.update(true, 1600));
  ASSERT_ENUM(Gesture::None, g.update(true, 3000));
  ASSERT_ENUM(Gesture::None, g.update(false, 3100));
}

void test_long_press_handles_millis_wraparound() {
  GestureDetector g(600);
  g.update(true, 0xFFFFFF00u);
  ASSERT_ENUM(Gesture::LongPress, g.update(true, 0x00000200u));
}

void test_button_mapping() {
  ASSERT_ENUM(InputEvent::Previous, toInputEvent(Button::A, Gesture::ShortPress));
  ASSERT_ENUM(InputEvent::Next, toInputEvent(Button::B, Gesture::ShortPress));
  ASSERT_ENUM(InputEvent::Select, toInputEvent(Button::A, Gesture::LongPress));
  ASSERT_ENUM(InputEvent::Back, toInputEvent(Button::B, Gesture::LongPress));
  ASSERT_ENUM(InputEvent::None, toInputEvent(Button::A, Gesture::None));
}

// --- Shell -----------------------------------------------------------------

void test_home_selection_wraps() {
  Shell shell;
  CountingApp a("One"), b("Two"), c("Three");
  shell.addApp(&a);
  shell.addApp(&b);
  shell.addApp(&c);
  TEST_ASSERT_EQUAL_INT(0, shell.selectedIndex());
  TEST_ASSERT_TRUE(shell.handle(InputEvent::Next));
  TEST_ASSERT_EQUAL_INT(1, shell.selectedIndex());
  shell.handle(InputEvent::Previous);
  shell.handle(InputEvent::Previous);
  TEST_ASSERT_EQUAL_INT(2, shell.selectedIndex());
  shell.handle(InputEvent::Next);
  TEST_ASSERT_EQUAL_INT(0, shell.selectedIndex());
}

void test_select_opens_and_back_closes() {
  Shell shell;
  FakeDisplay display;
  CountingApp a("One"), b("Two");
  shell.addApp(&a);
  shell.addApp(&b);

  shell.draw(display);
  TEST_ASSERT_TRUE(display.shows("paperOS"));
  TEST_ASSERT_FALSE(shell.handle(InputEvent::Back));  // nothing to go back to at home

  shell.handle(InputEvent::Next);
  TEST_ASSERT_TRUE(shell.handle(InputEvent::Select));
  TEST_ASSERT_TRUE(shell.activeApp() == &b);
  TEST_ASSERT_EQUAL_INT(1, b.opened);
  ASSERT_ENUM(Refresh::Quality, shell.nextRefresh());
  shell.draw(display);
  TEST_ASSERT_TRUE(display.shows("Two"));
  TEST_ASSERT_FALSE(display.shows("paperOS"));

  TEST_ASSERT_TRUE(shell.handle(InputEvent::Back));
  TEST_ASSERT_TRUE(shell.activeApp() == nullptr);
  TEST_ASSERT_EQUAL_INT(1, b.closed);
  shell.draw(display);
  TEST_ASSERT_TRUE(display.shows("paperOS"));
  TEST_ASSERT_EQUAL_INT(1, shell.selectedIndex());  // selection kept on return
}

void test_menu_moves_use_fast_refresh() {
  Shell shell;
  FakeDisplay display;
  CountingApp a("One"), b("Two");
  shell.addApp(&a);
  shell.addApp(&b);
  shell.draw(display);
  ASSERT_ENUM(Refresh::Quality, display.lastRefresh);  // first frame
  shell.handle(InputEvent::Next);
  shell.draw(display);
  ASSERT_ENUM(Refresh::Fast, display.lastRefresh);
}

void test_empty_shell_ignores_input() {
  Shell shell;
  FakeDisplay display;
  TEST_ASSERT_FALSE(shell.handle(InputEvent::Select));
  TEST_ASSERT_TRUE(shell.activeApp() == nullptr);
  shell.draw(display);
  TEST_ASSERT_TRUE(display.shows("No apps installed"));
}

// --- Config ----------------------------------------------------------------

void test_config_reads_owner_name() {
  Config c;
  TEST_ASSERT_TRUE(c.parse("[owner]\n  name = \"Dan\"\n"));
  TEST_ASSERT_TRUE(c.has("owner", "name"));
  TEST_ASSERT_EQUAL_STRING("Dan", c.getString("owner", "name"));
}

void test_config_sections_comments_and_value_kinds() {
  Config c;
  const char* text =
      "\xEF\xBB\xBF# paperOS settings\r\n"
      "version = 1\r\n"
      "\r\n"
      "[owner]   # who owns this device\r\n"
      "  name = \"Dan \\\"The Man\\\" # not a comment\"  # trailing comment\r\n"
      "  city = 'C:\\paper'\r\n"
      "[ display ]\r\n"
      "  dark = true\r\n";
  TEST_ASSERT_TRUE(c.parse(text));
  TEST_ASSERT_EQUAL_INT(0, c.errorCount());
  TEST_ASSERT_EQUAL_STRING("1", c.getString("", "version"));
  TEST_ASSERT_EQUAL_STRING("Dan \"The Man\" # not a comment", c.getString("owner", "name"));
  TEST_ASSERT_EQUAL_STRING("C:\\paper", c.getString("owner", "city"));
  TEST_ASSERT_EQUAL_STRING("true", c.getString("display", "dark"));
  TEST_ASSERT_FALSE(c.has("display", "name"));  // keys are scoped to their section
}

void test_config_missing_values_use_fallback() {
  Config c;
  TEST_ASSERT_TRUE(c.parse(""));
  TEST_ASSERT_EQUAL_STRING("nobody", c.getString("owner", "name", "nobody"));
  TEST_ASSERT_EQUAL_STRING("", c.getString("owner", "name"));
  TEST_ASSERT_EQUAL_STRING("x", c.getString(nullptr, nullptr, "x"));
}

void test_config_skips_bad_lines_and_keeps_good_ones() {
  Config c;
  TEST_ASSERT_FALSE(c.parse("[owner\nname = \"unterminated\ngarbage\n[owner]\nname = \"Dan\"\n"));
  TEST_ASSERT_EQUAL_INT(3, c.errorCount());
  TEST_ASSERT_EQUAL_INT(1, c.firstErrorLine());
  TEST_ASSERT_EQUAL_STRING("Dan", c.getString("owner", "name"));
}

void test_config_last_definition_wins_and_parse_resets() {
  Config c;
  c.parse("[owner]\nname = \"A\"\nname = \"B\"\n");
  TEST_ASSERT_EQUAL_STRING("B", c.getString("owner", "name"));
  TEST_ASSERT_EQUAL_INT(1, c.size());
  c.parse("");
  TEST_ASSERT_FALSE(c.has("owner", "name"));
}

// --- HelloWorldApp ---------------------------------------------------------

void test_hello_world_app_flow() {
  Config config;
  config.parse("[owner]\n  name = \"Dan\"\n");
  Shell shell;
  FakeDisplay display;
  HelloWorldApp hello(config);
  shell.addApp(&hello);
  shell.draw(display);
  TEST_ASSERT_TRUE(display.shows("Hello World"));  // listed on the home screen

  shell.handle(toInputEvent(Button::A, Gesture::LongPress));  // hold A: open
  TEST_ASSERT_TRUE(shell.activeApp() == &hello);
  shell.draw(display);
  TEST_ASSERT_TRUE(display.shows("Hello, World!"));
  TEST_ASSERT_TRUE(display.shows("My name is Dan"));
  TEST_ASSERT_TRUE(display.shows("1 / 5"));

  // B cycles forward through every language and wraps around.
  TEST_ASSERT_TRUE(shell.handle(toInputEvent(Button::B, Gesture::ShortPress)));
  shell.draw(display);
  TEST_ASSERT_TRUE(display.shows("Hallo, Welt!"));
  TEST_ASSERT_TRUE(display.shows("Mein Name ist Dan"));  // introduction follows the language
  TEST_ASSERT_FALSE(display.shows("My name is Dan"));
  TEST_ASSERT_TRUE(display.shows("2 / 5"));
  const char* const expectedIntros[] = {"My name is Dan", "Mein Name ist Dan",
                                        "Je m'appelle Dan", "Me llamo Dan", "Mi chiamo Dan"};
  TEST_ASSERT_EQUAL_INT(5, HelloWorldApp::greetingCount());
  for (size_t i = 2; i < 5; ++i) {
    shell.handle(toInputEvent(Button::B, Gesture::ShortPress));
    TEST_ASSERT_EQUAL_STRING(expectedIntros[i], hello.introduction().c_str());
  }
  shell.handle(toInputEvent(Button::B, Gesture::ShortPress));  // wraps back to English
  TEST_ASSERT_EQUAL_STRING(expectedIntros[0], hello.introduction().c_str());
  shell.handle(toInputEvent(Button::B, Gesture::ShortPress));  // German again
  for (size_t i = 0; i < HelloWorldApp::greetingCount(); ++i) {  // full lap
    shell.handle(toInputEvent(Button::B, Gesture::ShortPress));
  }
  TEST_ASSERT_EQUAL_STRING("Hallo, Welt!", hello.greeting());

  // A cycles backwards, wrapping from the first to the last language (index 1 -> 0 -> last).
  shell.handle(toInputEvent(Button::A, Gesture::ShortPress));
  shell.handle(toInputEvent(Button::A, Gesture::ShortPress));
  TEST_ASSERT_EQUAL_STRING(HelloWorldApp::greeting(HelloWorldApp::greetingCount() - 1),
                           hello.greeting());

  shell.handle(toInputEvent(Button::B, Gesture::LongPress));  // hold B: exit
  TEST_ASSERT_TRUE(shell.activeApp() == nullptr);

  shell.handle(toInputEvent(Button::A, Gesture::LongPress));  // reopen resets greeting
  TEST_ASSERT_EQUAL_STRING("Hello, World!", hello.greeting());
}

void test_hello_world_app_without_owner_name() {
  Config config;  // e.g. no SD card or no config.toml
  HelloWorldApp hello(config);
  FakeDisplay display;
  display.beginFrame(Color::White);
  hello.draw(display);
  TEST_ASSERT_TRUE(display.shows("Hello, World!"));
  TEST_ASSERT_TRUE(display.shows("Name is not set yet"));
  TEST_ASSERT_TRUE(display.shows("Set [owner] name in config.toml"));
  hello.onInput(InputEvent::Next);  // same message in every language
  TEST_ASSERT_EQUAL_STRING("Name is not set yet", hello.introduction().c_str());
}

void test_shell_ticks_only_the_open_app() {
  Shell shell;
  WorkingApp app;
  shell.addApp(&app);
  TEST_ASSERT_FALSE(shell.tick());
  TEST_ASSERT_EQUAL(0, app.ticks);

  shell.handle(InputEvent::Select);
  TEST_ASSERT_FALSE(shell.tick());
  app.redraw = true;
  TEST_ASSERT_TRUE(shell.tick());
  TEST_ASSERT_EQUAL(2, app.ticks);

  shell.handle(InputEvent::Back);
  TEST_ASSERT_FALSE(shell.tick());
  TEST_ASSERT_EQUAL(2, app.ticks);
}

void test_app_can_request_a_quality_refresh() {
  Shell shell;
  WorkingApp app;
  FakeDisplay display;
  shell.addApp(&app);
  shell.handle(InputEvent::Select);
  shell.draw(display);
  ASSERT_ENUM(Refresh::Fast, shell.nextRefresh());
  app.full = true;
  ASSERT_ENUM(Refresh::Quality, shell.nextRefresh());
  shell.draw(display);
  ASSERT_ENUM(Refresh::Quality, display.lastRefresh);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_short_press_fires_on_release);
  RUN_TEST(test_long_press_fires_once_while_held_and_release_is_swallowed);
  RUN_TEST(test_long_press_handles_millis_wraparound);
  RUN_TEST(test_button_mapping);
  RUN_TEST(test_home_selection_wraps);
  RUN_TEST(test_select_opens_and_back_closes);
  RUN_TEST(test_menu_moves_use_fast_refresh);
  RUN_TEST(test_empty_shell_ignores_input);
  RUN_TEST(test_config_reads_owner_name);
  RUN_TEST(test_config_sections_comments_and_value_kinds);
  RUN_TEST(test_config_missing_values_use_fallback);
  RUN_TEST(test_config_skips_bad_lines_and_keeps_good_ones);
  RUN_TEST(test_config_last_definition_wins_and_parse_resets);
  RUN_TEST(test_hello_world_app_flow);
  RUN_TEST(test_hello_world_app_without_owner_name);
  RUN_TEST(test_shell_ticks_only_the_open_app);
  RUN_TEST(test_app_can_request_a_quality_refresh);
  return UNITY_END();
}

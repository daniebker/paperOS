// Host-side tests for the paperOS core and apps. Run with: pio test -e native
#include <unity.h>

#include <paperos.h>
#include <HelloWorldApp.h>

#include <string>
#include <vector>

using namespace paperos;

namespace {

// Records drawn text so tests can assert on what is on screen.
class FakeDisplay : public Display {
 public:
  int width() const override { return 960; }
  int height() const override { return 540; }
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

// --- HelloWorldApp ---------------------------------------------------------

void test_hello_world_app_flow() {
  Shell shell;
  FakeDisplay display;
  HelloWorldApp hello;
  shell.addApp(&hello);
  shell.draw(display);
  TEST_ASSERT_TRUE(display.shows("Hello World"));  // listed on the home screen

  shell.handle(toInputEvent(Button::A, Gesture::LongPress));  // hold A: open
  TEST_ASSERT_TRUE(shell.activeApp() == &hello);
  shell.draw(display);
  TEST_ASSERT_TRUE(display.shows("Hello, World!"));

  TEST_ASSERT_TRUE(shell.handle(toInputEvent(Button::B, Gesture::ShortPress)));
  shell.draw(display);
  TEST_ASSERT_TRUE(display.shows("Hallo, Welt!"));

  shell.handle(toInputEvent(Button::B, Gesture::LongPress));  // hold B: exit
  TEST_ASSERT_TRUE(shell.activeApp() == nullptr);

  shell.handle(toInputEvent(Button::A, Gesture::LongPress));  // reopen resets greeting
  shell.draw(display);
  TEST_ASSERT_TRUE(display.shows("Hello, World!"));
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
  RUN_TEST(test_hello_world_app_flow);
  return UNITY_END();
}

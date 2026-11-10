// Host-side tests for the paperOS core and apps. Run with: pio test -e native
#include <unity.h>

#include <paperos.h>
#include <HelloWorldApp.h>

#include <cstring>
#include <algorithm>
#include <map>
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
    filledRects = 0;
    outlinedRects = 0;
    ++frames;
  }
  void endFrame(Refresh refresh) override { lastRefresh = refresh; }
  void fillRect(int, int, int, int, Color) override { ++filledRects; }
  void drawRect(int, int, int, int, Color) override { ++outlinedRects; }
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
  int filledRects = 0;
  int outlinedRects = 0;
  int frames = 0;
  Refresh lastRefresh = Refresh::Fast;
};

// A display that can show images, recording which it was asked for.
class ImageDisplay : public FakeDisplay {
 public:
  bool drawImage(const char* path) override {
    images.emplace_back(path);
    return std::string(path).find("broken") == std::string::npos;
  }
  std::vector<std::string> images;
};

// Lists a fixed set of entries in /wallpapers.
class ListingFileSystem : public FileSystem {
 public:
  std::unique_ptr<File> open(const char*, FileMode) override { return nullptr; }
  bool list(const char* path, std::vector<DirEntry>& out) override {
    if (!hasDir || std::string(path) != kWallpaperDir) return false;
    out = entries;
    return true;
  }
  bool makeDir(const char*) override { return false; }
  bool remove(const char*) override { return false; }
  bool rename(const char*, const char*) override { return false; }

  void add(const char* name, bool isDirectory = false) {
    DirEntry entry;
    entry.name = name;
    entry.isDirectory = isDirectory;
    entries.push_back(entry);
  }
  bool hasDir = true;
  std::vector<DirEntry> entries;
};

// Keeps files in memory, with append support, for the log file tests.
class AppendFileSystem : public FileSystem {
 public:
  class MemoryFile : public File {
   public:
    explicit MemoryFile(std::string& data) : data_(data) {}
    size_t read(void*, size_t) override { return 0; }
    size_t write(const void* data, size_t size) override {
      data_.append(static_cast<const char*>(data), size);
      return size;
    }
    bool seek(uint32_t) override { return false; }
    uint32_t size() const override { return uint32_t(data_.size()); }

   private:
    std::string& data_;
  };

  std::unique_ptr<File> open(const char* path, FileMode mode) override {
    if (failWrites) return nullptr;
    if (mode == FileMode::Write) files[path].clear();
    if (mode == FileMode::Read && files.count(path) == 0) return nullptr;
    return std::unique_ptr<File>(new MemoryFile(files[path]));
  }
  bool list(const char* path, std::vector<DirEntry>& out) override {
    if (std::string(path) != "/") return false;
    out.clear();
    for (const auto& file : files) {
      DirEntry entry;
      entry.name = file.first.substr(1);
      entry.size = uint32_t(file.second.size());
      out.push_back(entry);
    }
    return true;
  }
  bool makeDir(const char*) override { return false; }
  bool remove(const char* path) override { return files.erase(path) > 0; }
  bool rename(const char*, const char*) override { return false; }

  std::map<std::string, std::string> files;
  bool failWrites = false;
};

// An in-memory file to read image headers from.
class BytesFile : public File {
 public:
  explicit BytesFile(std::string data) : data_(std::move(data)) {}
  size_t read(void* buffer, size_t size) override {
    const size_t n = pos_ < data_.size() ? std::min(size, data_.size() - pos_) : 0;
    std::memcpy(buffer, data_.data() + pos_, n);
    pos_ += n;
    return n;
  }
  size_t write(const void*, size_t) override { return 0; }
  bool seek(uint32_t position) override {
    pos_ = position;
    return position <= data_.size();
  }
  uint32_t size() const override { return uint32_t(data_.size()); }

 private:
  std::string data_;
  size_t pos_ = 0;
};

std::string bytes(std::initializer_list<int> values) {
  std::string out;
  for (int v : values) out += char(v);
  return out;
}

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

class FakeBattery : public Battery {
 public:
  int level() const override { return charge; }
  int charge = 87;
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

void test_config_array_tables() {
  Config c;
  const char* text =
      "[calendar]\n"
      "  days = 7\n"
      "[[calendar.feed]]\n"
      "  name = \"Home\"\n"
      "  url = \"https://a.example/home.ics\"\n"
      "[[ calendar . feed ]]  # second\n"
      "  url = \"webcal://b.example/work.ics\"\n"
      "[owner]\n"
      "  name = \"Dan\"\n";
  TEST_ASSERT_TRUE(c.parse(text));
  TEST_ASSERT_EQUAL_INT(2, (int)c.tableCount("calendar.feed"));
  TEST_ASSERT_EQUAL_INT(0, (int)c.tableCount("calendar"));
  TEST_ASSERT_EQUAL_STRING("Home", c.getTableString("calendar.feed", 0, "name"));
  TEST_ASSERT_EQUAL_STRING("https://a.example/home.ics", c.getTableString("calendar.feed", 0, "url"));
  TEST_ASSERT_EQUAL_STRING("webcal://b.example/work.ics",
                           c.getTableString("calendar.feed", 1, "url"));
  TEST_ASSERT_EQUAL_STRING("none", c.getTableString("calendar.feed", 1, "name", "none"));
  TEST_ASSERT_EQUAL_STRING("none", c.getTableString("calendar.feed", 2, "url", "none"));
  // Table keys don't leak into a [section] of the same name, and back.
  TEST_ASSERT_FALSE(c.has("calendar.feed", "url"));
  TEST_ASSERT_EQUAL_STRING("", c.getTableString("calendar", 0, "days"));
  TEST_ASSERT_EQUAL_STRING("Dan", c.getString("owner", "name"));

  TEST_ASSERT_FALSE(c.parse("[[feed]\n[feed]]\n[[]]\n"));
  TEST_ASSERT_EQUAL_INT(3, c.errorCount());
  TEST_ASSERT_EQUAL_INT(0, (int)c.tableCount("feed"));
}

void test_config_reads_whole_numbers() {
  Config c;
  c.parse("[calendar]\ndays = 21\nneg = -3\nbad = 2.5\nword = \"7\"\nempty = \"\"\n");
  TEST_ASSERT_EQUAL_INT(21, (int)c.getInt("calendar", "days", 14));
  TEST_ASSERT_EQUAL_INT(-3, (int)c.getInt("calendar", "neg", 0));
  TEST_ASSERT_EQUAL_INT(14, (int)c.getInt("calendar", "bad", 14));
  TEST_ASSERT_EQUAL_INT(7, (int)c.getInt("calendar", "word", 14));
  TEST_ASSERT_EQUAL_INT(14, (int)c.getInt("calendar", "empty", 14));
  TEST_ASSERT_EQUAL_INT(14, (int)c.getInt("calendar", "missing", 14));
}

// --- Time zones ------------------------------------------------------------

void test_time_zone_names_and_rules() {
  TEST_ASSERT_EQUAL_STRING("CET-1CEST,M3.5.0,M10.5.0/3", posixTimeZone("Europe/Berlin"));
  TEST_ASSERT_EQUAL_STRING("EST5EDT,M3.2.0,M11.1.0", posixTimeZone("America/New_York"));
  TEST_ASSERT_EQUAL_STRING("UTC0", posixTimeZone("UTC"));
  TEST_ASSERT_EQUAL_STRING("CET-1CEST,M3.5.0,M10.5.0/3",
                           posixTimeZone("CET-1CEST,M3.5.0,M10.5.0/3"));
  TEST_ASSERT_EQUAL_STRING("<+03>-3", posixTimeZone("<+03>-3"));
  TEST_ASSERT_NULL(posixTimeZone("Mars/Olympus_Mons"));
  TEST_ASSERT_NULL(posixTimeZone(""));
  TEST_ASSERT_NULL(posixTimeZone(nullptr));
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

void test_home_screen_shows_the_battery_level() {
  Shell shell;
  FakeBattery battery;
  FakeDisplay display;
  shell.setBattery(&battery);
  shell.draw(display);
  TEST_ASSERT_TRUE(display.shows("87%"));

  battery.charge = -1;  // can't be read
  shell.draw(display);
  for (const std::string& text : display.texts) {
    TEST_ASSERT_EQUAL(std::string::npos, text.find('%'));
  }
}

void test_wifi_status_icon() {
  FakeDisplay display;
  display.beginFrame(Color::White);
  TEST_ASSERT_EQUAL_INT(0, drawWifiStatus(display, NetworkState::Off, 500, 0, 24));
  TEST_ASSERT_EQUAL_INT(0, drawWifiStatus(display, NetworkState::Failed, 500, 0, 24));
  TEST_ASSERT_EQUAL_INT(0, display.filledRects + display.outlinedRects);

  const int width = drawWifiStatus(display, NetworkState::Connected, 500, 0, 24);
  TEST_ASSERT_TRUE(width > 0);
  TEST_ASSERT_EQUAL_INT(3, display.filledRects);
  TEST_ASSERT_EQUAL_INT(width, drawWifiStatus(display, NetworkState::Connecting, 500, 0, 24));
  TEST_ASSERT_EQUAL_INT(3, display.outlinedRects);
}

void test_config_reads_yes_no_settings() {
  Config config;
  config.parse("[Developer]\n"
               "debug = \"true\"\n"
               "a = TRUE\n"
               "b = 'no'\n"
               "c = 0\n"
               "d = maybe\n");
  TEST_ASSERT_TRUE(config.getBool("Developer", "debug", false));
  TEST_ASSERT_TRUE(config.getBool("Developer", "a", false));
  TEST_ASSERT_FALSE(config.getBool("Developer", "b", true));
  TEST_ASSERT_FALSE(config.getBool("Developer", "c", true));
  TEST_ASSERT_TRUE(config.getBool("Developer", "d", true));        // not a yes/no
  TEST_ASSERT_FALSE(config.getBool("developer", "debug", false));  // names are matched as written
  TEST_ASSERT_TRUE(config.getBool("Developer", "missing", true));
}

void test_idle_timer() {
  IdleTimer timer;
  TEST_ASSERT_FALSE(timer.expired(0xFFFFFFF0u));  // never by default

  timer.setTimeoutMinutes(10);
  TEST_ASSERT_EQUAL_UINT32(600000u, timer.timeoutMs());
  timer.reset(1000);
  TEST_ASSERT_FALSE(timer.expired(1000 + 599999));
  TEST_ASSERT_TRUE(timer.expired(1000 + 600000));
  timer.reset(1000 + 600000);  // a button press starts it again
  TEST_ASSERT_FALSE(timer.expired(1000 + 600001));

  // Across millis() wraparound.
  timer.reset(0xFFFFFF00u);
  TEST_ASSERT_FALSE(timer.expired(0x100u));
  TEST_ASSERT_TRUE(timer.expired(0xFFFFFF00u + 600000u));

  timer.setTimeoutMinutes(0);
  TEST_ASSERT_FALSE(timer.expired(0xFFFFFF00u + 600000u));
  timer.setTimeoutMinutes(-5);
  TEST_ASSERT_EQUAL_UINT32(0u, timer.timeoutMs());
}

void test_wallpaper_file_names() {
  TEST_ASSERT_TRUE(isWallpaper("forest.jpg"));
  TEST_ASSERT_TRUE(isWallpaper("Forest.JPEG"));
  TEST_ASSERT_TRUE(isWallpaper("cat.png"));
  TEST_ASSERT_TRUE(isWallpaper("map.bmp"));
  TEST_ASSERT_FALSE(isWallpaper("notes.txt"));
  TEST_ASSERT_FALSE(isWallpaper("jpg"));
  TEST_ASSERT_FALSE(isWallpaper("._forest.jpg"));  // macOS metadata
  TEST_ASSERT_FALSE(isWallpaper(""));
}

void test_sleep_screen_shows_a_random_wallpaper() {
  ListingFileSystem fs;
  fs.add("b.png");
  fs.add("readme.txt");
  fs.add("a.jpg");
  fs.add("c.bmp");
  fs.add("old.jpg", true);  // a folder

  ImageDisplay display;
  // Sorted: a.jpg, b.png, c.bmp. The random number picks one.
  TEST_ASSERT_EQUAL_STRING("/wallpapers/a.jpg", drawSleepScreen(display, fs, 0).c_str());
  TEST_ASSERT_EQUAL_STRING("/wallpapers/b.png", drawSleepScreen(display, fs, 1).c_str());
  TEST_ASSERT_EQUAL_STRING("/wallpapers/c.bmp", drawSleepScreen(display, fs, 5).c_str());
  TEST_ASSERT_FALSE(display.shows("paperOS"));
}

void test_sleep_screen_skips_images_it_cannot_draw() {
  ListingFileSystem fs;
  fs.add("a-broken.jpg");
  fs.add("b.jpg");
  ImageDisplay display;
  TEST_ASSERT_EQUAL_STRING("/wallpapers/b.jpg", drawSleepScreen(display, fs, 0).c_str());
  TEST_ASSERT_EQUAL_INT(2, (int)display.images.size());
}

void test_sleep_screen_without_wallpapers_shows_a_message() {
  ListingFileSystem fs;
  fs.hasDir = false;
  ImageDisplay display;
  display.beginFrame(Color::White);
  TEST_ASSERT_EQUAL_STRING("", drawSleepScreen(display, fs, 7).c_str());
  TEST_ASSERT_TRUE(display.shows("paperOS"));
  TEST_ASSERT_TRUE(display.shows("Press the power button to wake"));

  // Only broken images: the message too.
  fs.hasDir = true;
  fs.add("broken.png");
  display.beginFrame(Color::White);
  TEST_ASSERT_EQUAL_STRING("", drawSleepScreen(display, fs, 0).c_str());
  TEST_ASSERT_TRUE(display.shows("paperOS"));

  // A display that can't show images falls back to the message as well.
  ListingFileSystem withImage;
  withImage.add("a.jpg");
  FakeDisplay plain;
  plain.beginFrame(Color::White);
  TEST_ASSERT_EQUAL_STRING("", drawSleepScreen(plain, withImage, 0).c_str());
  TEST_ASSERT_TRUE(plain.shows("paperOS"));
}

void test_shell_closes_the_app_before_sleep() {
  Shell shell;
  CountingApp app("App");
  shell.addApp(&app);
  shell.closeApp();  // nothing open: no-op
  TEST_ASSERT_EQUAL(0, app.closed);
  shell.handle(InputEvent::Select);
  shell.closeApp();
  TEST_ASSERT_EQUAL(1, app.closed);
  TEST_ASSERT_NULL(shell.activeApp());
}

void test_debug_setting() {
  Config config;
  TEST_ASSERT_FALSE(debugEnabled(config));
  config.parse("[Developer]\ndebug = \"true\"\n");
  TEST_ASSERT_TRUE(debugEnabled(config));
  config.parse("[developer]\ndebug = yes\n");
  TEST_ASSERT_TRUE(debugEnabled(config));
  config.parse("[Developer]\ndebug = \"false\"\n");
  TEST_ASSERT_FALSE(debugEnabled(config));
}

void test_log_file_starts_at_zero_and_appends() {
  AppendFileSystem fs;
  fs.files["/config.toml"] = "x";
  LogFile log(fs, 100);
  log.begin();
  TEST_ASSERT_EQUAL_STRING("/debug.0.log", log.path().c_str());
  TEST_ASSERT_TRUE(log.flush());  // nothing to write
  TEST_ASSERT_EQUAL_INT(0, (int)fs.files.count("/debug.0.log"));

  log.add("first");
  log.add("second\n");
  TEST_ASSERT_TRUE(log.flush());
  log.add("third");
  TEST_ASSERT_TRUE(log.flush());
  TEST_ASSERT_EQUAL_STRING("first\nsecond\nthird\n", fs.files["/debug.0.log"].c_str());
}

void test_log_file_starts_a_new_file_when_full() {
  AppendFileSystem fs;
  LogFile log(fs, 20);
  log.begin();
  log.add("0123456789");  // 11 bytes
  log.flush();
  log.add("abcdefghij");  // would make 22: new file
  log.flush();
  TEST_ASSERT_EQUAL_STRING("0123456789\n", fs.files["/debug.0.log"].c_str());
  TEST_ASSERT_EQUAL_STRING("abcdefghij\n", fs.files["/debug.1.log"].c_str());
  TEST_ASSERT_EQUAL_INT(1, log.fileNumber());
}

void test_log_file_carries_on_after_a_restart() {
  AppendFileSystem fs;
  fs.files["/debug.0.log"] = std::string(50, 'a');
  fs.files["/debug.2.log"] = "old\n";
  fs.files["/debug.10.log"] = "newest\n";
  fs.files["/debug.x.log"] = "not a log";
  fs.files["/debugger.log"] = "nor this";
  LogFile log(fs, 20);
  log.begin();
  TEST_ASSERT_EQUAL_STRING("/debug.10.log", log.path().c_str());
  log.add("more");
  log.flush();
  TEST_ASSERT_EQUAL_STRING("newest\nmore\n", fs.files["/debug.10.log"].c_str());
}

void test_log_file_keeps_lines_until_the_card_works() {
  AppendFileSystem fs;
  fs.failWrites = true;
  LogFile log(fs);
  log.begin();
  log.add("kept");
  TEST_ASSERT_FALSE(log.flush());
  TEST_ASSERT_TRUE(log.hasPending());
  fs.failWrites = false;
  TEST_ASSERT_TRUE(log.flush());
  TEST_ASSERT_EQUAL_STRING("kept\n", fs.files["/debug.0.log"].c_str());

  // Without a card, memory use is capped.
  fs.failWrites = true;
  for (int i = 0; i < 10000; ++i) log.add("a line that is a few dozen bytes long");
  TEST_ASSERT_FALSE(log.flush());
  fs.failWrites = false;
  log.flush();
  TEST_ASSERT_TRUE(fs.files["/debug.0.log"].size() <= LogFile::kMaxPendingBytes + 5);
}

// Feeds samples every 20 ms, like main.cpp: down for `down` ms, then up for
// `up` ms. Returns the last non-zero count seen.
int pressFor(PressCounter& counter, uint32_t& t, uint32_t down, uint32_t up) {
  int result = 0;
  for (uint32_t end = t + down; t < end; t += 20) result = counter.update(true, t) ?: result;
  for (uint32_t end = t + up; t < end; t += 20) result = counter.update(false, t) ?: result;
  return result;
}

void test_power_button_press_counter() {
  PressCounter counter;
  uint32_t t = 1000;
  // Timings from a real double press: 105 ms down, 95 ms up, 140 ms down.
  TEST_ASSERT_EQUAL_INT(0, pressFor(counter, t, 100, 100));
  TEST_ASSERT_EQUAL_INT(2, pressFor(counter, t, 140, 1000));

  // A single press.
  TEST_ASSERT_EQUAL_INT(1, pressFor(counter, t, 120, 1000));

  // Two presses further apart than the gap are two singles.
  TEST_ASSERT_EQUAL_INT(1, pressFor(counter, t, 120, 600));
  TEST_ASSERT_EQUAL_INT(1, pressFor(counter, t, 120, 1000));

  // A hold isn't a press, and cancels a press just before it.
  TEST_ASSERT_EQUAL_INT(0, pressFor(counter, t, 2000, 1000));
  TEST_ASSERT_EQUAL_INT(0, pressFor(counter, t, 100, 100) + pressFor(counter, t, 1500, 1000));

  // Three quick presses.
  pressFor(counter, t, 100, 100);
  pressFor(counter, t, 100, 100);
  TEST_ASSERT_EQUAL_INT(3, pressFor(counter, t, 100, 1000));
}

void test_reads_image_sizes() {
  int w = 0, h = 0;

  // PNG 800x1200: signature, then the IHDR chunk.
  BytesFile png(bytes({0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 13, 'I', 'H', 'D', 'R',
                       0, 0, 0x03, 0x20, 0, 0, 0x04, 0xB0, 8, 0}));
  TEST_ASSERT_TRUE(readImageSize(png, w, h));
  TEST_ASSERT_EQUAL_INT(800, w);
  TEST_ASSERT_EQUAL_INT(1200, h);

  // BMP 1920x1080 stored top-down (negative height).
  std::string bmp = bytes({'B', 'M'}) + std::string(16, '\0');
  bmp += bytes({0x80, 0x07, 0, 0, 0xC8, 0xFB, 0xFF, 0xFF});  // 1920, -1080
  BytesFile bmpFile(bmp);
  TEST_ASSERT_TRUE(readImageSize(bmpFile, w, h));
  TEST_ASSERT_EQUAL_INT(1920, w);
  TEST_ASSERT_EQUAL_INT(1080, h);

  // JPEG 640x480 with an APP1 (EXIF) segment and padding before the frame.
  std::string jpeg = bytes({0xFF, 0xD8, 0xFF, 0xE1, 0x00, 0x10}) + std::string(14, 'x');
  jpeg += bytes({0xFF, 0xFF, 0xC4, 0x00, 0x04, 0, 0});            // padding, then DHT
  jpeg += bytes({0xFF, 0xC2, 0x00, 0x11, 8, 0x01, 0xE0, 0x02, 0x80, 3});  // progressive SOF
  BytesFile jpegFile(jpeg);
  TEST_ASSERT_TRUE(readImageSize(jpegFile, w, h));
  TEST_ASSERT_EQUAL_INT(640, w);
  TEST_ASSERT_EQUAL_INT(480, h);

  // Not an image, or cut short.
  BytesFile text("hello, world");
  TEST_ASSERT_FALSE(readImageSize(text, w, h));
  BytesFile cut(bytes({0xFF, 0xD8, 0xFF, 0xE1, 0x00, 0x40}));
  TEST_ASSERT_FALSE(readImageSize(cut, w, h));
  BytesFile empty("");
  TEST_ASSERT_FALSE(readImageSize(empty, w, h));
}

void test_cover_scale_fills_the_screen() {
  // Landscape 1920x1080 on the 540x960 portrait screen: the height decides.
  const float landscape = coverScale(1920, 1080, 540, 960);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 960.0f / 1080.0f, landscape);
  TEST_ASSERT_TRUE(1920 * landscape >= 540 && 1080 * landscape >= 959.9f);
  // A tall narrow image: the width decides.
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 2.0f, coverScale(270, 2000, 540, 960));
  // Exactly the screen: 1.
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.0f, coverScale(540, 960, 540, 960));
  TEST_ASSERT_EQUAL_FLOAT(0.0f, coverScale(0, 100, 540, 960));
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
  RUN_TEST(test_config_array_tables);
  RUN_TEST(test_config_reads_whole_numbers);
  RUN_TEST(test_config_reads_yes_no_settings);
  RUN_TEST(test_time_zone_names_and_rules);
  RUN_TEST(test_hello_world_app_flow);
  RUN_TEST(test_hello_world_app_without_owner_name);
  RUN_TEST(test_shell_ticks_only_the_open_app);
  RUN_TEST(test_home_screen_shows_the_battery_level);
  RUN_TEST(test_app_can_request_a_quality_refresh);
  RUN_TEST(test_wifi_status_icon);
  RUN_TEST(test_idle_timer);
  RUN_TEST(test_wallpaper_file_names);
  RUN_TEST(test_sleep_screen_shows_a_random_wallpaper);
  RUN_TEST(test_sleep_screen_skips_images_it_cannot_draw);
  RUN_TEST(test_sleep_screen_without_wallpapers_shows_a_message);
  RUN_TEST(test_shell_closes_the_app_before_sleep);
  RUN_TEST(test_power_button_press_counter);
  RUN_TEST(test_reads_image_sizes);
  RUN_TEST(test_cover_scale_fills_the_screen);
  RUN_TEST(test_debug_setting);
  RUN_TEST(test_log_file_starts_at_zero_and_appends);
  RUN_TEST(test_log_file_starts_a_new_file_when_full);
  RUN_TEST(test_log_file_carries_on_after_a_restart);
  RUN_TEST(test_log_file_keeps_lines_until_the_card_works);
  return UNITY_END();
}

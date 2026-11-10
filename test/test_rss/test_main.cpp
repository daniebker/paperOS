// Host-side tests for the feed parser and the RSS app. Run with:
// pio test -e native
#include <unity.h>

#include <paperos.h>
#include <Feed.h>
#include <RssApp.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace paperos;
using rss::kNoTime;

namespace {

constexpr const char* kBerlin = "CET-1CEST,M3.5.0,M10.5.0/3";
constexpr const char* kNewsUrl = "https://news.example.com/rss.xml";
constexpr const char* kBlogUrl = "https://blog.example.com/atom.xml";

// --- Fakes -------------------------------------------------------------------

class FakeDisplay : public Display {
 public:
  int width() const override { return 540; }
  int height() const override { return 960; }
  void beginFrame(Color) override { texts.clear(); }
  void endFrame(Refresh) override {}
  void fillRect(int, int, int, int, Color) override {}
  void drawRect(int, int, int, int, Color) override {}
  void drawLine(int, int, int, int, Color) override {}
  void drawText(const char* text, int, int, TextSize, TextAlign, Color) override {
    texts.emplace_back(text);
  }
  int textHeight(TextSize) const override { return 30; }
  int textWidth(const char* text, TextSize) const override { return 10 * int(strlen(text)); }
  bool shows(const std::string& text) const {
    return std::find(texts.begin(), texts.end(), text) != texts.end();
  }
  std::vector<std::string> texts;
};

class FakeBattery : public Battery {
 public:
  int level() const override { return 80; }
};

class FakeClock : public Clock {
 public:
  int64_t now() const override { return time; }
  uint32_t uptimeMs() const override { return ms; }
  int64_t time = -1;
  uint32_t ms = 0;
};

class MemoryFileSystem : public FileSystem {
 public:
  class MemoryFile : public File {
   public:
    explicit MemoryFile(std::string& data) : data_(data) {}
    size_t read(void* buffer, size_t size) override {
      const size_t count = pos_ >= data_.size() ? 0 : std::min(size, data_.size() - pos_);
      memcpy(buffer, data_.data() + pos_, count);
      pos_ += count;
      return count;
    }
    size_t write(const void* data, size_t size) override {
      data_.append(static_cast<const char*>(data), size);
      return size;
    }
    bool seek(uint32_t position) override {
      pos_ = position;
      return position <= data_.size();
    }
    uint32_t size() const override { return uint32_t(data_.size()); }

   private:
    std::string& data_;
    size_t pos_ = 0;
  };

  std::unique_ptr<File> open(const char* path, FileMode mode) override {
    ++opens[path];
    if (mode == FileMode::Write) {
      files[path].clear();
    } else if (files.find(path) == files.end()) {
      return nullptr;
    }
    return std::unique_ptr<File>(new MemoryFile(files[path]));
  }
  bool list(const char* path, std::vector<DirEntry>& entries) override {
    entries.clear();
    if (dirs.count(path) == 0) return false;
    const std::string prefix = std::string(path) + "/";
    for (const auto& file : files) {
      if (file.first.compare(0, prefix.size(), prefix) != 0) continue;
      DirEntry entry;
      entry.name = file.first.substr(prefix.size());
      entries.push_back(entry);
    }
    return true;
  }
  bool makeDir(const char* path) override { return dirs.insert({path, true}).second; }
  bool remove(const char* path) override { return files.erase(path) > 0; }
  bool rename(const char* from, const char* to) override {
    auto found = files.find(from);
    if (found == files.end() || files.count(to) > 0) return false;
    files[to] = found->second;
    files.erase(from);
    return true;
  }
  std::map<std::string, std::string> files;
  std::map<std::string, bool> dirs;
  std::map<std::string, int> opens;
};

class FakeResponse : public HttpResponse {
 public:
  FakeResponse(const std::string& body, bool failAtEnd) : body_(body), failAtEnd_(failAtEnd) {}
  int read(void* buffer, size_t size) override {
    waiting_ = !waiting_;
    if (waiting_) return 0;
    if (pos_ >= body_.size()) return failAtEnd_ ? kError : kEnd;
    const size_t count = std::min<size_t>({size, body_.size() - pos_, 700});
    memcpy(buffer, body_.data() + pos_, count);
    pos_ += count;
    return int(count);
  }
  int64_t contentLength() const override { return int64_t(body_.size()); }

 private:
  std::string body_;
  bool failAtEnd_;
  size_t pos_ = 0;
  bool waiting_ = false;
};

class FakeNetwork : public Network {
 public:
  bool configured() const override { return isConfigured; }
  void connect() override {
    ++connects;
    state_ = NetworkState::Connecting;
  }
  void disconnect() override {
    ++disconnects;
    state_ = NetworkState::Off;
  }
  NetworkState state() override {
    if (state_ == NetworkState::Connecting) {
      state_ = joinFails ? NetworkState::Failed : NetworkState::Connected;
    }
    return state_;
  }
  std::unique_ptr<HttpResponse> get(const char* url, std::string& error) override {
    TEST_ASSERT_TRUE(state_ == NetworkState::Connected);
    requests.push_back(url);
    auto found = bodies.find(url);
    if (found == bodies.end()) {
      error = "HTTP 404";
      return nullptr;
    }
    return std::unique_ptr<HttpResponse>(new FakeResponse(found->second, failAtEnd));
  }

  bool isConfigured = true;
  bool joinFails = false;
  bool failAtEnd = false;
  int connects = 0;
  int disconnects = 0;
  std::map<std::string, std::string> bodies;
  std::vector<std::string> requests;

 private:
  NetworkState state_ = NetworkState::Off;
};

// --- Sample feeds -----------------------------------------------------------------

const char* kRss =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!-- generated -->\n"
    "<rss version=\"2.0\" xmlns:content=\"http://purl.org/rss/1.0/modules/content/\"\n"
    "     xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
    "<channel>\n"
    "  <title>Example News</title>\n"
    "  <link>https://news.example.com/</link>\n"
    "  <image><title>Logo</title><url>https://news.example.com/logo.png</url></image>\n"
    "  <item>\n"
    "    <title>Fish &amp; Chips &#8211; a history</title>\n"
    "    <link>https://news.example.com/fish</link>\n"
    "    <guid isPermaLink=\"false\">fish-1</guid>\n"
    "    <pubDate>Tue, 10 Nov 2026 08:00:00 +0000</pubDate>\n"
    "    <description><![CDATA[<p>Short summary.</p>]]></description>\n"
    "    <content:encoded><![CDATA[<p>First paragraph with <b>bold</b> &amp; "
    "<a href=\"x\">a link</a>.</p><p>Second<br/>line.</p><script>alert(1)</script>]]>"
    "</content:encoded>\n"
    "  </item>\n"
    "  <item>\n"
    "    <title>Older story</title>\n"
    "    <link>https://news.example.com/older</link>\n"
    "    <pubDate>Sun, 08 Nov 2026 18:30:00 GMT</pubDate>\n"
    "    <description>&lt;p&gt;Escaped &amp;amp; HTML.&lt;/p&gt;</description>\n"
    "  </item>\n"
    "  <item>\n"
    "    <description>A post without a title, like a microblog.</description>\n"
    "    <dc:date>2026-11-09T12:00:00Z</dc:date>\n"
    "  </item>\n"
    "</channel>\n"
    "</rss>\n";

const char* kAtom =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<feed xmlns=\"http://www.w3.org/2005/Atom\">\n"
    "  <title type=\"text\">Dan's Blog</title>\n"
    "  <link rel=\"self\" href=\"https://blog.example.com/atom.xml\"/>\n"
    "  <entry>\n"
    "    <title type=\"html\">&lt;b&gt;Bold&lt;/b&gt; news</title>\n"
    "    <link rel=\"self\" href=\"https://blog.example.com/self\"/>\n"
    "    <link rel=\"alternate\" type=\"text/html\" href=\"https://blog.example.com/p?a=1&amp;b=2\"/>\n"
    "    <id>tag:blog.example.com,2026:1</id>\n"
    "    <updated>2026-11-10T10:00:00+01:00</updated>\n"
    "    <published>2026-11-09T20:00:00+01:00</published>\n"
    "    <summary>Summary only</summary>\n"
    "    <content type=\"xhtml\"><div xmlns=\"http://www.w3.org/1999/xhtml\">"
    "<p>Inline <em>XHTML</em> body.</p><p>Caf\xC3\xA9 time.</p></div></content>\n"
    "  </entry>\n"
    "  <entry>\n"
    "    <title>Updated only</title>\n"
    "    <link href=\"https://blog.example.com/u\"/>\n"
    "    <id>tag:blog.example.com,2026:2</id>\n"
    "    <updated>2026-11-10T07:00:00Z</updated>\n"
    "    <summary type=\"html\">&lt;p&gt;Hello&lt;/p&gt;</summary>\n"
    "  </entry>\n"
    "</feed>\n";

std::vector<rss::Article> parseAll(const std::string& text, size_t pieceSize = 1 << 20,
                                   size_t max = 50, rss::FeedParser** keep = nullptr) {
  static rss::FeedParser* last = nullptr;
  delete last;
  last = new rss::FeedParser(max, 16 * 1024);
  std::vector<rss::Article> out;
  for (size_t i = 0; i < text.size(); i += pieceSize) {
    last->feed(text.data() + i, std::min(pieceSize, text.size() - i));
    for (auto& a : last->take()) out.push_back(a);
  }
  last->finish();
  for (auto& a : last->take()) out.push_back(a);
  if (keep != nullptr) *keep = last;
  return out;
}

const char* kConfig =
    "[[rss.feed]]\n"
    "  url = \"https://news.example.com/rss.xml\"\n"
    "[[rss.feed]]\n"
    "  name = \"Blog\"\n"
    "  url = \"https://blog.example.com/atom.xml\"\n";

// Tuesday 10 November 2026, 12:00 in Berlin.
int64_t noon() { return rss::parseDate("2026-11-10T11:00:00Z"); }

struct Harness {
  explicit Harness(const char* text = kConfig) {
    config.parse(text);
    clock.time = noon();
    network.bodies[kNewsUrl] = kRss;
    network.bodies[kBlogUrl] = kAtom;
  }
  void run(int limit = 2000) {
    for (int i = 0; i < limit; ++i) {
      app.tick();
      if (app.phase() == RssApp::Phase::Idle) return;
    }
  }
  std::vector<std::string> titles() const {
    std::vector<std::string> out;
    for (const auto& e : app.entries()) out.push_back(e.title);
    return out;
  }
  Config config;
  MemoryFileSystem fs;
  FakeNetwork network;
  FakeClock clock;
  FakeDisplay display;
  FakeBattery battery;
  RssApp app{config, fs, network, clock, display, battery};
};

void assertList(const std::vector<std::string>& expected, const std::vector<std::string>& got) {
  std::string e, g;
  for (const auto& s : expected) e += s + "|";
  for (const auto& s : got) g += s + "|";
  TEST_ASSERT_EQUAL_STRING(e.c_str(), g.c_str());
}

}  // namespace

void setUp() {
  setenv("TZ", kBerlin, 1);
  tzset();
}
void tearDown() {}

// --- The parser ---------------------------------------------------------------------

void test_parser_reads_rss() {
  rss::FeedParser* parser = nullptr;
  const auto articles = parseAll(kRss, 1 << 20, 50, &parser);
  TEST_ASSERT_TRUE(parser->isFeed());
  TEST_ASSERT_EQUAL_STRING("Example News", parser->feedTitle().c_str());
  TEST_ASSERT_EQUAL(3, int(articles.size()));

  const rss::Article& fish = articles[0];
  TEST_ASSERT_EQUAL_STRING("Fish & Chips - a history", fish.title.c_str());
  TEST_ASSERT_EQUAL_STRING("https://news.example.com/fish", fish.link.c_str());
  TEST_ASSERT_EQUAL_STRING("fish-1", fish.id.c_str());
  TEST_ASSERT_TRUE(fish.published == rss::parseDate("2026-11-10T08:00:00Z"));
  // content:encoded wins over the description; scripts are dropped.
  TEST_ASSERT_EQUAL_STRING("First paragraph with bold & a link.\n\nSecond\nline.",
                           fish.text.c_str());

  TEST_ASSERT_EQUAL_STRING("Escaped & HTML.", articles[1].text.c_str());
  TEST_ASSERT_EQUAL_STRING("https://news.example.com/older", articles[1].id.c_str());
  // No title: the start of the text.
  TEST_ASSERT_EQUAL_STRING("A post without a title, like a microblog.", articles[2].title.c_str());
  TEST_ASSERT_TRUE(articles[2].published == rss::parseDate("2026-11-09T12:00:00Z"));
}

void test_parser_reads_atom() {
  rss::FeedParser* parser = nullptr;
  const auto articles = parseAll(kAtom, 1 << 20, 50, &parser);
  TEST_ASSERT_EQUAL_STRING("Dan's Blog", parser->feedTitle().c_str());
  TEST_ASSERT_EQUAL(2, int(articles.size()));
  const rss::Article& first = articles[0];
  TEST_ASSERT_EQUAL_STRING("Bold news", first.title.c_str());
  TEST_ASSERT_EQUAL_STRING("https://blog.example.com/p?a=1&b=2", first.link.c_str());
  TEST_ASSERT_EQUAL_STRING("tag:blog.example.com,2026:1", first.id.c_str());
  TEST_ASSERT_TRUE(first.published == rss::parseDate("2026-11-09T19:00:00Z"));
  TEST_ASSERT_EQUAL_STRING("Inline XHTML body.\n\nCafe time.", first.text.c_str());

  TEST_ASSERT_EQUAL_STRING("https://blog.example.com/u", articles[1].link.c_str());
  TEST_ASSERT_TRUE(articles[1].published == rss::parseDate("2026-11-10T07:00:00Z"));
  TEST_ASSERT_EQUAL_STRING("Hello", articles[1].text.c_str());
}

void test_parser_reads_rss_1() {
  const char* rdf =
      "<?xml version=\"1.0\"?><rdf:RDF xmlns:rdf=\"r\" xmlns=\"http://purl.org/rss/1.0/\">"
      "<channel><title>RDF feed</title></channel>"
      "<item rdf:about=\"https://x/1\"><title>One</title><link>https://x/1</link>"
      "<description>Body</description></item></rdf:RDF>";
  rss::FeedParser* parser = nullptr;
  const auto articles = parseAll(rdf, 1 << 20, 50, &parser);
  TEST_ASSERT_TRUE(parser->isFeed());
  TEST_ASSERT_EQUAL_STRING("RDF feed", parser->feedTitle().c_str());
  TEST_ASSERT_EQUAL(1, int(articles.size()));
  TEST_ASSERT_EQUAL_STRING("Body", articles[0].text.c_str());
}

void test_parser_reads_any_piece_sizes() {
  for (const char* feed : {kRss, kAtom}) {
    const auto whole = parseAll(feed);
    for (size_t piece : {1, 2, 3, 7, 64}) {
      const auto pieces = parseAll(feed, piece);
      TEST_ASSERT_EQUAL(int(whole.size()), int(pieces.size()));
      for (size_t i = 0; i < whole.size(); ++i) {
        TEST_ASSERT_EQUAL_STRING(whole[i].title.c_str(), pieces[i].title.c_str());
        TEST_ASSERT_EQUAL_STRING(whole[i].text.c_str(), pieces[i].text.c_str());
        TEST_ASSERT_EQUAL_STRING(whole[i].link.c_str(), pieces[i].link.c_str());
        TEST_ASSERT_TRUE(whole[i].published == pieces[i].published);
      }
    }
  }
}

void test_parser_keeps_the_first_articles() {
  rss::FeedParser* parser = nullptr;
  const auto articles = parseAll(kRss, 1 << 20, 2, &parser);
  TEST_ASSERT_EQUAL(2, int(articles.size()));
  TEST_ASSERT_TRUE(parser->full());
}

void test_parse_date_formats() {
  const int64_t t = rss::parseDate("2026-11-10T08:00:00Z");
  TEST_ASSERT_TRUE(t != kNoTime);
  TEST_ASSERT_TRUE(rss::parseDate("Tue, 10 Nov 2026 08:00:00 +0000") == t);
  TEST_ASSERT_TRUE(rss::parseDate("Tue, 10 Nov 2026 08:00:00 GMT") == t);
  TEST_ASSERT_TRUE(rss::parseDate("Tue, 10 Nov 2026 03:00:00 EST") == t);
  TEST_ASSERT_TRUE(rss::parseDate("10 Nov 2026 09:00:00 +0100") == t);
  TEST_ASSERT_TRUE(rss::parseDate("Tue, 10 Nov 26 08:00 GMT") == t);
  TEST_ASSERT_TRUE(rss::parseDate("Tuesday, 10 November 2026 08:00:00 Z") == t);
  TEST_ASSERT_TRUE(rss::parseDate(" 2026-11-10T09:00:00.123+01:00 ") == t);
  TEST_ASSERT_TRUE(rss::parseDate("2026-11-10T08:00:00") == t);
  TEST_ASSERT_TRUE(rss::parseDate("2026-11-10") == rss::parseDate("2026-11-10T00:00:00Z"));
  TEST_ASSERT_TRUE(rss::parseDate("") == kNoTime);
  TEST_ASSERT_TRUE(rss::parseDate("yesterday") == kNoTime);
  TEST_ASSERT_TRUE(rss::parseDate("Tue, 10 Foo 2026 08:00:00 GMT") == kNoTime);
}

void test_html_to_text() {
  TEST_ASSERT_EQUAL_STRING("A\n\nB", rss::htmlToText("<p>A</p>\n\n<p>B</p>", 100).c_str());
  TEST_ASSERT_EQUAL_STRING("Title\n\nText", rss::htmlToText("<h2>Title</h2>Text", 100).c_str());
  TEST_ASSERT_EQUAL_STRING("- one\n\n- two",
                           rss::htmlToText("<ul><li>one</li><li>two</li></ul>", 100).c_str());
  TEST_ASSERT_EQUAL_STRING("a b", rss::htmlToText("a&nbsp;&nbsp; b", 100).c_str());
  TEST_ASSERT_EQUAL_STRING("Plain text", rss::htmlToText("Plain text", 100).c_str());
  TEST_ASSERT_EQUAL_STRING("one two...", rss::htmlToText("one two three four", 6).c_str());
  TEST_ASSERT_EQUAL_STRING("", rss::htmlToText("<img src=\"x\"/>", 100).c_str());
}

void test_looks_like_feed() {
  TEST_ASSERT_TRUE(rss::looksLikeFeed("<?xml version=\"1.0\"?>"));
  TEST_ASSERT_TRUE(rss::looksLikeFeed("\xEF\xBB\xBF\n  <rss version=\"2.0\">"));
  TEST_ASSERT_TRUE(rss::looksLikeFeed("<feed xmlns="));
  TEST_ASSERT_FALSE(rss::looksLikeFeed("<!DOCTYPE html><html>"));
  TEST_ASSERT_FALSE(rss::looksLikeFeed("{\"items\": []}"));
}

// --- The app ------------------------------------------------------------------------------

void test_app_downloads_and_lists_articles_newest_first() {
  Harness h;
  h.app.onOpen();
  // Nothing downloaded yet: it reads the (missing) feeds, then downloads.
  TEST_ASSERT_EQUAL(int(RssApp::Phase::Loading), int(h.app.phase()));
  h.run();
  TEST_ASSERT_EQUAL(1, h.network.connects);
  TEST_ASSERT_EQUAL(1, h.network.disconnects);
  assertList({"Fish & Chips - a history", "Updated only", "Bold news",
              "A post without a title, like a microblog.", "Older story"},
             h.titles());
  TEST_ASSERT_TRUE(h.fs.files.count(RssApp::cachePath(kNewsUrl)) == 1);
  TEST_ASSERT_TRUE(h.fs.files.count(RssApp::kIndexPath) == 1);
  TEST_ASSERT_TRUE(h.fs.files.count(RssApp::kTextPath) == 1);
  TEST_ASSERT_EQUAL(5, int(h.app.unreadCount()));

  h.app.draw(h.display);
  TEST_ASSERT_TRUE(h.display.shows("Fish & Chips - a history"));
  // The feed's own title, or the name from config.toml.
  TEST_ASSERT_TRUE(h.display.shows("Example News - Today 09:00"));
  TEST_ASSERT_TRUE(h.display.shows("Blog - Today 08:00"));
  TEST_ASSERT_TRUE(h.display.shows("Blog - Yesterday 20:00"));
  TEST_ASSERT_TRUE(h.display.shows("Example News - Sun 8 Nov"));
  TEST_ASSERT_TRUE(h.display.shows("5 unread"));
  TEST_ASSERT_TRUE(h.display.shows("Updated 12:00"));
}

void test_app_shows_the_cached_list_without_reading_the_feeds() {
  Harness h;
  h.app.onOpen();
  h.run();
  h.run();
  h.app.onClose();

  // Within refresh_minutes: no download, and the feeds aren't read again.
  h.clock.time += 60;
  h.clock.ms += 60000;
  const int feedOpens = h.fs.opens[RssApp::cachePath(kNewsUrl)];
  h.app.onOpen();
  TEST_ASSERT_EQUAL(int(RssApp::Phase::Idle), int(h.app.phase()));
  TEST_ASSERT_EQUAL(5, int(h.app.entries().size()));
  TEST_ASSERT_EQUAL(feedOpens, h.fs.opens[RssApp::cachePath(kNewsUrl)]);
  TEST_ASSERT_EQUAL(1, h.network.connects);
  TEST_ASSERT_EQUAL_STRING("First paragraph with bold & a link.\n\nSecond\nline.",
                           h.app.articleText(h.app.entries()[0]).c_str());
}

void test_app_shows_a_stale_list_while_downloading_a_new_one() {
  Harness h;
  h.app.onOpen();
  h.run();
  h.run();
  h.app.onClose();

  // A new article arrives after refresh_minutes.
  std::string rss = kRss;
  rss.replace(rss.find("  <item>"), 0,
              "  <item><title>Breaking</title><link>https://news.example.com/b</link>"
              "<pubDate>Tue, 10 Nov 2026 10:30:00 GMT</pubDate></item>\n");
  h.network.bodies[kNewsUrl] = rss;
  h.clock.time += RssApp::kDefaultRefreshMinutes * 60;
  h.app.onOpen();
  TEST_ASSERT_EQUAL(5, int(h.app.entries().size()));  // the cached list at once
  TEST_ASSERT_EQUAL(int(RssApp::Phase::Connecting), int(h.app.phase()));
  h.app.draw(h.display);
  TEST_ASSERT_TRUE(h.display.shows("Updating..."));
  h.run();
  h.run();
  TEST_ASSERT_EQUAL(6, int(h.app.entries().size()));
  TEST_ASSERT_EQUAL_STRING("Breaking", h.app.entries()[0].title.c_str());
  TEST_ASSERT_TRUE(h.app.wantsFullRefresh());
}

void test_opening_an_article_marks_it_read() {
  Harness h;
  h.app.onOpen();
  h.run();
  h.run();
  h.app.onInput(InputEvent::Next);
  TEST_ASSERT_EQUAL(1, int(h.app.selected()));
  TEST_ASSERT_TRUE(h.app.onInput(InputEvent::Select));
  TEST_ASSERT_EQUAL(int(RssApp::Screen::Article), int(h.app.screen()));
  h.app.draw(h.display);
  TEST_ASSERT_TRUE(h.display.shows("Updated only"));
  TEST_ASSERT_TRUE(h.display.shows("Blog - Today 08:00"));
  TEST_ASSERT_TRUE(h.display.shows("Hello"));
  TEST_ASSERT_EQUAL(4, int(h.app.unreadCount()));
  TEST_ASSERT_TRUE(h.fs.files[RssApp::kReadPath].size() > 0);

  TEST_ASSERT_TRUE(h.app.onInput(InputEvent::Back));
  TEST_ASSERT_EQUAL(int(RssApp::Screen::List), int(h.app.screen()));
  TEST_ASSERT_FALSE(h.app.onInput(InputEvent::Back));  // the shell closes the app

  // Remembered after closing.
  h.app.onClose();
  h.app.onOpen();
  TEST_ASSERT_EQUAL(4, int(h.app.unreadCount()));
}

void test_long_articles_turn_pages() {
  Harness h("[[rss.feed]]\n  url = \"https://news.example.com/rss.xml\"\n");
  std::string body;
  for (int i = 0; i < 80; ++i) body += "&lt;p&gt;Paragraph " + std::to_string(i) + "&lt;/p&gt;";
  h.network.bodies[kNewsUrl] =
      "<rss><channel><item><title>Long read</title><description>" + body +
      "</description></item></channel></rss>";
  h.app.onOpen();
  h.run();
  h.run();
  h.app.onInput(InputEvent::Select);
  TEST_ASSERT_TRUE(h.app.articlePageCount() > 3);
  TEST_ASSERT_FALSE(h.app.onInput(InputEvent::Previous));
  TEST_ASSERT_TRUE(h.app.onInput(InputEvent::Next));
  h.app.draw(h.display);
  TEST_ASSERT_FALSE(h.display.shows("Long read"));
  TEST_ASSERT_TRUE(h.display.shows("A / B: page    Hold B: back"));
  while (h.app.onInput(InputEvent::Next)) {
  }
  TEST_ASSERT_EQUAL(int(h.app.articlePageCount()) - 1, int(h.app.articlePage()));
  h.app.draw(h.display);
  TEST_ASSERT_TRUE(h.display.shows("Paragraph 79"));
}

void test_failed_download_keeps_the_old_copy() {
  Harness h;
  h.app.onOpen();
  h.run();
  h.run();
  const std::string saved = h.fs.files[RssApp::cachePath(kNewsUrl)];

  h.network.bodies[kNewsUrl] = "<!DOCTYPE html><html>Sign in</html>";
  h.network.bodies.erase(kBlogUrl);
  h.clock.ms += RssApp::kDefaultRefreshMinutes * 60000u;
  h.clock.time += RssApp::kDefaultRefreshMinutes * 60;
  h.app.tick();
  h.run();
  TEST_ASSERT_TRUE(h.fs.files[RssApp::cachePath(kNewsUrl)] == saved);
  TEST_ASSERT_EQUAL(5, int(h.app.entries().size()));
  TEST_ASSERT_EQUAL_STRING("Example News: not a feed, check the url (+1 more)",
                           h.app.statusText().c_str());
  for (const auto& file : h.fs.files) {
    TEST_ASSERT_TRUE(file.first.find(".part") == std::string::npos);
  }
}

void test_app_ignores_a_cached_list_from_other_feeds() {
  Harness h;
  h.app.onOpen();
  h.run();
  h.run();
  h.app.onClose();
  h.config.parse("[[rss.feed]]\n  url = \"https://news.example.com/rss.xml\"\n");
  h.clock.ms += 1000;
  h.app.onOpen();
  // The saved list had the blog too: build it again from the downloaded feed.
  TEST_ASSERT_EQUAL(int(RssApp::Phase::Loading), int(h.app.phase()));
  h.run();
  TEST_ASSERT_EQUAL(3, int(h.app.entries().size()));
}

void test_read_marks_are_forgotten_with_their_articles() {
  Harness h;
  h.app.onOpen();
  h.run();
  h.run();
  h.app.onInput(InputEvent::Select);  // Fish & Chips
  h.app.onInput(InputEvent::Back);
  TEST_ASSERT_EQUAL(4, int(h.app.unreadCount()));

  h.network.bodies[kNewsUrl] = "<rss><channel><item><title>Only</title></item></channel></rss>";
  h.clock.ms += RssApp::kDefaultRefreshMinutes * 60000u;
  h.clock.time += RssApp::kDefaultRefreshMinutes * 60;
  h.app.tick();
  h.run();
  h.run();
  TEST_ASSERT_EQUAL(3, int(h.app.entries().size()));
  TEST_ASSERT_EQUAL(3, int(h.app.unreadCount()));
  TEST_ASSERT_EQUAL(0, int(h.fs.files[RssApp::kReadPath].size()));
}

void test_app_explains_what_to_set() {
  Harness noFeeds("");
  noFeeds.app.onOpen();
  noFeeds.app.draw(noFeeds.display);
  TEST_ASSERT_TRUE(noFeeds.display.shows("No feeds set up"));
  TEST_ASSERT_TRUE(noFeeds.display.shows("Add a [[rss.feed]] url to config.toml"));

  Harness noWifi;
  noWifi.network.isConfigured = false;
  noWifi.app.onOpen();
  noWifi.run();
  TEST_ASSERT_EQUAL(0, noWifi.network.connects);
  noWifi.app.draw(noWifi.display);
  TEST_ASSERT_TRUE(noWifi.display.shows("Not downloaded yet"));
  TEST_ASSERT_TRUE(noWifi.display.shows("Set [wifi] in config.toml"));
}

void test_hold_a_downloads_when_there_is_nothing_to_read() {
  Harness h;
  h.network.joinFails = true;
  h.app.onOpen();
  h.run();
  TEST_ASSERT_EQUAL_STRING("Could not connect to Wi-Fi", h.app.statusText().c_str());
  h.network.joinFails = false;
  TEST_ASSERT_TRUE(h.app.onInput(InputEvent::Select));
  h.run();
  h.run();
  TEST_ASSERT_EQUAL(5, int(h.app.entries().size()));
}

void test_closing_mid_download_cleans_up() {
  Harness h;
  h.app.onOpen();
  for (int i = 0; i < 3; ++i) h.app.tick();
  TEST_ASSERT_EQUAL(int(RssApp::Phase::Downloading), int(h.app.phase()));
  h.app.onClose();
  TEST_ASSERT_EQUAL(1, h.network.disconnects);
  for (const auto& file : h.fs.files) {
    TEST_ASSERT_TRUE(file.first.find(".part") == std::string::npos);
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_parser_reads_rss);
  RUN_TEST(test_parser_reads_atom);
  RUN_TEST(test_parser_reads_rss_1);
  RUN_TEST(test_parser_reads_any_piece_sizes);
  RUN_TEST(test_parser_keeps_the_first_articles);
  RUN_TEST(test_parse_date_formats);
  RUN_TEST(test_html_to_text);
  RUN_TEST(test_looks_like_feed);
  RUN_TEST(test_app_downloads_and_lists_articles_newest_first);
  RUN_TEST(test_app_shows_the_cached_list_without_reading_the_feeds);
  RUN_TEST(test_app_shows_a_stale_list_while_downloading_a_new_one);
  RUN_TEST(test_opening_an_article_marks_it_read);
  RUN_TEST(test_long_articles_turn_pages);
  RUN_TEST(test_failed_download_keeps_the_old_copy);
  RUN_TEST(test_app_ignores_a_cached_list_from_other_feeds);
  RUN_TEST(test_read_marks_are_forgotten_with_their_articles);
  RUN_TEST(test_app_explains_what_to_set);
  RUN_TEST(test_hold_a_downloads_when_there_is_nothing_to_read);
  RUN_TEST(test_closing_mid_download_cleans_up);
  return UNITY_END();
}

// Host-side tests for the e-reader app. Run with: pio test -e native
#include <unity.h>

#include <AsciiFold.h>
#include <BookCache.h>
#include <EReaderApp.h>
#include <Epub.h>
#include <Inflate.h>
#include <PageLayout.h>
#include <XhtmlParser.h>
#include <ZipArchive.h>
#include <paperos.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "deflate_vectors.h"

using namespace paperos;
using namespace ereader;

namespace {

class FakeDisplay : public Display {
 public:
  FakeDisplay(int w = 540, int h = 960) : w_(w), h_(h) {}
  int width() const override { return w_; }
  int height() const override { return h_; }
  void beginFrame(Color) override { texts.clear(); }
  void endFrame(Refresh refresh) override { lastRefresh = refresh; }
  void fillRect(int, int, int, int, Color) override {}
  void drawRect(int, int, int, int, Color) override {}
  void drawLine(int, int, int, int, Color) override {}
  void drawText(const char* text, int, int y, TextSize, TextAlign, Color) override {
    texts.emplace_back(text);
    lowestY = std::max(lowestY, y);
  }
  int textHeight(TextSize) const override { return 30; }
  int textWidth(const char* text, TextSize) const override { return 10 * int(strlen(text)); }

  bool shows(const std::string& text) const {
    return std::find(texts.begin(), texts.end(), text) != texts.end();
  }
  bool showsPart(const std::string& text) const {
    return std::any_of(texts.begin(), texts.end(), [&](const std::string& t) {
      return t.find(text) != std::string::npos;
    });
  }

  std::vector<std::string> texts;
  int lowestY = 0;
  Refresh lastRefresh = Refresh::Fast;

 private:
  int w_;
  int h_;
};

// In-memory FileSystem. Paths in `files` are absolute; directories exist
// implicitly in `dirs`.
class MemoryFileSystem : public FileSystem {
 public:
  class MemoryFile : public File {
   public:
    MemoryFile(std::string& data, bool failWrites) : data_(data), failWrites_(failWrites) {}
    size_t read(void* buffer, size_t size) override {
      const size_t count = pos_ >= data_.size() ? 0 : std::min(size, data_.size() - pos_);
      memcpy(buffer, data_.data() + pos_, count);
      pos_ += count;
      return count;
    }
    size_t write(const void* data, size_t size) override {
      if (failWrites_) return 0;
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
    bool failWrites_;
    size_t pos_ = 0;
  };

  std::unique_ptr<File> open(const char* path, FileMode mode) override {
    ++opens[path];
    if (mode == FileMode::Write) {
      files[path].clear();
    } else if (files.find(path) == files.end()) {
      return nullptr;
    }
    return std::unique_ptr<File>(new MemoryFile(files[path], failWrites));
  }

  bool list(const char* path, std::vector<DirEntry>& entries) override {
    entries.clear();
    if (dirs.find(path) == dirs.end()) return false;
    const std::string prefix = std::string(path) + "/";
    for (const auto& file : files) {
      if (file.first.compare(0, prefix.size(), prefix) != 0) continue;
      const std::string name = file.first.substr(prefix.size());
      if (name.find('/') != std::string::npos) continue;
      DirEntry entry;
      entry.name = name;
      entry.size = uint32_t(file.second.size());
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

  size_t countFiles(const char* suffix) const {
    size_t count = 0;
    const std::string end = suffix;
    for (const auto& file : files) {
      const std::string& name = file.first;
      if (name.size() >= end.size() &&
          name.compare(name.size() - end.size(), end.size(), end) == 0) {
        ++count;
      }
    }
    return count;
  }

  std::map<std::string, std::string> files;
  std::map<std::string, bool> dirs;
  std::map<std::string, int> opens;
  bool failWrites = false;
};

std::unique_ptr<File> memoryFile(std::string& data) {
  return std::unique_ptr<File>(new MemoryFileSystem::MemoryFile(data, false));
}

// Builds a ZIP. Entries are stored unless given as already deflated.
class ZipBuilder {
 public:
  void addStored(const std::string& name, const std::string& data) {
    add(name, data, data.size(), 0);
  }
  void addDeflated(const std::string& name, const std::string& compressed, uint32_t size) {
    add(name, compressed, size, 8);
  }
  std::string finish() {
    const uint32_t directoryOffset = uint32_t(out_.size());
    const std::string result = out_ + directory_;
    std::string end;
    u32(end, 0x06054B50);
    u16(end, 0);
    u16(end, 0);
    u16(end, count_);
    u16(end, count_);
    u32(end, uint32_t(directory_.size()));
    u32(end, directoryOffset);
    u16(end, 0);
    return result + end;
  }

 private:
  static void u16(std::string& s, uint16_t v) {
    s += char(v & 0xFF);
    s += char(v >> 8);
  }
  static void u32(std::string& s, uint32_t v) {
    u16(s, uint16_t(v & 0xFFFF));
    u16(s, uint16_t(v >> 16));
  }
  void add(const std::string& name, const std::string& data, uint32_t size, uint16_t method) {
    const uint32_t offset = uint32_t(out_.size());
    u32(out_, 0x04034B50);
    u16(out_, 20);
    u16(out_, 0);
    u16(out_, method);
    u32(out_, 0);  // time, date
    u32(out_, 0);  // crc
    u32(out_, uint32_t(data.size()));
    u32(out_, size);
    u16(out_, uint16_t(name.size()));
    u16(out_, 0);
    out_ += name + data;

    u32(directory_, 0x02014B50);
    u16(directory_, 20);
    u16(directory_, 20);
    u16(directory_, 0);
    u16(directory_, method);
    u32(directory_, 0);
    u32(directory_, 0);
    u32(directory_, uint32_t(data.size()));
    u32(directory_, size);
    u16(directory_, uint16_t(name.size()));
    u16(directory_, 0);
    u16(directory_, 0);
    u16(directory_, 0);
    u16(directory_, 0);
    u32(directory_, 0);
    u32(directory_, offset);
    directory_ += name;
    ++count_;
  }

  std::string out_;
  std::string directory_;
  uint16_t count_ = 0;
};

std::string longDeflateText() {
  std::string text;
  for (int i = 0; i < 5000; ++i) text += "line " + std::to_string(i % 97) + " of the book\n";
  return text;
}

const char kContainer[] =
    "<?xml version='1.0'?><container><rootfiles>"
    "<rootfile full-path='OEBPS/content.opf' media-type='application/oebps-package+xml'/>"
    "</rootfiles></container>";

const char kPackage[] =
    "<?xml version='1.0' encoding='utf-8'?>\n"
    "<opf:package xmlns:opf='http://www.idpf.org/2007/opf'>\n"
    "<opf:metadata><dc:title id='t'>Fish &amp; Chips</dc:title>\n"
    "<dc:creator>Ann Author</dc:creator><dc:creator>Second Author</dc:creator></opf:metadata>\n"
    "<!-- <opf:item id='ghost' href='ghost.xhtml'/> -->\n"
    "<opf:manifest>\n"
    "<opf:item id='ch2' href='text/two.xhtml' media-type='application/xhtml+xml'/>\n"
    "<opf:item href=\"text/chapter%20one.xhtml#top\" id=\"ch1\" "
    "media-type=\"application/xhtml+xml\"/>\n"
    "<opf:item id='note' href='note.xhtml' media-type='application/xhtml+xml'/>\n"
    "<opf:item id='cover' href='cover.jpg' media-type='image/jpeg'/>\n"
    "<opf:item id='gone' href='missing.xhtml' media-type='application/xhtml+xml'/>\n"
    "<opf:item id='ch3' href='../deflated.xhtml' media-type='application/xhtml+xml'/>\n"
    "</opf:manifest><opf:spine>\n"
    "<opf:itemref idref='cover'/><opf:itemref idref='ch1'/><opf:itemref idref='note' linear='no'/>\n"
    "<opf:itemref idref='gone'/><opf:itemref idref='ch2'/><opf:itemref idref='ch3'/>\n"
    "</opf:spine></opf:package>";

std::string chapterText(const char* word, int paragraphs) {
  std::string body = "<html><head><title>Hidden</title></head><body><h1>Chapter</h1>";
  for (int i = 0; i < paragraphs; ++i) {
    body += "<p>";
    for (int w = 0; w < 30; ++w) body += std::string(word) + " ";
    body += "</p>";
  }
  return body + "</body></html>";
}

// An EPUB with three chapters; `paragraphs` sets its length in pages.
std::string makeEpub(int paragraphs = 40) {
  ZipBuilder zip;
  zip.addStored("mimetype", "application/epub+zip");
  zip.addStored("META-INF/container.xml", kContainer);
  zip.addStored("OEBPS/content.opf", kPackage);
  zip.addStored("OEBPS/text/chapter one.xhtml", chapterText("alpha", paragraphs));
  zip.addStored("OEBPS/text/two.xhtml", chapterText("bravo", paragraphs));
  zip.addStored("OEBPS/note.xhtml", chapterText("note", 2));
  zip.addStored("OEBPS/cover.jpg", "not really a jpeg");
  zip.addDeflated("deflated.xhtml",
                  std::string(reinterpret_cast<const char*>(kChapterDeflate),
                              sizeof(kChapterDeflate)),
                  uint32_t(strlen("<html><body><h1>Deflated</h1><p>Caf\xC3\xA9 "
                                  "\xE2\x80\x9Cquoted\xE2\x80\x9D text.</p></body></html>")));
  return zip.finish();
}

// Records everything the parser produces, with markers for structure.
class RecordingSink : public TextSink {
 public:
  void onChar(char c) override { text += c; }
  void onLineBreak() override { text += "|br|"; }
  void onParagraphEnd() override {
    if (text.size() < 3 || text.compare(text.size() - 3, 3, "|p|") != 0) text += "|p|";
  }
  void onHeading(bool active) override { text += active ? "|h|" : "|/h|"; }
  std::string text;
};

std::string parse(const std::string& xhtml) {
  RecordingSink sink;
  XhtmlParser parser(sink);
  parser.feed(xhtml.data(), xhtml.size());
  parser.finish();
  return sink.text;
}

std::string inflateVector(const uint8_t* data, size_t size, bool* ok = nullptr) {
  std::string input(reinterpret_cast<const char*>(data), size);
  std::unique_ptr<File> file = memoryFile(input);
  std::string out;
  const bool result = inflate(*file, size, [&](const uint8_t* chunk, size_t count) {
    out.append(reinterpret_cast<const char*>(chunk), count);
    return true;
  });
  if (ok != nullptr) *ok = result;
  return out;
}

PageMetrics smallPage(int width, int lines) {
  FakeDisplay display;
  PageMetrics metrics;
  metrics.body.measure(display, TextSize::Small);
  metrics.heading.measure(display, TextSize::Large);
  metrics.width = width;
  metrics.height = lines * metrics.body.lineHeight;
  metrics.paragraphGap = 15;
  return metrics;
}

std::vector<std::string> layoutText(const PageMetrics& metrics, const std::string& xhtml) {
  std::vector<std::string> pages;
  PageLayout layout(metrics, [&](const std::string& page) { pages.push_back(page); });
  XhtmlParser parser(layout);
  parser.feed(xhtml.data(), xhtml.size());
  layout.chapterBreak();
  return pages;
}

// Runs the app's background work until it leaves the indexing screen.
void finishIndexing(EReaderApp& app) {
  for (int i = 0; i < 1000 && app.view() == EReaderApp::View::Indexing; ++i) app.tick();
}

constexpr const char* kBookPath = "/books/book.epub";

struct Fixture {
  Fixture() {
    fs.dirs["/books"] = true;
    fs.files[kBookPath] = makeEpub();
  }
  MemoryFileSystem fs;
  FakeDisplay display;
};

#define ASSERT_VIEW(expected, app) \
  TEST_ASSERT_EQUAL_INT((int)(EReaderApp::View::expected), (int)((app).view()))

}  // namespace

void setUp() {}
void tearDown() {}

void test_fold_keeps_ascii_and_folds_typography() {
  TEST_ASSERT_EQUAL_STRING("A", foldToAscii('A'));
  TEST_ASSERT_EQUAL_STRING("e", foldToAscii(0xE9));
  TEST_ASSERT_EQUAL_STRING("n", foldToAscii(0xF1));
  TEST_ASSERT_EQUAL_STRING("ss", foldToAscii(0xDF));
  TEST_ASSERT_EQUAL_STRING("oe", foldToAscii(0x153));
  TEST_ASSERT_EQUAL_STRING("L", foldToAscii(0x141));
  TEST_ASSERT_EQUAL_STRING("z", foldToAscii(0x17E));
  TEST_ASSERT_EQUAL_STRING("'", foldToAscii(0x2019));
  TEST_ASSERT_EQUAL_STRING("\"", foldToAscii(0x201C));
  TEST_ASSERT_EQUAL_STRING("--", foldToAscii(0x2014));
  TEST_ASSERT_EQUAL_STRING("...", foldToAscii(0x2026));
  TEST_ASSERT_EQUAL_STRING("fi", foldToAscii(0xFB01));
}

void test_fold_drops_invisible_and_marks_unsupported() {
  TEST_ASSERT_EQUAL_STRING("", foldToAscii(0xAD));
  TEST_ASSERT_EQUAL_STRING("", foldToAscii(0x301));
  TEST_ASSERT_EQUAL_STRING("", foldToAscii(0xFEFF));
  TEST_ASSERT_EQUAL_STRING("", foldToAscii(0x07));
  TEST_ASSERT_EQUAL_STRING("?", foldToAscii(0x416));   // Cyrillic
  TEST_ASSERT_EQUAL_STRING("?", foldToAscii(0x4E2D));  // CJK
}

void test_parser_extracts_text_and_structure() {
  TEST_ASSERT_EQUAL_STRING("|h|Title|/h||p|One|br|two|p|",
                           parse("<H1 class=\"a>b\">Title</H1><p>One<br/>two</p>").c_str());
}

void test_parser_skips_non_text_content() {
  const std::string text = parse(
      "<?xml version='1.0'?><!DOCTYPE html><html><head><title>No</title>"
      "<style>p > a { color: red }</style></head><body><!-- no <p> here -->"
      "<svg:svg><svg:title>No</svg:title></svg:svg><script src='x'/>"
      "yes<img src='a.png'/></body></html>");
  TEST_ASSERT_EQUAL_STRING("|p|yes|p|", text.c_str());
}

void test_parser_decodes_entities_and_utf8() {
  TEST_ASSERT_EQUAL_STRING("a & b -- \" A A e",
                           parse("a &amp; b &mdash; &quot; &#65; &#x41; \xC3\xA9").c_str());
  TEST_ASSERT_EQUAL_STRING("AT&T", parse("AT&T").c_str());
  TEST_ASSERT_EQUAL_STRING("a & b", parse("a & b").c_str());
  TEST_ASSERT_EQUAL_STRING("?", parse("&unknownthing;").c_str());
}

void test_parser_handles_data_split_anywhere() {
  const std::string xhtml =
      "<p class='x'>Caf\xC3\xA9 &amp; \xE2\x80\x9C" "bar\xE2\x80\x9D<!-- c --></p><br/>";
  RecordingSink sink;
  XhtmlParser parser(sink);
  for (const char c : xhtml) parser.feed(&c, 1);
  TEST_ASSERT_EQUAL_STRING(parse(xhtml).c_str(), sink.text.c_str());
  TEST_ASSERT_EQUAL_STRING("|p|Cafe & \"bar\"|p||br|", sink.text.c_str());
}

void test_xml_text_to_ascii() {
  TEST_ASSERT_EQUAL_STRING("Fish & Chips", xmlTextToAscii("  Fish &amp;\n  Chips ").c_str());
}

void test_layout_wraps_at_line_width() {
  const auto pages = layoutText(smallPage(80, 10), "<p>aaaa bbbb cccc</p>");
  TEST_ASSERT_EQUAL(1, pages.size());
  TEST_ASSERT_EQUAL_STRING("aaaa\nbbbb\ncccc\n", pages[0].c_str());

  // "aaaa bbbb" is 90 px wide, so it fits exactly with a 90 px line.
  const auto wider = layoutText(smallPage(90, 10), "<p>aaaa bbbb cccc</p>");
  TEST_ASSERT_EQUAL_STRING("aaaa bbbb\ncccc\n", wider[0].c_str());
}

void test_layout_splits_words_wider_than_a_line() {
  const auto pages = layoutText(smallPage(40, 10), "<p>abcdefghij k</p>");
  TEST_ASSERT_EQUAL_STRING("abcd\nefgh\nij k\n", pages[0].c_str());
}

void test_layout_starts_a_new_page_when_full() {
  const auto pages = layoutText(smallPage(40, 2), "one two three four five");
  TEST_ASSERT_EQUAL(3, pages.size());
  TEST_ASSERT_EQUAL_STRING("one\ntwo\n", pages[0].c_str());
  TEST_ASSERT_EQUAL_STRING("thre\ne\n", pages[1].c_str());
  TEST_ASSERT_EQUAL_STRING("four\nfive\n", pages[2].c_str());
}

void test_layout_marks_gaps_and_headings() {
  const auto pages = layoutText(smallPage(100, 10), "<h1>Top</h1><p>one</p><p>two</p>");
  TEST_ASSERT_EQUAL_STRING("\x01Top\n\none\n\ntwo\n", pages[0].c_str());
}

void test_layout_keeps_heading_with_following_text() {
  const auto pages = layoutText(smallPage(100, 3), "<p>a</p><p>b</p><h2>Head</h2><p>c</p>");
  TEST_ASSERT_EQUAL(2, pages.size());
  TEST_ASSERT_EQUAL_STRING("\x01Head\n\nc\n", pages[1].c_str());
}

void test_layout_never_overflows_the_page() {
  const PageMetrics metrics = smallPage(200, 8);
  std::string xhtml;
  for (int i = 0; i < 200; ++i) {
    xhtml += i % 7 == 0 ? "<h2>A heading that wraps over lines</h2>" : "";
    xhtml += "<p>" + std::string(size_t(i % 13) * 3 + 1, 'x') + " word word word word</p>";
  }
  for (const std::string& page : layoutText(metrics, xhtml)) {
    int height = 0;
    size_t start = 0;
    while (start < page.size()) {
      const size_t end = page.find('\n', start);
      height += end == start ? metrics.paragraphGap : metrics.body.lineHeight;
      start = end + 1;
    }
    TEST_ASSERT_LESS_OR_EQUAL(metrics.height, height);
    TEST_ASSERT_NOT_EQUAL('\n', page[0]);
  }
}

void test_layout_chapter_break_skips_empty_chapters() {
  const PageMetrics metrics = smallPage(100, 10);
  std::vector<std::string> pages;
  PageLayout layout(metrics, [&](const std::string& page) { pages.push_back(page); });
  layout.chapterBreak();
  layout.onChar('a');
  layout.chapterBreak();
  layout.chapterBreak();
  layout.onChar('b');
  layout.chapterBreak();
  TEST_ASSERT_EQUAL(2, pages.size());
  TEST_ASSERT_EQUAL(2, layout.pageCount());
}

void test_inflate_block_types() {
  std::string expected;
  for (int i = 0; i < 3; ++i) expected += "The quick brown fox jumps over the lazy dog. ";
  TEST_ASSERT_EQUAL_STRING(expected.c_str(),
                           inflateVector(kStoredDeflate, sizeof(kStoredDeflate)).c_str());
  TEST_ASSERT_EQUAL_STRING(expected.c_str(),
                           inflateVector(kFixedDeflate, sizeof(kFixedDeflate)).c_str());
}

void test_inflate_output_larger_than_the_window() {
  const std::string expected = longDeflateText();
  TEST_ASSERT_EQUAL(kLongDeflateTextSize, expected.size());
  TEST_ASSERT_GREATER_THAN(70000, int(expected.size()));
  const std::string out = inflateVector(kDynamicDeflate, sizeof(kDynamicDeflate));
  TEST_ASSERT_TRUE(out == expected);
}

void test_inflate_rejects_truncated_and_corrupt_data() {
  bool ok = true;
  inflateVector(kDynamicDeflate, sizeof(kDynamicDeflate) / 2, &ok);
  TEST_ASSERT_FALSE(ok);

  const uint8_t reservedBlockType[] = {0x07, 0x00};
  inflateVector(reservedBlockType, sizeof(reservedBlockType), &ok);
  TEST_ASSERT_FALSE(ok);

  const uint8_t badStoredLength[] = {0x01, 0x05, 0x00, 0x00, 0x00, 'h', 'e', 'l', 'l', 'o'};
  inflateVector(badStoredLength, sizeof(badStoredLength), &ok);
  TEST_ASSERT_FALSE(ok);

  // Fuzz: flipped bits must never crash or loop forever, whatever they decode to.
  std::vector<uint8_t> damaged(kDynamicDeflate, kDynamicDeflate + sizeof(kDynamicDeflate));
  for (size_t i = 0; i < damaged.size(); i += 3) {
    damaged[i] ^= uint8_t(1u << (i % 8));
    inflateVector(damaged.data(), damaged.size());
    damaged[i] ^= uint8_t(1u << (i % 8));
  }
}

void test_inflate_stops_when_sink_does() {
  std::string input(reinterpret_cast<const char*>(kDynamicDeflate), sizeof(kDynamicDeflate));
  std::unique_ptr<File> file = memoryFile(input);
  int calls = 0;
  const bool ok = inflate(*file, input.size(), [&](const uint8_t*, size_t) {
    ++calls;
    return false;
  });
  TEST_ASSERT_FALSE(ok);
  TEST_ASSERT_EQUAL(1, calls);
}

void test_zip_reads_stored_and_deflated_entries() {
  ZipBuilder builder;
  builder.addStored("a.txt", "hello");
  builder.addDeflated("b.txt",
                      std::string(reinterpret_cast<const char*>(kDynamicDeflate),
                                  sizeof(kDynamicDeflate)),
                      kLongDeflateTextSize);
  std::string data = builder.finish();

  ZipArchive zip;
  TEST_ASSERT_TRUE(zip.open(memoryFile(data)));
  TEST_ASSERT_NULL(zip.find("missing"));

  std::string text;
  TEST_ASSERT_TRUE(zip.readToString(*zip.find("a.txt"), text, 100));
  TEST_ASSERT_EQUAL_STRING("hello", text.c_str());
  TEST_ASSERT_TRUE(zip.readToString(*zip.find("b.txt"), text, 1000000));
  TEST_ASSERT_TRUE(text == longDeflateText());
  TEST_ASSERT_FALSE(zip.readToString(*zip.find("b.txt"), text, 100));
}

void test_zip_rejects_wrong_size_and_non_zip_files() {
  ZipBuilder builder;
  builder.addDeflated("short.txt",
                      std::string(reinterpret_cast<const char*>(kFixedDeflate),
                                  sizeof(kFixedDeflate)),
                      5);
  std::string data = builder.finish();
  ZipArchive zip;
  TEST_ASSERT_TRUE(zip.open(memoryFile(data)));
  std::string text;
  TEST_ASSERT_FALSE(zip.readToString(*zip.find("short.txt"), text, 1000));

  std::string garbage(4096, 'x');
  TEST_ASSERT_FALSE(zip.open(memoryFile(garbage)));
  std::string tiny = "PK";
  TEST_ASSERT_FALSE(zip.open(memoryFile(tiny)));
}

void test_resolve_href() {
  TEST_ASSERT_EQUAL_STRING("OEBPS/a.xhtml", resolveHref("OEBPS/c.opf", "a.xhtml").c_str());
  TEST_ASSERT_EQUAL_STRING("a.xhtml", resolveHref("c.opf", "a.xhtml").c_str());
  TEST_ASSERT_EQUAL_STRING("x/b.xhtml", resolveHref("OEBPS/c.opf", "../x/./b.xhtml").c_str());
  TEST_ASSERT_EQUAL_STRING("OEBPS/my book.xhtml",
                           resolveHref("OEBPS/c.opf", "my%20book.xhtml#ch1").c_str());
}

void test_epub_reads_metadata_and_spine_order() {
  std::string data = makeEpub(2);
  Epub epub;
  TEST_ASSERT_TRUE(epub.open(memoryFile(data)));
  TEST_ASSERT_EQUAL_STRING("Fish & Chips", epub.title().c_str());
  TEST_ASSERT_EQUAL_STRING("Ann Author", epub.author().c_str());

  // Cover image, non-linear note and the missing file are all left out.
  TEST_ASSERT_EQUAL(3, epub.chapterCount());
  std::string chapters[3];
  for (size_t i = 0; i < 3; ++i) {
    TEST_ASSERT_TRUE(epub.readChapter(i, [&](const uint8_t* bytes, size_t size) {
      chapters[i].append(reinterpret_cast<const char*>(bytes), size);
      return true;
    }));
  }
  TEST_ASSERT_NOT_EQUAL(std::string::npos, chapters[0].find("alpha"));
  TEST_ASSERT_NOT_EQUAL(std::string::npos, chapters[1].find("bravo"));
  TEST_ASSERT_NOT_EQUAL(std::string::npos, chapters[2].find("Deflated"));
}

void test_epub_rejects_missing_parts() {
  ZipBuilder noContainer;
  noContainer.addStored("mimetype", "application/epub+zip");
  std::string a = noContainer.finish();
  Epub epub;
  TEST_ASSERT_FALSE(epub.open(memoryFile(a)));

  ZipBuilder noPackage;
  noPackage.addStored("META-INF/container.xml", kContainer);
  std::string b = noPackage.finish();
  TEST_ASSERT_FALSE(epub.open(memoryFile(b)));

  ZipBuilder emptySpine;
  emptySpine.addStored("META-INF/container.xml", kContainer);
  emptySpine.addStored("OEBPS/content.opf", "<package><spine></spine></package>");
  std::string c = emptySpine.finish();
  TEST_ASSERT_FALSE(epub.open(memoryFile(c)));
}

void test_pages_remaining() {
  TEST_ASSERT_EQUAL(339, pagesRemaining(0, 340));
  TEST_ASSERT_EQUAL(1, pagesRemaining(338, 340));
  TEST_ASSERT_EQUAL(0, pagesRemaining(339, 340));
  TEST_ASSERT_EQUAL(0, pagesRemaining(500, 340));
  TEST_ASSERT_EQUAL(0, pagesRemaining(0, 0));
}

void test_library_lists_only_epubs_sorted_by_title() {
  Fixture f;
  f.fs.files.clear();
  f.fs.files["/books/zebra.epub"] = "x";
  f.fs.files["/books/Apple.EPUB"] = "x";
  f.fs.files["/books/mango.epub"] = "x";
  f.fs.files["/books/notes.txt"] = "x";
  f.fs.files["/books/._mango.epub"] = "x";
  f.fs.files["/books/.epub"] = "x";
  f.fs.files["/other/outside.epub"] = "x";

  EReaderApp app(f.fs, f.display);
  app.onOpen();
  TEST_ASSERT_EQUAL(3, app.books().size());
  TEST_ASSERT_EQUAL_STRING("Apple", app.books()[0].title.c_str());
  TEST_ASSERT_EQUAL_STRING("mango", app.books()[1].title.c_str());
  TEST_ASSERT_EQUAL_STRING("zebra", app.books()[2].title.c_str());

  app.draw(f.display);
  TEST_ASSERT_TRUE(f.display.shows("Books"));
  TEST_ASSERT_TRUE(f.display.shows("Apple"));
  TEST_ASSERT_TRUE(f.display.shows("Not indexed yet"));

  TEST_ASSERT_TRUE(app.onInput(InputEvent::Previous));  // wraps to the end
  TEST_ASSERT_EQUAL(2, app.selectedIndex());
  TEST_ASSERT_TRUE(app.onInput(InputEvent::Next));
  TEST_ASSERT_EQUAL(0, app.selectedIndex());
}

void test_library_scrolls_a_screen_at_a_time() {
  Fixture f;
  f.fs.files.clear();
  for (int i = 10; i < 40; ++i) f.fs.files["/books/" + std::to_string(i) + ".epub"] = "x";
  EReaderApp app(f.fs, f.display);
  app.onOpen();
  app.draw(f.display);
  TEST_ASSERT_TRUE(f.display.shows("10"));
  TEST_ASSERT_FALSE(f.display.shows("39"));
  for (int i = 0; i < 29; ++i) app.onInput(InputEvent::Next);
  f.display.beginFrame(Color::White);
  app.draw(f.display);
  TEST_ASSERT_TRUE(f.display.shows("39"));
  TEST_ASSERT_FALSE(f.display.shows("10"));
}

void test_library_explains_a_missing_folder_or_no_books() {
  MemoryFileSystem fs;
  FakeDisplay display;
  EReaderApp app(fs, display);
  app.onOpen();
  app.draw(display);
  TEST_ASSERT_TRUE(display.shows("No /books folder"));

  fs.dirs["/books"] = true;
  app.onOpen();
  display.beginFrame(Color::White);
  app.draw(display);
  TEST_ASSERT_TRUE(display.shows("No books found"));
  TEST_ASSERT_FALSE(app.onInput(InputEvent::Select));
  TEST_ASSERT_FALSE(app.onInput(InputEvent::Back));  // shell returns home
}

void test_first_open_indexes_then_shows_the_first_page() {
  Fixture f;
  EReaderApp app(f.fs, f.display);
  app.onOpen();
  TEST_ASSERT_TRUE(app.onInput(InputEvent::Select));
  ASSERT_VIEW(Indexing, app);
  app.draw(f.display);
  TEST_ASSERT_TRUE(f.display.shows("Indexing book"));

  finishIndexing(app);
  ASSERT_VIEW(Reading, app);
  TEST_ASSERT_EQUAL(0, app.currentPage());
  TEST_ASSERT_GREATER_THAN(2, int(app.pageCount()));
  TEST_ASSERT_EQUAL(app.pageCount() - 1, app.pagesLeft());

  TEST_ASSERT_EQUAL(1, f.fs.countFiles(".idx"));
  TEST_ASSERT_EQUAL(0, f.fs.countFiles(".tmp"));

  f.display.beginFrame(Color::White);
  app.draw(f.display);
  TEST_ASSERT_TRUE(f.display.shows("Chapter"));
  TEST_ASSERT_TRUE(f.display.showsPart("alpha alpha"));
  const std::string position = "Page 1 of " + std::to_string(app.pageCount());
  TEST_ASSERT_TRUE(f.display.shows(position));
  const std::string left = std::to_string(app.pagesLeft()) + " pages left";
  TEST_ASSERT_TRUE(f.display.shows(left));
}

void test_second_open_uses_the_cache_without_touching_the_epub() {
  Fixture f;
  {
    EReaderApp app(f.fs, f.display);
    app.onOpen();
    app.onInput(InputEvent::Select);
    finishIndexing(app);
  }
  const uint32_t size = uint32_t(f.fs.files[kBookPath].size());
  f.fs.files[kBookPath] = std::string(size, 'x');  // unreadable, but same name and size
  f.fs.opens.clear();

  EReaderApp app(f.fs, f.display);
  app.onOpen();
  TEST_ASSERT_TRUE(app.onInput(InputEvent::Select));
  ASSERT_VIEW(Reading, app);
  TEST_ASSERT_EQUAL(0, f.fs.opens[kBookPath]);
  TEST_ASSERT_EQUAL_STRING("Fish & Chips", app.books()[0].title.c_str());
}

void test_every_page_of_every_chapter_can_be_read() {
  Fixture f;
  EReaderApp app(f.fs, f.display);
  app.onOpen();
  app.onInput(InputEvent::Select);
  finishIndexing(app);

  std::string seen;
  const uint32_t total = app.pageCount();
  for (uint32_t i = 0; i < total; ++i) {
    f.display.beginFrame(Color::White);
    app.draw(f.display);
    for (const std::string& text : f.display.texts) seen += text + "\n";
    TEST_ASSERT_EQUAL(i, app.currentPage());
    TEST_ASSERT_EQUAL(i + 1 < total, app.onInput(InputEvent::Next));
  }
  TEST_ASSERT_EQUAL(0, app.pagesLeft());
  TEST_ASSERT_TRUE(seen.find("alpha") < seen.find("bravo"));
  TEST_ASSERT_TRUE(seen.find("bravo") < seen.find("Deflated"));
  TEST_ASSERT_NOT_EQUAL(std::string::npos, seen.find("Cafe \"quoted\" text."));
  TEST_ASSERT_EQUAL(std::string::npos, seen.find("note"));
  TEST_ASSERT_EQUAL(std::string::npos, seen.find("Hidden"));
  TEST_ASSERT_NOT_EQUAL(std::string::npos, seen.find("Last page"));
}

void test_page_turns_stop_at_both_ends() {
  Fixture f;
  EReaderApp app(f.fs, f.display);
  app.onOpen();
  app.onInput(InputEvent::Select);
  finishIndexing(app);

  TEST_ASSERT_FALSE(app.onInput(InputEvent::Previous));
  TEST_ASSERT_EQUAL(0, app.currentPage());
  TEST_ASSERT_TRUE(app.onInput(InputEvent::Next));
  TEST_ASSERT_EQUAL(1, app.currentPage());
  TEST_ASSERT_TRUE(app.onInput(InputEvent::Previous));
  TEST_ASSERT_EQUAL(0, app.currentPage());
  TEST_ASSERT_FALSE(app.onInput(InputEvent::Select));
}

void test_last_page_is_remembered_across_restarts() {
  Fixture f;
  uint32_t total = 0;
  {
    EReaderApp app(f.fs, f.display);
    app.onOpen();
    app.onInput(InputEvent::Select);
    finishIndexing(app);
    for (int i = 0; i < 2; ++i) app.onInput(InputEvent::Next);
    total = app.pageCount();
    // No Back: the device can be switched off at any time.
  }

  EReaderApp app(f.fs, f.display);
  app.onOpen();
  TEST_ASSERT_EQUAL(2, app.books()[0].page);
  TEST_ASSERT_EQUAL(total, app.books()[0].pageCount);
  app.draw(f.display);
  const std::string progress =
      std::to_string(total - 3) + " of " + std::to_string(total) + " pages left";
  TEST_ASSERT_TRUE(f.display.shows(progress));

  app.onInput(InputEvent::Select);
  ASSERT_VIEW(Reading, app);
  TEST_ASSERT_EQUAL(2, app.currentPage());
  TEST_ASSERT_EQUAL(total - 3, app.pagesLeft());
}

void test_back_returns_to_library_and_updates_progress() {
  Fixture f;
  EReaderApp app(f.fs, f.display);
  app.onOpen();
  app.onInput(InputEvent::Select);
  finishIndexing(app);
  app.onInput(InputEvent::Next);

  TEST_ASSERT_TRUE(app.onInput(InputEvent::Back));  // consumed: stays in the app
  ASSERT_VIEW(Library, app);
  TEST_ASSERT_EQUAL_STRING("Fish & Chips", app.books()[0].title.c_str());
  TEST_ASSERT_EQUAL(1, app.books()[0].page);
  TEST_ASSERT_FALSE(app.onInput(InputEvent::Back));  // leaves the app
}

void test_finished_book_shows_as_finished() {
  Fixture f;
  EReaderApp app(f.fs, f.display);
  app.onOpen();
  app.onInput(InputEvent::Select);
  finishIndexing(app);
  while (app.pagesLeft() > 0) app.onInput(InputEvent::Next);
  app.onInput(InputEvent::Back);
  f.display.beginFrame(Color::White);
  app.draw(f.display);
  TEST_ASSERT_TRUE(f.display.showsPart("Finished"));
}

void test_saved_position_past_the_end_is_clamped() {
  Fixture f;
  EReaderApp app(f.fs, f.display);
  app.onOpen();
  app.onInput(InputEvent::Select);
  finishIndexing(app);
  const std::string stem = app.books()[0].stem;
  const uint32_t total = app.pageCount();
  app.onInput(InputEvent::Back);

  savePosition(f.fs, stem, total + 50);
  app.onOpen();
  app.onInput(InputEvent::Select);
  TEST_ASSERT_EQUAL(total - 1, app.currentPage());
}

void test_cancel_leaves_no_index_behind() {
  Fixture f;
  EReaderApp app(f.fs, f.display);
  app.onOpen();
  app.onInput(InputEvent::Select);
  app.tick();
  TEST_ASSERT_EQUAL(1, f.fs.countFiles(".tmp"));

  TEST_ASSERT_TRUE(app.onInput(InputEvent::Back));
  ASSERT_VIEW(Library, app);
  TEST_ASSERT_EQUAL(0, f.fs.countFiles(".tmp"));
  TEST_ASSERT_EQUAL(0, f.fs.countFiles(".idx"));

  app.onInput(InputEvent::Select);
  finishIndexing(app);
  ASSERT_VIEW(Reading, app);
}

void test_interrupted_index_is_ignored_and_rebuilt() {
  Fixture f;
  std::string stem;
  {
    EReaderApp app(f.fs, f.display);
    app.onOpen();
    stem = app.books()[0].stem;
    app.onInput(InputEvent::Select);
    app.tick();
    f.fs.files[stem + ".idx"] = "half a file from a power cut, padded out past the footer size" +
                                std::string(300, '?');
  }  // the destructor stands in for a reboot, apart from removing the .tmp

  EReaderApp app(f.fs, f.display);
  app.onOpen();
  TEST_ASSERT_EQUAL(0, app.books()[0].pageCount);
  app.onInput(InputEvent::Select);
  ASSERT_VIEW(Indexing, app);
  finishIndexing(app);
  ASSERT_VIEW(Reading, app);
}

void test_changed_layout_reindexes_and_keeps_the_place() {
  Fixture f;
  uint32_t oldTotal = 0;
  {
    EReaderApp app(f.fs, f.display);
    app.onOpen();
    app.onInput(InputEvent::Select);
    finishIndexing(app);
    app.onInput(InputEvent::Next);
    oldTotal = app.pageCount();
  }

  FakeDisplay narrower(300, 960);
  EReaderApp app(f.fs, narrower);
  app.onOpen();
  app.onInput(InputEvent::Select);
  ASSERT_VIEW(Indexing, app);
  finishIndexing(app);
  ASSERT_VIEW(Reading, app);
  TEST_ASSERT_NOT_EQUAL(oldTotal, app.pageCount());
  TEST_ASSERT_EQUAL(1, app.currentPage());
  TEST_ASSERT_EQUAL(1, f.fs.countFiles(".idx"));
}

void test_replacing_a_book_starts_it_over() {
  Fixture f;
  {
    EReaderApp app(f.fs, f.display);
    app.onOpen();
    app.onInput(InputEvent::Select);
    finishIndexing(app);
    app.onInput(InputEvent::Next);
  }
  f.fs.files[kBookPath] = makeEpub(60);

  EReaderApp app(f.fs, f.display);
  app.onOpen();
  TEST_ASSERT_EQUAL(0, app.books()[0].pageCount);
  app.onInput(InputEvent::Select);
  ASSERT_VIEW(Indexing, app);
  finishIndexing(app);
  TEST_ASSERT_EQUAL(0, app.currentPage());
}

void test_bad_books_show_an_error_and_recover() {
  Fixture f;
  f.fs.files[kBookPath] = "this is not a zip file";
  EReaderApp app(f.fs, f.display);
  app.onOpen();
  app.onInput(InputEvent::Select);
  ASSERT_VIEW(Error, app);
  app.draw(f.display);
  TEST_ASSERT_TRUE(f.display.shows("This file is not a readable EPUB"));
  TEST_ASSERT_TRUE(app.onInput(InputEvent::Back));
  ASSERT_VIEW(Library, app);
}

void test_corrupt_chapter_fails_indexing_cleanly() {
  Fixture f;
  ZipBuilder zip;
  zip.addStored("META-INF/container.xml", kContainer);
  zip.addStored("OEBPS/content.opf",
                "<package><manifest><item id='a' href='a.xhtml' "
                "media-type='application/xhtml+xml'/></manifest>"
                "<spine><itemref idref='a'/></spine></package>");
  zip.addDeflated("OEBPS/a.xhtml", std::string(100, char(0xFF)), 500);
  f.fs.files[kBookPath] = zip.finish();

  EReaderApp app(f.fs, f.display);
  app.onOpen();
  app.onInput(InputEvent::Select);
  finishIndexing(app);
  ASSERT_VIEW(Error, app);
  TEST_ASSERT_EQUAL(0, f.fs.countFiles(".tmp"));
  TEST_ASSERT_EQUAL(0, f.fs.countFiles(".idx"));
}

void test_book_without_text_fails_cleanly() {
  Fixture f;
  ZipBuilder zip;
  zip.addStored("META-INF/container.xml", kContainer);
  zip.addStored("OEBPS/content.opf",
                "<package><manifest><item id='a' href='a.xhtml' "
                "media-type='application/xhtml+xml'/></manifest>"
                "<spine><itemref idref='a'/></spine></package>");
  zip.addStored("OEBPS/a.xhtml", "<html><body><img src='page.jpg'/></body></html>");
  f.fs.files[kBookPath] = zip.finish();

  EReaderApp app(f.fs, f.display);
  app.onOpen();
  app.onInput(InputEvent::Select);
  finishIndexing(app);
  ASSERT_VIEW(Error, app);
  app.draw(f.display);
  TEST_ASSERT_TRUE(f.display.shows("This book has no readable text"));
}

void test_full_sd_card_fails_indexing_cleanly() {
  Fixture f;
  EReaderApp app(f.fs, f.display);
  app.onOpen();
  f.fs.failWrites = true;
  app.onInput(InputEvent::Select);
  finishIndexing(app);
  ASSERT_VIEW(Error, app);
  TEST_ASSERT_EQUAL(0, f.fs.countFiles(".idx"));
}

void test_quality_refresh_clears_ghosting_periodically() {
  Fixture f;
  f.fs.files[kBookPath] = makeEpub(200);
  Shell shell;
  EReaderApp app(f.fs, f.display);
  shell.addApp(&app);
  shell.draw(f.display);
  shell.handle(InputEvent::Select);  // opens the library
  shell.draw(f.display);
  shell.handle(InputEvent::Select);  // opens the book
  while (app.view() == EReaderApp::View::Indexing) {
    if (shell.tick()) shell.draw(f.display);
  }
  TEST_ASSERT_TRUE(f.display.lastRefresh == Refresh::Quality);

  std::vector<bool> quality;
  for (int i = 0; i < 16; ++i) {
    TEST_ASSERT_TRUE(shell.handle(InputEvent::Next));
    shell.draw(f.display);
    quality.push_back(f.display.lastRefresh == Refresh::Quality);
  }
  TEST_ASSERT_EQUAL(2, std::count(quality.begin(), quality.end(), true));
  TEST_ASSERT_TRUE(quality[7]);
  TEST_ASSERT_TRUE(quality[15]);
}

void test_long_title_is_shortened_to_fit() {
  Fixture f;
  f.fs.files.clear();
  const std::string name(200, 'a');
  f.fs.files["/books/" + name + ".epub"] = "x";
  EReaderApp app(f.fs, f.display);
  app.onOpen();
  app.draw(f.display);
  TEST_ASSERT_TRUE(f.display.showsPart("..."));
  for (const std::string& text : f.display.texts) {
    TEST_ASSERT_LESS_OR_EQUAL(540, f.display.textWidth(text.c_str(), TextSize::Medium));
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_fold_keeps_ascii_and_folds_typography);
  RUN_TEST(test_fold_drops_invisible_and_marks_unsupported);
  RUN_TEST(test_parser_extracts_text_and_structure);
  RUN_TEST(test_parser_skips_non_text_content);
  RUN_TEST(test_parser_decodes_entities_and_utf8);
  RUN_TEST(test_parser_handles_data_split_anywhere);
  RUN_TEST(test_xml_text_to_ascii);
  RUN_TEST(test_layout_wraps_at_line_width);
  RUN_TEST(test_layout_splits_words_wider_than_a_line);
  RUN_TEST(test_layout_starts_a_new_page_when_full);
  RUN_TEST(test_layout_marks_gaps_and_headings);
  RUN_TEST(test_layout_keeps_heading_with_following_text);
  RUN_TEST(test_layout_never_overflows_the_page);
  RUN_TEST(test_layout_chapter_break_skips_empty_chapters);
  RUN_TEST(test_inflate_block_types);
  RUN_TEST(test_inflate_output_larger_than_the_window);
  RUN_TEST(test_inflate_rejects_truncated_and_corrupt_data);
  RUN_TEST(test_inflate_stops_when_sink_does);
  RUN_TEST(test_zip_reads_stored_and_deflated_entries);
  RUN_TEST(test_zip_rejects_wrong_size_and_non_zip_files);
  RUN_TEST(test_resolve_href);
  RUN_TEST(test_epub_reads_metadata_and_spine_order);
  RUN_TEST(test_epub_rejects_missing_parts);
  RUN_TEST(test_pages_remaining);
  RUN_TEST(test_library_lists_only_epubs_sorted_by_title);
  RUN_TEST(test_library_scrolls_a_screen_at_a_time);
  RUN_TEST(test_library_explains_a_missing_folder_or_no_books);
  RUN_TEST(test_first_open_indexes_then_shows_the_first_page);
  RUN_TEST(test_second_open_uses_the_cache_without_touching_the_epub);
  RUN_TEST(test_every_page_of_every_chapter_can_be_read);
  RUN_TEST(test_page_turns_stop_at_both_ends);
  RUN_TEST(test_last_page_is_remembered_across_restarts);
  RUN_TEST(test_back_returns_to_library_and_updates_progress);
  RUN_TEST(test_finished_book_shows_as_finished);
  RUN_TEST(test_saved_position_past_the_end_is_clamped);
  RUN_TEST(test_cancel_leaves_no_index_behind);
  RUN_TEST(test_interrupted_index_is_ignored_and_rebuilt);
  RUN_TEST(test_changed_layout_reindexes_and_keeps_the_place);
  RUN_TEST(test_replacing_a_book_starts_it_over);
  RUN_TEST(test_bad_books_show_an_error_and_recover);
  RUN_TEST(test_corrupt_chapter_fails_indexing_cleanly);
  RUN_TEST(test_book_without_text_fails_cleanly);
  RUN_TEST(test_full_sd_card_fails_indexing_cleanly);
  RUN_TEST(test_quality_refresh_clears_ghosting_periodically);
  RUN_TEST(test_long_title_is_shortened_to_fit);
  return UNITY_END();
}

// Host-side tests for the WebDAV client and the Sync app. Run with:
// pio test -e native
#include <unity.h>

#include <paperos.h>
#include <SyncApp.h>

#include <cstring>
#include <string>
#include <vector>

using namespace paperos;

namespace {

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
    for (const auto& t : texts)
      if (t == text) return true;
    return false;
  }
  std::vector<std::string> texts;
};

class FakeBattery : public Battery {
 public:
  int level() const override { return 80; }
};

class FakeClock : public Clock {
 public:
  int64_t now() const override { return 1700000000; }
  uint32_t uptimeMs() const override { return ms; }
  uint32_t ms = 0;
};

class FakeResponse : public HttpResponse {
 public:
  explicit FakeResponse(int status) : status_(status) {}
  int read(void*, size_t) override { return kEnd; }
  int64_t contentLength() const override { return 0; }
  int status() const override { return status_; }

 private:
  int status_;
};

class FakeNetwork : public Network {
 public:
  bool configured() const override { return isConfigured; }
  void connect() override {
    ++connects;
    state_ = NetworkState::Connecting;
    polls_ = 0;
  }
  void disconnect() override {
    ++disconnects;
    state_ = NetworkState::Off;
  }
  std::string failureDetail() const override { return "reason 201 NO_AP_FOUND"; }
  NetworkState state() override {
    if (state_ == NetworkState::Connecting && ++polls_ >= 3) {
      state_ = joinFails ? NetworkState::Failed : NetworkState::Connected;
    }
    return state_;
  }
  std::unique_ptr<HttpResponse> get(const char*, std::string& error) override {
    error = "unexpected";
    return nullptr;
  }
  std::unique_ptr<HttpResponse> send(const HttpRequest& request, std::string& error) override {
    requests.push_back(request);
    if (status == 0) {
      error = "connection refused";
      return nullptr;
    }
    return std::unique_ptr<HttpResponse>(new FakeResponse(status));
  }

  std::string header(size_t index, const char* name) const {
    for (const auto& h : requests.at(index).headers)
      if (h.first == name) return h.second;
    return "";
  }

  bool isConfigured = true;
  bool joinFails = false;
  int status = 207;  // 0: no response at all
  int connects = 0;
  int disconnects = 0;
  std::vector<HttpRequest> requests;

 private:
  NetworkState state_ = NetworkState::Off;
  int polls_ = 0;
};

const char* kConfig =
    "[sync]\n"
    "  url = \"https://cloud.example.com/remote.php/dav/files/dan/mindwtr/\"\n"
    "  username = \"dan\"\n"
    "  password = \"secret\"\n";

struct Harness {
  explicit Harness(const char* text = kConfig) { config.parse(text); }
  // Ticks until the app goes idle, at most `limit` times.
  void runUntilIdle(int limit = 20) {
    for (int i = 0; i < limit && app.phase() != SyncApp::Phase::Idle; ++i) app.tick();
  }
  Config config;
  FakeNetwork network;
  FakeClock clock;
  FakeBattery battery;
  FakeDisplay display;
  SyncApp app{config, network, clock, battery};
};

}  // namespace

void setUp() {}
void tearDown() {}

// --- WebDAV client ---------------------------------------------------------

void test_base64_encodes_with_padding() {
  TEST_ASSERT_EQUAL_STRING("", base64Encode("").c_str());
  TEST_ASSERT_EQUAL_STRING("Zg==", base64Encode("f").c_str());
  TEST_ASSERT_EQUAL_STRING("Zm8=", base64Encode("fo").c_str());
  TEST_ASSERT_EQUAL_STRING("Zm9v", base64Encode("foo").c_str());
  TEST_ASSERT_EQUAL_STRING("Zm9vYmFy", base64Encode("foobar").c_str());
  TEST_ASSERT_EQUAL_STRING("QWxhZGRpbjpvcGVuIHNlc2FtZQ==",
                           base64Encode("Aladdin:open sesame").c_str());
  TEST_ASSERT_EQUAL_STRING("/w==", base64Encode("\xff").c_str());
}

void test_settings_read_from_config() {
  Config config;
  config.parse(kConfig);
  const DavSettings settings = DavSettings::fromConfig(config);
  TEST_ASSERT_TRUE(settings.configured());
  TEST_ASSERT_EQUAL_STRING("dan", settings.username.c_str());
  TEST_ASSERT_EQUAL_STRING("secret", settings.password.c_str());

  config.parse("[sync]\nurl = \"webcal://example.com/dav/\"\n");
  TEST_ASSERT_EQUAL_STRING("https://example.com/dav/",
                           DavSettings::fromConfig(config).url.c_str());
  config.parse("");
  TEST_ASSERT_FALSE(DavSettings::fromConfig(config).configured());
}

void test_check_sends_an_authenticated_propfind() {
  FakeNetwork network;
  DavSettings settings;
  settings.url = "https://example.com/dav/";
  settings.username = "Aladdin";
  settings.password = "open sesame";
  DavClient client(network, settings);
  std::string message;
  TEST_ASSERT_EQUAL(int(DavCheck::Ok), int(client.checkCredentials(message)));
  TEST_ASSERT_EQUAL_STRING("", message.c_str());
  TEST_ASSERT_EQUAL(1, int(network.requests.size()));
  TEST_ASSERT_EQUAL_STRING("PROPFIND", network.requests[0].method.c_str());
  TEST_ASSERT_EQUAL_STRING("https://example.com/dav/", network.requests[0].url.c_str());
  TEST_ASSERT_EQUAL_STRING("0", network.header(0, "Depth").c_str());
  TEST_ASSERT_EQUAL_STRING("Basic QWxhZGRpbjpvcGVuIHNlc2FtZQ==",
                           network.header(0, "Authorization").c_str());
  TEST_ASSERT_TRUE(network.requests[0].body.find("propfind") != std::string::npos);
}

void test_check_without_credentials_sends_no_authorization() {
  FakeNetwork network;
  DavSettings settings;
  settings.url = "https://example.com/dav/";
  DavClient client(network, settings);
  std::string message;
  client.checkCredentials(message);
  TEST_ASSERT_EQUAL_STRING("", network.header(0, "Authorization").c_str());
}

void test_check_explains_each_failure() {
  struct Case {
    int status;
    DavCheck result;
    const char* message;
  };
  const Case cases[] = {
      {401, DavCheck::BadCredentials, "Wrong username or password"},
      {403, DavCheck::Forbidden, "Not allowed to open this folder"},
      {404, DavCheck::NotFound, "Folder not found, check the url"},
      {200, DavCheck::NotDav, "Not a WebDAV address, check the url"},
      {405, DavCheck::NotDav, "Not a WebDAV address, check the url"},
      {301, DavCheck::Failed, "The server moved it (HTTP 301), check the url"},
      {500, DavCheck::Failed, "Server answered HTTP 500"},
      {0, DavCheck::Failed, "connection refused"},
  };
  for (const Case& c : cases) {
    FakeNetwork network;
    network.status = c.status;
    DavSettings settings;
    settings.url = "https://example.com/dav/";
    DavClient client(network, settings);
    std::string message;
    TEST_ASSERT_EQUAL(int(c.result), int(client.checkCredentials(message)));
    TEST_ASSERT_EQUAL_STRING(c.message, message.c_str());
  }
}

void test_check_needs_a_url() {
  FakeNetwork network;
  DavClient client(network, DavSettings{});
  std::string message;
  TEST_ASSERT_EQUAL(int(DavCheck::NotConfigured), int(client.checkCredentials(message)));
  TEST_ASSERT_EQUAL(0, int(network.requests.size()));
}

void test_file_urls_join_the_folder() {
  FakeNetwork network;
  DavSettings settings;
  settings.url = "https://example.com/dav";
  DavClient client(network, settings);
  TEST_ASSERT_EQUAL_STRING("https://example.com/dav/data.json", client.fileUrl("data.json").c_str());
  TEST_ASSERT_EQUAL_STRING("https://example.com/dav/data.json",
                           client.fileUrl("/data.json").c_str());
  settings.url = "https://example.com/dav/";
  DavClient slash(network, settings);
  TEST_ASSERT_EQUAL_STRING("https://example.com/dav/a/b.json", slash.fileUrl("a/b.json").c_str());
}

void test_get_file_is_authenticated() {
  FakeNetwork network;
  network.status = 200;
  DavSettings settings;
  settings.url = "https://example.com/dav/";
  settings.username = "u";
  settings.password = "p";
  DavClient client(network, settings);
  std::string error;
  TEST_ASSERT_NOT_NULL(client.getFile("data.json", error).get());
  TEST_ASSERT_EQUAL_STRING("GET", network.requests[0].method.c_str());
  TEST_ASSERT_EQUAL_STRING("https://example.com/dav/data.json", network.requests[0].url.c_str());
  TEST_ASSERT_EQUAL_STRING("Basic dTpw", network.header(0, "Authorization").c_str());

  network.status = 404;
  TEST_ASSERT_NULL(client.getFile("data.json", error).get());
  TEST_ASSERT_EQUAL_STRING("data.json not found", error.c_str());
}

void test_origin_etag_undoes_proxy_changes() {
  TEST_ASSERT_EQUAL_STRING("\"abc\"", originEtag("W/\"abc-gzip\"").c_str());
  TEST_ASSERT_EQUAL_STRING("\"abc\"", originEtag("\"abc-br\"").c_str());
  TEST_ASSERT_EQUAL_STRING("\"abc\"", originEtag("W/\"abc\"").c_str());
  TEST_ASSERT_EQUAL_STRING("\"abc\"", originEtag("\"abc\"").c_str());
  TEST_ASSERT_EQUAL_STRING("abc", originEtag("abc-deflate").c_str());
  TEST_ASSERT_EQUAL_STRING("\"-gzip\"", originEtag("\"-gzip\"").c_str());
  TEST_ASSERT_EQUAL_STRING("", originEtag("").c_str());
}

void test_put_sends_the_origin_etag() {
  FakeNetwork network;
  network.status = 204;
  DavSettings settings;
  settings.url = "https://example.com/dav/";
  DavClient client(network, settings);
  std::string error;
  TEST_ASSERT_EQUAL(int(DavWrite::Ok), int(client.putFile("data.json", "{}",
                                                          "W/\"85606f-gzip\"",
                                                          "application/json", error)));
  TEST_ASSERT_EQUAL_STRING("PUT", network.requests[0].method.c_str());
  TEST_ASSERT_EQUAL_STRING("\"85606f\"", network.header(0, "If-Match").c_str());
  network.status = 412;
  TEST_ASSERT_EQUAL(int(DavWrite::Conflict),
                    int(client.putFile("data.json", "{}", "\"x\"", "application/json", error)));
}

// --- Sync app --------------------------------------------------------------

void test_app_signs_in_on_open() {
  Harness h;
  h.app.onOpen();
  TEST_ASSERT_EQUAL(1, h.network.connects);
  TEST_ASSERT_EQUAL_STRING("Connecting to Wi-Fi...", h.app.headline().c_str());
  h.runUntilIdle();
  TEST_ASSERT_TRUE(h.app.checked());
  TEST_ASSERT_EQUAL(int(DavCheck::Ok), int(h.app.result()));
  TEST_ASSERT_EQUAL(1, int(h.network.requests.size()));
  TEST_ASSERT_EQUAL(1, h.network.disconnects);  // Wi-Fi off again afterwards

  h.app.draw(h.display);
  TEST_ASSERT_TRUE(h.display.shows("Signed in"));
  TEST_ASSERT_TRUE(h.display.shows("as dan"));
  TEST_ASSERT_TRUE(h.display.shows("Server: cloud.example.com"));
  TEST_ASSERT_TRUE(h.display.shows("User: dan"));
}

void test_app_shows_signing_in_before_the_request() {
  Harness h;
  h.app.onOpen();
  while (h.app.phase() == SyncApp::Phase::Connecting) h.app.tick();
  TEST_ASSERT_EQUAL(int(SyncApp::Phase::Checking), int(h.app.phase()));
  TEST_ASSERT_EQUAL(0, int(h.network.requests.size()));
  TEST_ASSERT_EQUAL_STRING("Signing in...", h.app.headline().c_str());
}

void test_app_reports_wrong_credentials() {
  Harness h;
  h.network.status = 401;
  h.app.onOpen();
  h.runUntilIdle();
  h.app.draw(h.display);
  TEST_ASSERT_TRUE(h.display.shows("Could not sign in"));
  TEST_ASSERT_TRUE(h.display.shows("Wrong username or password"));
}

void test_app_reports_wifi_failure() {
  Harness h;
  h.network.joinFails = true;
  h.app.onOpen();
  h.runUntilIdle();
  TEST_ASSERT_EQUAL(0, int(h.network.requests.size()));
  TEST_ASSERT_EQUAL_STRING("Could not connect to Wi-Fi", h.app.detail().c_str());
}

void test_app_gives_up_joining_after_a_timeout() {
  Harness h;
  h.app.onOpen();
  h.clock.ms = SyncApp::kConnectTimeoutMs + 1;  // before the fake's 3rd poll joins
  h.app.tick();
  TEST_ASSERT_EQUAL(int(SyncApp::Phase::Idle), int(h.app.phase()));
  TEST_ASSERT_EQUAL_STRING("Could not connect to Wi-Fi", h.app.detail().c_str());
}

void test_hold_a_checks_again() {
  Harness h;
  h.app.onOpen();
  h.runUntilIdle();
  TEST_ASSERT_TRUE(h.app.onInput(InputEvent::Select));
  h.runUntilIdle();
  TEST_ASSERT_EQUAL(2, int(h.network.requests.size()));
  // Ignored while a check is running.
  h.app.onInput(InputEvent::Select);
  TEST_ASSERT_FALSE(h.app.onInput(InputEvent::Select));
  TEST_ASSERT_FALSE(h.app.onInput(InputEvent::Back));
}

void test_app_explains_missing_settings() {
  Harness noSync("");
  noSync.app.onOpen();
  TEST_ASSERT_EQUAL(0, noSync.network.connects);
  TEST_ASSERT_EQUAL_STRING("Not set up", noSync.app.headline().c_str());
  TEST_ASSERT_EQUAL_STRING("Add a [sync] url to config.toml", noSync.app.detail().c_str());
  TEST_ASSERT_FALSE(noSync.app.onInput(InputEvent::Select));

  Harness noWifi;
  noWifi.network.isConfigured = false;
  noWifi.app.onOpen();
  TEST_ASSERT_EQUAL(0, noWifi.network.connects);
  TEST_ASSERT_EQUAL_STRING("Set [wifi] in config.toml", noWifi.app.detail().c_str());
}

void test_closing_mid_check_leaves_wifi() {
  Harness h;
  h.app.onOpen();
  h.app.onClose();
  TEST_ASSERT_EQUAL(1, h.network.disconnects);
  TEST_ASSERT_EQUAL(int(SyncApp::Phase::Idle), int(h.app.phase()));
}

void test_host_of_url() {
  TEST_ASSERT_EQUAL_STRING("example.com", SyncApp::hostOf("https://example.com/dav/").c_str());
  TEST_ASSERT_EQUAL_STRING("example.com:8443", SyncApp::hostOf("https://example.com:8443").c_str());
  TEST_ASSERT_EQUAL_STRING("example.com", SyncApp::hostOf("https://u:p@example.com/x").c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_base64_encodes_with_padding);
  RUN_TEST(test_settings_read_from_config);
  RUN_TEST(test_check_sends_an_authenticated_propfind);
  RUN_TEST(test_check_without_credentials_sends_no_authorization);
  RUN_TEST(test_check_explains_each_failure);
  RUN_TEST(test_check_needs_a_url);
  RUN_TEST(test_file_urls_join_the_folder);
  RUN_TEST(test_get_file_is_authenticated);
  RUN_TEST(test_origin_etag_undoes_proxy_changes);
  RUN_TEST(test_put_sends_the_origin_etag);
  RUN_TEST(test_app_signs_in_on_open);
  RUN_TEST(test_app_shows_signing_in_before_the_request);
  RUN_TEST(test_app_reports_wrong_credentials);
  RUN_TEST(test_app_reports_wifi_failure);
  RUN_TEST(test_app_gives_up_joining_after_a_timeout);
  RUN_TEST(test_hold_a_checks_again);
  RUN_TEST(test_app_explains_missing_settings);
  RUN_TEST(test_closing_mid_check_leaves_wifi);
  RUN_TEST(test_host_of_url);
  return UNITY_END();
}

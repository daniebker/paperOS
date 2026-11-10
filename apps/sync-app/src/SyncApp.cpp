#include "SyncApp.h"

using paperos::Color;
using paperos::DavCheck;
using paperos::InputEvent;
using paperos::NetworkState;
using paperos::TextAlign;
using paperos::TextSize;

namespace {

constexpr int kMargin = 24;

std::string fitToWidth(const paperos::Display& display, std::string text, TextSize size,
                       int maxWidth) {
  if (display.textWidth(text.c_str(), size) <= maxWidth) return text;
  while (!text.empty() && display.textWidth((text + "...").c_str(), size) > maxWidth) {
    text.pop_back();
  }
  return text + "...";
}

}  // namespace

SyncApp::SyncApp(const paperos::Config& config, paperos::Network& network, paperos::Clock& clock,
                 const paperos::Battery& battery)
    : config_(config), network_(network), clock_(clock), battery_(battery) {}

std::string SyncApp::hostOf(const std::string& url) {
  size_t start = url.find("://");
  start = start == std::string::npos ? 0 : start + 3;
  const size_t end = url.find_first_of("/?#", start);
  std::string host = url.substr(start, end == std::string::npos ? std::string::npos : end - start);
  const size_t at = host.rfind('@');  // drop any user:password@
  if (at != std::string::npos) host = host.substr(at + 1);
  return host;
}

void SyncApp::onOpen() {
  settings_ = paperos::DavSettings::fromConfig(config_);
  debug_ = paperos::debugEnabled(config_);
  phase_ = Phase::Idle;
  checked_ = false;
  result_ = DavCheck::NotConfigured;
  message_.clear();
  wifiDetail_.clear();
  startCheck();
}

void SyncApp::onClose() {
  if (phase_ != Phase::Idle) network_.disconnect();
  phase_ = Phase::Idle;
}

bool SyncApp::onInput(InputEvent event) {
  if (event == InputEvent::Select) return startCheck();
  return false;  // Back falls through to the shell, which closes the app.
}

bool SyncApp::startCheck() {
  if (phase_ != Phase::Idle) return false;
  if (!settings_.configured() || !network_.configured()) return false;
  checked_ = false;
  message_.clear();
  wifiDetail_.clear();
  phase_ = Phase::Connecting;
  phaseStartedMs_ = clock_.uptimeMs();
  network_.connect();
  return true;
}

void SyncApp::finish() {
  network_.disconnect();
  phase_ = Phase::Idle;
  checked_ = true;
}

bool SyncApp::tick() {
  switch (phase_) {
    case Phase::Idle:
      return false;
    case Phase::Connecting: {
      const NetworkState state = network_.state();
      if (state == NetworkState::Connected) {
        // Redraw to show "Signing in" before the request, which waits for
        // the server.
        phase_ = Phase::Checking;
        return true;
      }
      const uint32_t elapsed = clock_.uptimeMs() - phaseStartedMs_;
      if (state == NetworkState::Failed || elapsed > kConnectTimeoutMs) {
        result_ = DavCheck::Failed;
        message_ = "Could not connect to Wi-Fi";
        if (debug_) {
          wifiDetail_ = network_.failureDetail();
          if (wifiDetail_.empty()) wifiDetail_ = "no detail from the Wi-Fi driver";
        }
        finish();
        return true;
      }
      return false;
    }
    case Phase::Checking: {
      paperos::DavClient client(network_, settings_);
      result_ = client.checkCredentials(message_);
      finish();
      return true;
    }
  }
  return false;
}

std::string SyncApp::headline() const {
  if (!settings_.configured()) return "Not set up";
  if (!network_.configured()) return "No Wi-Fi set up";
  switch (phase_) {
    case Phase::Connecting:
      return "Connecting to Wi-Fi...";
    case Phase::Checking:
      return "Signing in...";
    case Phase::Idle:
      break;
  }
  if (!checked_) return "Not checked yet";
  return result_ == DavCheck::Ok ? "Signed in" : "Could not sign in";
}

std::string SyncApp::detail() const {
  if (!settings_.configured()) return "Add a [sync] url to config.toml";
  if (!network_.configured()) return "Set [wifi] in config.toml";
  if (phase_ != Phase::Idle || !checked_) return "";
  if (result_ == DavCheck::Ok) {
    return settings_.username.empty() ? "The server accepted the connection"
                                      : "as " + paperos::foldUtf8ToAscii(settings_.username);
  }
  return message_;
}

paperos::NetworkState SyncApp::wifiState() const {
  switch (phase_) {
    case Phase::Connecting:
      return NetworkState::Connecting;
    case Phase::Checking:
      return NetworkState::Connected;
    case Phase::Idle:
      break;
  }
  return NetworkState::Off;
}

void SyncApp::draw(paperos::Display& display) {
  const int w = display.width();
  const int h = display.height();
  const int maxW = w - 2 * kMargin;

  // Header, like the Calendar's.
  const int titleH = display.textHeight(TextSize::Title);
  const int smallH = display.textHeight(TextSize::Small);
  const int mediumH = display.textHeight(TextSize::Medium);
  display.drawText("Sync", kMargin, kMargin, TextSize::Title);
  const int batteryY = kMargin + (titleH - smallH) / 2;
  const int batteryW =
      paperos::drawBatteryLevel(display, battery_, w - kMargin, batteryY, TextAlign::Right);
  const int wifiH = smallH * 2 / 3;
  paperos::drawWifiStatus(display, wifiState(), w - kMargin - batteryW - (batteryW > 0 ? 12 : 0),
                          batteryY + (smallH - wifiH) / 2, wifiH);
  const int headerBottom = kMargin * 2 + titleH;
  display.fillRect(0, headerBottom - 4, w, 4, Color::Black);

  // Server and account.
  int y = headerBottom + kMargin;
  if (settings_.configured()) {
    const std::string server = fitToWidth(display, "Server: " + hostOf(settings_.url),
                                          TextSize::Medium, maxW);
    display.drawText(server.c_str(), kMargin, y, TextSize::Medium);
    y += mediumH + 12;
    const std::string user = settings_.username.empty()
                                 ? std::string("User: none")
                                 : "User: " + paperos::foldUtf8ToAscii(settings_.username);
    display.drawText(fitToWidth(display, user, TextSize::Medium, maxW).c_str(), kMargin, y,
                     TextSize::Medium);
  }

  // Result, in the middle.
  const int largeH = display.textHeight(TextSize::Large);
  const int midY = h / 2 - largeH;
  display.drawText(fitToWidth(display, headline(), TextSize::Large, maxW).c_str(), w / 2, midY,
                   TextSize::Large, TextAlign::Center);
  const std::string more = detail();
  if (!more.empty()) {
    display.drawText(fitToWidth(display, more, TextSize::Small, maxW).c_str(), w / 2,
                     midY + largeH + 16, TextSize::Small, TextAlign::Center, Color::DarkGray);
  }
  if (!wifiDetail_.empty()) {
    display.drawText(fitToWidth(display, wifiDetail_, TextSize::Small, maxW).c_str(), w / 2,
                     midY + largeH + 16 + smallH + 8, TextSize::Small, TextAlign::Center,
                     Color::DarkGray);
  }

  // Footer.
  const char* legend = settings_.configured() && network_.configured()
                           ? "Hold A: check again    Hold B: back"
                           : "Hold B: back";
  display.drawText(legend, w / 2, h - kMargin - smallH, TextSize::Small, TextAlign::Center,
                   Color::DarkGray);
}

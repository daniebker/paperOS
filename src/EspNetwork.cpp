#include "EspNetwork.h"

#include <HTTPClient.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

// The ESP-IDF's root certificate bundle, linked in from libmbedtls.
extern const uint8_t kRootCertificates[] asm("_binary_x509_crt_bundle_start");

namespace {

constexpr uint32_t kJoinTimeoutMs = 20000;
constexpr uint32_t kConnectTimeoutMs = 10000;
constexpr uint16_t kHeaderTimeoutMs = 15000;
// A download with no data for this long has failed.
constexpr uint32_t kStallTimeoutMs = 20000;

bool startsWith(const std::string& text, const char* prefix) {
  return text.compare(0, strlen(prefix), prefix) == 0;
}

class EspHttpResponse : public paperos::HttpResponse {
 public:
  explicit EspHttpResponse(bool secure) {
    if (secure) {
      secureClient_.setCACertBundle(kRootCertificates);
      client_ = &secureClient_;
    } else {
      client_ = &plainClient_;
    }
  }
  ~EspHttpResponse() override { http_.end(); }

  bool begin(const std::string& url, std::string& error) {
    http_.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http_.setRedirectLimit(5);
    // HTTP/1.0 keeps the body free of chunked encoding: it is the raw bytes
    // until the server closes the connection.
    http_.useHTTP10(true);
    http_.setConnectTimeout(kConnectTimeoutMs);
    http_.setTimeout(kHeaderTimeoutMs);
    http_.setUserAgent("paperOS");
    if (!http_.begin(*client_, url.c_str())) {
      error = "bad url";
      return false;
    }
    const int code = http_.GET();
    if (code <= 0) {
      error = HTTPClient::errorToString(code).c_str();
      M5_LOGW("GET failed: %s", error.c_str());
      return false;
    }
    if (code != HTTP_CODE_OK) {
      error = "HTTP " + std::to_string(code);
      M5_LOGW("GET failed: %s", error.c_str());
      return false;
    }
    stream_ = http_.getStreamPtr();
    remaining_ = http_.getSize();
    total_ = remaining_;
    lastDataMs_ = millis();
    return true;
  }

  int read(void* buffer, size_t size) override {
    if (remaining_ == 0) return kEnd;
    const int available = stream_->available();
    if (available <= 0) {
      // With no length given, the body ends when the server hangs up.
      if (!stream_->connected()) return remaining_ < 0 ? kEnd : kError;
      return millis() - lastDataMs_ > kStallTimeoutMs ? kError : 0;
    }
    size_t count = std::min(size, size_t(available));
    if (remaining_ > 0) count = std::min(count, size_t(remaining_));
    const int n = stream_->read(static_cast<uint8_t*>(buffer), count);
    if (n <= 0) return 0;
    if (remaining_ > 0) remaining_ -= n;
    lastDataMs_ = millis();
    return n;
  }

  int64_t contentLength() const override { return total_; }

 private:
  WiFiClientSecure secureClient_;
  WiFiClient plainClient_;
  WiFiClient* client_ = nullptr;
  HTTPClient http_;  // declared last so it is destroyed before the clients
  WiFiClient* stream_ = nullptr;
  int64_t remaining_ = -1;  // bytes left, or -1 if the length is unknown
  int64_t total_ = -1;      // from the headers, or -1
  uint32_t lastDataMs_ = 0;
};

}  // namespace

void EspNetwork::begin(const paperos::Config& config) {
  ssid_ = config.getString("wifi", "ssid", "");
  password_ = config.getString("wifi", "password", "");
}

void EspNetwork::connect() {
  if (!configured()) {
    state_ = paperos::NetworkState::Failed;
    return;
  }
  if (state_ == paperos::NetworkState::Connecting) return;
  if (state_ == paperos::NetworkState::Connected && WiFi.status() == WL_CONNECTED) return;
  M5_LOGI("Joining Wi-Fi network %s", ssid_.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid_.c_str(), password_.c_str());
  state_ = paperos::NetworkState::Connecting;
  connectStartedMs_ = millis();
}

void EspNetwork::disconnect() {
  if (state_ == paperos::NetworkState::Off) return;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  state_ = paperos::NetworkState::Off;
}

paperos::NetworkState EspNetwork::state() {
  const wl_status_t status = WiFi.status();
  if (state_ == paperos::NetworkState::Connecting) {
    if (status == WL_CONNECTED) {
      M5_LOGI("Wi-Fi connected, IP %s", WiFi.localIP().toString().c_str());
      state_ = paperos::NetworkState::Connected;
    } else if (status == WL_CONNECT_FAILED || millis() - connectStartedMs_ > kJoinTimeoutMs) {
      M5_LOGW("Could not join Wi-Fi network %s (status %d)", ssid_.c_str(), int(status));
      disconnect();
      state_ = paperos::NetworkState::Failed;
    }
  } else if (state_ == paperos::NetworkState::Connected && status != WL_CONNECTED) {
    state_ = paperos::NetworkState::Failed;
  }
  return state_;
}

std::unique_ptr<paperos::HttpResponse> EspNetwork::get(const char* url, std::string& error) {
  std::string target = url;
  if (startsWith(target, "webcal://")) target = "https://" + target.substr(9);
  if (startsWith(target, "webcals://")) target = "https://" + target.substr(10);
  const bool secure = startsWith(target, "https://");
  if (!secure && !startsWith(target, "http://")) {
    error = "url must start with https://";
    return nullptr;
  }
  if (state() != paperos::NetworkState::Connected) {
    error = "no Wi-Fi connection";
    return nullptr;
  }
  std::unique_ptr<EspHttpResponse> response(new EspHttpResponse(secure));
  if (!response->begin(target, error)) return nullptr;
  return std::unique_ptr<paperos::HttpResponse>(response.release());
}

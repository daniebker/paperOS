#pragma once

#include <string>

#include <paperos.h>

// paperos::Network backed by the ESP32's Wi-Fi and HTTPClient. The network is
// read from config.toml:
//
//   [wifi]
//     ssid = "MyNetwork"
//     password = "secret"
//
// https certificates are checked against the root certificates built into
// the ESP-IDF.
class EspNetwork : public paperos::Network {
 public:
  void begin(const paperos::Config& config);

  bool configured() const override { return !ssid_.empty(); }
  void connect() override;
  void disconnect() override;
  paperos::NetworkState state() override;
  std::unique_ptr<paperos::HttpResponse> get(const char* url, std::string& error) override;

 private:
  std::string ssid_;
  std::string password_;
  paperos::NetworkState state_ = paperos::NetworkState::Off;
  uint32_t connectStartedMs_ = 0;
};

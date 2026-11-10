#pragma once

#include <cstdint>
#include <string>

#include <paperos.h>

// Checks the sync server set in config.toml accepts your credentials:
//
//   [sync]
//     url = "https://cloud.example.com/remote.php/dav/files/dan/mindwtr/"
//     username = "dan"
//     password = "app-password"
//
// Opening the app joins Wi-Fi (see [wifi]), signs in to the server and shows
// whether it worked, then leaves Wi-Fi again. Hold A checks again and hold B
// returns home.
class SyncApp : public paperos::App {
 public:
  static constexpr uint32_t kConnectTimeoutMs = 30000;

  enum class Phase : uint8_t { Idle, Connecting, Checking };

  SyncApp(const paperos::Config& config, paperos::Network& network, paperos::Clock& clock,
          const paperos::Battery& battery);

  const char* name() const override { return "Sync"; }
  void onOpen() override;
  void onClose() override;
  bool onInput(paperos::InputEvent event) override;
  bool tick() override;
  void draw(paperos::Display& display) override;

  // For tests.
  Phase phase() const { return phase_; }
  bool checked() const { return checked_; }
  paperos::DavCheck result() const { return result_; }
  // The headline and detail drawn in the middle of the screen.
  std::string headline() const;
  std::string detail() const;

  // The host name in `url`, such as "cloud.example.com".
  static std::string hostOf(const std::string& url);

 private:
  bool startCheck();
  void finish();
  paperos::NetworkState wifiState() const;

  const paperos::Config& config_;
  paperos::Network& network_;
  paperos::Clock& clock_;
  const paperos::Battery& battery_;

  paperos::DavSettings settings_;
  Phase phase_ = Phase::Idle;
  uint32_t phaseStartedMs_ = 0;
  bool checked_ = false;  // a check has finished since the app opened
  paperos::DavCheck result_ = paperos::DavCheck::NotConfigured;
  std::string message_;
  std::string wifiDetail_;  // with [Developer] debug, why Wi-Fi failed
  bool debug_ = false;
};

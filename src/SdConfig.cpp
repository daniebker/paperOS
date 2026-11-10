#include "SdConfig.h"

#include <M5Unified.h>
#include <SD.h>
#include <SPI.h>

namespace {

constexpr uint32_t kSdSpiHz = 25000000;
constexpr size_t kMaxConfigBytes = 16 * 1024;
// A card that was just inserted, or is slow to wake, can fail the first try.
constexpr int kMountAttempts = 3;
constexpr uint32_t kMountRetryDelayMs = 200;

bool mountSd() {
  const int cs = M5.getPin(m5::pin_name_t::sd_spi_cs);
  if (cs < 0) {
    M5_LOGW("No SD card slot known for this board");
    return false;
  }
  SPI.begin(M5.getPin(m5::pin_name_t::sd_spi_sclk), M5.getPin(m5::pin_name_t::sd_spi_miso),
            M5.getPin(m5::pin_name_t::sd_spi_mosi), cs);
  for (int attempt = 1; attempt <= kMountAttempts; ++attempt) {
    if (SD.begin(cs, SPI, kSdSpiHz)) return true;
    SD.end();
    if (attempt < kMountAttempts) delay(kMountRetryDelayMs);
  }
  M5_LOGW("SD card not mounted (missing or unreadable)");
  return false;
}

}  // namespace

bool ensureSdMounted() {
  if (SD.cardType() != CARD_NONE) return true;
  SD.end();
  const bool mounted = mountSd();
  if (mounted) M5_LOGI("SD card mounted");
  return mounted;
}

bool loadConfigFromSd(paperos::Config& config) {
  config.clear();
  if (!mountSd()) return false;

  File file = SD.open(kConfigPath, FILE_READ);
  if (!file) {
    M5_LOGW("%s not found on SD card", kConfigPath);
    return false;
  }
  if (file.size() > kMaxConfigBytes) {
    M5_LOGW("%s is larger than %u bytes, ignoring it", kConfigPath, unsigned(kMaxConfigBytes));
    file.close();
    return false;
  }
  const String text = file.readString();
  file.close();

  if (!config.parse(text.c_str())) {
    M5_LOGW("%s: %d line(s) could not be parsed, first at line %d", kConfigPath,
            config.errorCount(), config.firstErrorLine());
  }
  M5_LOGI("Loaded %u setting(s) from %s", unsigned(config.size()), kConfigPath);
  return true;
}

#pragma once

#include <paperos.h>

// Path of the config file on the SD card.
constexpr const char* kConfigPath = "/config.toml";

// Mounts the SD card (SPI, pins from M5Unified's board table) and loads
// kConfigPath into `config`. Returns false if the card or file is missing;
// `config` is then left empty so apps fall back to defaults.
bool loadConfigFromSd(paperos::Config& config);

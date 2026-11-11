#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace calendar {

// The daily weather forecast shown under each day's heading, from Open-Meteo
// (https://open-meteo.com), which needs no account or key. The place is set
// in config.toml:
//
//   [weather]
//     latitude = 52.52
//     longitude = 13.41
//     units = "celsius"   # or "fahrenheit"

struct DayForecast {
  int64_t day = 0;     // local civil seconds of the day's midnight
  int code = -1;       // WMO weather code, -1 if unknown
  bool hasTemps = false;
  int high = 0;        // rounded, in the units asked for
  int low = 0;
  int rainChance = -1;  // percent, -1 if unknown
};

struct WeatherSettings {
  std::string latitude;   // as written in config.toml, checked to be a number
  std::string longitude;
  bool fahrenheit = false;
};

// Reads [weather] from config.toml's values. Returns false, leaving `out`
// alone, if the latitude or longitude is missing or not a valid number.
bool parseWeatherSettings(const char* latitude, const char* longitude, const char* units,
                          WeatherSettings& out);

// The Open-Meteo address for `days` days of forecasts from today (1 to 16).
std::string forecastUrl(const WeatherSettings& settings, int days);

// Parses Open-Meteo's JSON reply. Returns false if it holds no daily
// forecast, such as an error reply.
bool parseForecast(const std::string& json, std::vector<DayForecast>& out);

// "Partly cloudy", "Light rain", ... or "" for an unknown code.
const char* describeWeatherCode(int code);

// One line for the agenda: "Partly cloudy   High 12C  Low 5C   Rain 40%".
std::string forecastText(const DayForecast& forecast, bool fahrenheit);

}  // namespace calendar

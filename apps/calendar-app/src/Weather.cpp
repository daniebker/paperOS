#include "Weather.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <utility>

#include "CivilTime.h"

namespace calendar {

namespace {

constexpr int kMaxForecastDays = 16;  // Open-Meteo's limit

// True if `text` is a plain decimal number such as "52.52" or "-0.1276",
// between -limit and limit.
bool parseCoordinate(const char* text, double limit) {
  if (text == nullptr || *text == '\0') return false;
  for (const char* p = text; *p != '\0'; ++p) {
    const char c = *p;
    if (!((c >= '0' && c <= '9') || c == '.' || (p == text && (c == '-' || c == '+')))) {
      return false;
    }
  }
  char* end = nullptr;
  const double value = std::strtod(text, &end);
  return end != text && *end == '\0' && std::fabs(value) <= limit;
}

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// Reads the JSON array after `"key":` in text[begin, end) as raw tokens:
// strings without their quotes, numbers as written, and "" for null.
bool readArray(const std::string& text, size_t begin, size_t end, const char* key,
               std::vector<std::string>& out) {
  out.clear();
  const std::string quoted = std::string("\"") + key + "\"";
  size_t pos = text.find(quoted, begin);
  if (pos == std::string::npos || pos >= end) return false;
  pos += quoted.size();
  while (pos < end && isSpace(text[pos])) ++pos;
  if (pos >= end || text[pos] != ':') return false;
  ++pos;
  while (pos < end && isSpace(text[pos])) ++pos;
  if (pos >= end || text[pos] != '[') return false;
  ++pos;
  while (pos < end) {
    while (pos < end && (isSpace(text[pos]) || text[pos] == ',')) ++pos;
    if (pos >= end) return false;
    if (text[pos] == ']') return true;
    if (text[pos] == '"') {
      const size_t close = text.find('"', pos + 1);
      if (close == std::string::npos || close >= end) return false;
      out.push_back(text.substr(pos + 1, close - pos - 1));
      pos = close + 1;
    } else {
      const size_t start = pos;
      while (pos < end && text[pos] != ',' && text[pos] != ']' && !isSpace(text[pos])) ++pos;
      const std::string token = text.substr(start, pos - start);
      out.push_back(token == "null" ? "" : token);
    }
  }
  return false;
}

// "2026-11-09" as civil seconds of that midnight.
bool parseDate(const std::string& text, int64_t& out) {
  int year = 0, month = 0, day = 0;
  if (std::sscanf(text.c_str(), "%4d-%2d-%2d", &year, &month, &day) != 3) return false;
  if (month < 1 || month > 12 || day < 1 || day > daysInMonth(year, month)) return false;
  out = daysFromCivil(year, month, day) * kSecondsPerDay;
  return true;
}

bool parseNumber(const std::string& text, double& out) {
  if (text.empty()) return false;
  char* end = nullptr;
  out = std::strtod(text.c_str(), &end);
  return end != text.c_str() && *end == '\0' && std::isfinite(out);
}

}  // namespace

bool parseWeatherSettings(const char* latitude, const char* longitude, const char* units,
                          WeatherSettings& out) {
  if (!parseCoordinate(latitude, 90) || !parseCoordinate(longitude, 180)) return false;
  out.latitude = latitude;
  out.longitude = longitude;
  const std::string u = units == nullptr ? "" : units;
  out.fahrenheit = u == "fahrenheit" || u == "Fahrenheit" || u == "F" || u == "f";
  return true;
}

std::string forecastUrl(const WeatherSettings& settings, int days) {
  if (days < 1) days = 1;
  if (days > kMaxForecastDays) days = kMaxForecastDays;
  std::string url = "https://api.open-meteo.com/v1/forecast?latitude=" + settings.latitude +
                    "&longitude=" + settings.longitude +
                    "&daily=weather_code,temperature_2m_max,temperature_2m_min,"
                    "precipitation_probability_max&timezone=auto&forecast_days=" +
                    std::to_string(days);
  if (settings.fahrenheit) url += "&temperature_unit=fahrenheit";
  return url;
}

bool parseForecast(const std::string& json, std::vector<DayForecast>& out) {
  // "daily_units" holds the same keys, so look inside "daily" only. Its
  // values are flat arrays, so the first '}' after it closes it.
  const size_t daily = json.find("\"daily\"");
  if (daily == std::string::npos) return false;
  const size_t open = json.find('{', daily);
  if (open == std::string::npos) return false;
  const size_t close = json.find('}', open);
  if (close == std::string::npos) return false;

  std::vector<std::string> times, codes, highs, lows, rain;
  if (!readArray(json, open, close, "time", times) || times.empty()) return false;
  // The others are optional: a day without them shows what it has.
  readArray(json, open, close, "weather_code", codes);
  readArray(json, open, close, "temperature_2m_max", highs);
  readArray(json, open, close, "temperature_2m_min", lows);
  readArray(json, open, close, "precipitation_probability_max", rain);

  std::vector<DayForecast> days;
  for (size_t i = 0; i < times.size(); ++i) {
    DayForecast f;
    if (!parseDate(times[i], f.day)) return false;
    double value = 0;
    if (i < codes.size() && parseNumber(codes[i], value)) f.code = int(value);
    double high = 0, low = 0;
    if (i < highs.size() && i < lows.size() && parseNumber(highs[i], high) &&
        parseNumber(lows[i], low)) {
      f.hasTemps = true;
      f.high = int(std::lround(high));
      f.low = int(std::lround(low));
    }
    if (i < rain.size() && parseNumber(rain[i], value)) {
      f.rainChance = int(std::lround(std::fmin(100, std::fmax(0, value))));
    }
    days.push_back(f);
  }
  out = std::move(days);
  return true;
}

const char* describeWeatherCode(int code) {
  switch (code) {
    case 0: return "Clear";
    case 1: return "Mostly clear";
    case 2: return "Partly cloudy";
    case 3: return "Overcast";
    case 45:
    case 48: return "Fog";
    case 51:
    case 53:
    case 55: return "Drizzle";
    case 56:
    case 57: return "Freezing drizzle";
    case 61: return "Light rain";
    case 63: return "Rain";
    case 65: return "Heavy rain";
    case 66:
    case 67: return "Freezing rain";
    case 71: return "Light snow";
    case 73: return "Snow";
    case 75: return "Heavy snow";
    case 77: return "Snow grains";
    case 80: return "Light showers";
    case 81: return "Showers";
    case 82: return "Heavy showers";
    case 85:
    case 86: return "Snow showers";
    case 95: return "Thunderstorms";
    case 96:
    case 99: return "Thunderstorms, hail";
    default: return "";
  }
}

std::string forecastText(const DayForecast& forecast, bool fahrenheit) {
  // The fonts are 7-bit ASCII, so there is no degree sign.
  const char unit = fahrenheit ? 'F' : 'C';
  std::string text = describeWeatherCode(forecast.code);
  if (forecast.hasTemps) {
    if (!text.empty()) text += "   ";
    text += "High " + std::to_string(forecast.high) + unit + "  Low " +
            std::to_string(forecast.low) + unit;
  }
  if (forecast.rainChance >= 0) {
    if (!text.empty()) text += "   ";
    text += "Rain " + std::to_string(forecast.rainChance) + "%";
  }
  return text;
}

}  // namespace calendar

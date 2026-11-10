#include "paperos/WebDav.h"

#include <utility>

namespace paperos {

namespace {

constexpr const char* kSection = "sync";

// Asks only for the resource type, the cheapest property to answer.
constexpr const char* kPropfindBody =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<d:propfind xmlns:d=\"DAV:\"><d:prop><d:resourcetype/></d:prop></d:propfind>";

bool startsWith(const std::string& text, const char* prefix) {
  return text.compare(0, std::char_traits<char>::length(prefix), prefix) == 0;
}

}  // namespace

std::string base64Encode(const std::string& data) {
  static const char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((data.size() + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 2 < data.size(); i += 3) {
    const uint32_t n = uint32_t(uint8_t(data[i])) << 16 | uint32_t(uint8_t(data[i + 1])) << 8 |
                       uint8_t(data[i + 2]);
    out += kAlphabet[n >> 18 & 63];
    out += kAlphabet[n >> 12 & 63];
    out += kAlphabet[n >> 6 & 63];
    out += kAlphabet[n & 63];
  }
  const size_t rest = data.size() - i;
  if (rest > 0) {
    uint32_t n = uint32_t(uint8_t(data[i])) << 16;
    if (rest == 2) n |= uint32_t(uint8_t(data[i + 1])) << 8;
    out += kAlphabet[n >> 18 & 63];
    out += kAlphabet[n >> 12 & 63];
    out += rest == 2 ? kAlphabet[n >> 6 & 63] : '=';
    out += '=';
  }
  return out;
}

std::string originEtag(const std::string& etag) {
  std::string tag = etag;
  if (tag.compare(0, 2, "W/") == 0) tag.erase(0, 2);
  const bool quoted = tag.size() >= 2 && tag.front() == '"' && tag.back() == '"';
  std::string value = quoted ? tag.substr(1, tag.size() - 2) : tag;
  for (const char* suffix : {"-gzip", "-br", "-deflate"}) {
    const size_t n = std::char_traits<char>::length(suffix);
    if (value.size() > n && value.compare(value.size() - n, n, suffix) == 0) {
      value.erase(value.size() - n);
      break;
    }
  }
  return quoted ? "\"" + value + "\"" : value;
}

DavSettings DavSettings::fromConfig(const Config& config) {
  DavSettings settings;
  settings.url = config.getString(kSection, "url", "");
  settings.username = config.getString(kSection, "username", "");
  settings.password = config.getString(kSection, "password", "");
  // Calendar-style addresses work too, since CalDAV is WebDAV underneath.
  if (startsWith(settings.url, "webcal://")) settings.url = "https://" + settings.url.substr(9);
  if (startsWith(settings.url, "webcals://")) settings.url = "https://" + settings.url.substr(10);
  return settings;
}

DavClient::DavClient(Network& network, DavSettings settings)
    : network_(network), settings_(std::move(settings)) {}

std::string DavClient::fileUrl(const std::string& name) const {
  std::string url = settings_.url;
  if (url.empty() || url.back() != '/') url += '/';
  size_t start = 0;
  while (start < name.size() && name[start] == '/') ++start;
  return url + name.substr(start);
}

void DavClient::addAuthorization(HttpRequest& request) const {
  if (settings_.username.empty() && settings_.password.empty()) return;
  request.headers.emplace_back(
      "Authorization", "Basic " + base64Encode(settings_.username + ":" + settings_.password));
}

DavCheck DavClient::checkCredentials(std::string& message) {
  message.clear();
  if (!settings_.configured()) {
    message = "Set [sync] url in config.toml";
    return DavCheck::NotConfigured;
  }
  HttpRequest request;
  request.method = "PROPFIND";
  request.url = settings_.url;
  request.headers.emplace_back("Depth", "0");
  request.headers.emplace_back("Content-Type", "application/xml; charset=utf-8");
  addAuthorization(request);
  request.body = kPropfindBody;

  std::string error;
  std::unique_ptr<HttpResponse> response = network_.send(request, error);
  if (!response) {
    message = error.empty() ? "No answer from the server" : error;
    return DavCheck::Failed;
  }
  const int status = response->status();
  switch (status) {
    case 207:  // Multi-Status: a WebDAV answer
      return DavCheck::Ok;
    case 401:
      message = "Wrong username or password";
      return DavCheck::BadCredentials;
    case 403:
      message = "Not allowed to open this folder";
      return DavCheck::Forbidden;
    case 404:
      message = "Folder not found, check the url";
      return DavCheck::NotFound;
    case 200:  // a plain web page
    case 405:  // Method Not Allowed
    case 501:  // Not Implemented
      message = "Not a WebDAV address, check the url";
      return DavCheck::NotDav;
    case 301:
    case 302:
    case 307:
    case 308:
      message = "The server moved it (HTTP " + std::to_string(status) + "), check the url";
      return DavCheck::Failed;
    default:
      message = "Server answered HTTP " + std::to_string(status);
      return DavCheck::Failed;
  }
}

std::unique_ptr<HttpResponse> DavClient::getFile(const std::string& name, std::string& error) {
  if (!settings_.configured()) {
    error = "Set [sync] url in config.toml";
    return nullptr;
  }
  HttpRequest request;
  request.url = fileUrl(name);
  addAuthorization(request);
  std::unique_ptr<HttpResponse> response = network_.send(request, error);
  if (!response) return nullptr;
  const int status = response->status();
  if (status == 200) return response;
  error = status == 401   ? "Wrong username or password"
          : status == 404 ? name + " not found"
                          : "HTTP " + std::to_string(status);
  return nullptr;
}

DavWrite DavClient::putFile(const std::string& name, const std::string& body,
                            const std::string& ifMatch, const char* contentType,
                            std::string& error) {
  if (!settings_.configured()) {
    error = "Set [sync] url in config.toml";
    return DavWrite::Failed;
  }
  HttpRequest request;
  request.method = "PUT";
  request.url = fileUrl(name);
  request.headers.emplace_back("Content-Type", contentType);
  if (!ifMatch.empty()) request.headers.emplace_back("If-Match", originEtag(ifMatch));
  addAuthorization(request);
  request.body = body;
  std::unique_ptr<HttpResponse> response = network_.send(request, error);
  if (!response) return DavWrite::Failed;
  const int status = response->status();
  if (status >= 200 && status < 300) return DavWrite::Ok;
  if (status == 412) {
    error = name + " changed on the server";
    return DavWrite::Conflict;
  }
  error = status == 401 ? "Wrong username or password" : "HTTP " + std::to_string(status);
  return DavWrite::Failed;
}

}  // namespace paperos

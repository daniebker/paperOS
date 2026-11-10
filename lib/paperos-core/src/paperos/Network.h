#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "paperos/Display.h"

namespace paperos {

// The body of an HTTP response, read a piece at a time so apps can download
// from tick() without freezing the buttons.
class HttpResponse {
 public:
  static constexpr int kEnd = -1;    // the whole body has been read
  static constexpr int kError = -2;  // the connection failed or timed out

  virtual ~HttpResponse() = default;

  // Copies up to `size` bytes of the body into `buffer`. Returns the number of
  // bytes copied, 0 if none have arrived yet, or kEnd / kError. Never blocks
  // for long.
  virtual int read(void* buffer, size_t size) = 0;

  // Body size from the headers, or negative if the server didn't say.
  virtual int64_t contentLength() const = 0;

  // HTTP status code. get() only returns 200 responses; send() returns any.
  virtual int status() const { return 200; }

  // The value of the response header `name` (case-insensitive), or "" if the
  // server didn't send it. Only ETag is kept.
  virtual std::string header(const char* name) const {
    (void)name;
    return {};
  }
};

// A request for Network::send(), for methods other than a plain GET, such as
// WebDAV's PROPFIND.
struct HttpRequest {
  std::string method = "GET";
  std::string url;
  std::vector<std::pair<std::string, std::string>> headers;  // name, value
  std::string body;
};

enum class NetworkState : uint8_t { Off, Connecting, Connected, Failed };

// Wi-Fi and HTTP(S), read through this interface so apps can be tested on the
// host, like Display. The network to join comes from config.toml.
class Network {
 public:
  virtual ~Network() = default;

  // False when no network is configured, so connect() can never succeed.
  virtual bool configured() const = 0;

  // Starts joining the network if not already connected or connecting.
  // Returns at once; poll state() to follow it.
  virtual void connect() = 0;
  // Leaves the network to save power.
  virtual void disconnect() = 0;
  virtual NetworkState state() = 0;
  // Technical detail on why joining the network last failed, such as the
  // driver's reason code, for debugging. Empty if there is none.
  virtual std::string failureDetail() const { return {}; }

  // Sends a GET request and waits for the response headers, following
  // redirects. Accepts http://, https:// and webcal:// (fetched over https)
  // URLs; https certificates are verified. Returns nullptr on failure, with a
  // short reason for the user in `error`.
  virtual std::unique_ptr<HttpResponse> get(const char* url, std::string& error) = 0;

  // Sends `request` and waits for the response headers. Accepts http:// and
  // https:// URLs; https certificates are verified. Redirects are not
  // followed. Unlike get(), returns the response whatever its status code;
  // check status(). Returns nullptr only when no response arrived, with a
  // short reason for the user in `error`.
  virtual std::unique_ptr<HttpResponse> send(const HttpRequest& request, std::string& error) {
    (void)request;
    error = "not supported";
    return nullptr;
  }
};

// Draws a Wi-Fi icon of three rising bars whose right edge is at `right` and
// top at `y`, `height` pixels tall: filled when connected, outlined while
// connecting, and nothing when off or failed. Returns the width drawn, 0 if
// nothing.
int drawWifiStatus(Display& display, NetworkState state, int right, int y, int height,
                   Color color = Color::DarkGray);

}  // namespace paperos

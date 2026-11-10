#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "paperos/Config.h"
#include "paperos/Network.h"

namespace paperos {

// Encodes `data` as base64 (RFC 4648, with padding).
std::string base64Encode(const std::string& data);

// The server files are synced with, set in config.toml:
//
//   [sync]
//     url = "https://cloud.example.com/remote.php/dav/files/dan/mindwtr/"
//     username = "dan"
//     password = "app-password"
//
// Any WebDAV server works, including CalDAV servers such as Nextcloud, which
// are WebDAV underneath. `url` is the folder that holds the files.
struct DavSettings {
  std::string url;
  std::string username;
  std::string password;

  static DavSettings fromConfig(const Config& config);
  // True when a url is set.
  bool configured() const { return !url.empty(); }
};

// Result of DavClient::checkCredentials().
enum class DavCheck : uint8_t {
  Ok,              // signed in and the folder is there
  NotConfigured,   // no url in [sync]
  BadCredentials,  // 401: wrong username or password
  Forbidden,       // 403: signed in, but not allowed to read the folder
  NotFound,        // 404: no folder at the url
  NotDav,          // the server answered, but not as a WebDAV server
  Failed,          // no answer, or an unexpected one
};

// The server's own ETag for a response's ETag header. A proxy or compression
// module in front of the server can weaken it and mark it for the encoding,
// such as W/"abc-gzip" for "abc", and the server then refuses If-Match with
// that value. Strips the W/ and a -gzip, -br or -deflate suffix.
std::string originEtag(const std::string& etag);

// Result of DavClient::putFile().
enum class DavWrite : uint8_t {
  Ok,
  Conflict,  // 412: the file changed on the server since it was read
  Failed,
};

// Talks to a WebDAV / CalDAV server with HTTP basic authentication. Each call
// waits for the response headers, like Network::get(), so call it from an
// app's tick() once the network is connected.
class DavClient {
 public:
  DavClient(Network& network, DavSettings settings);

  // Asks the server about the folder at the url (PROPFIND, depth 0), which
  // needs valid credentials. On anything but Ok, `message` says what went
  // wrong in a few words for the user.
  DavCheck checkCredentials(std::string& message);

  // Downloads the file `name` from the folder, such as "data.json". Returns
  // nullptr on failure, with a short reason in `error`.
  std::unique_ptr<HttpResponse> getFile(const std::string& name, std::string& error);

  // Replaces the file `name` with `body`, only if the server's copy still has
  // the ETag `ifMatch`, as read from the download's ETag header, so changes
  // made elsewhere since it was read are never overwritten. A weakened ETag is
  // turned back into the server's own with originEtag(). On anything but Ok, `error` says why in a few words.
  DavWrite putFile(const std::string& name, const std::string& body, const std::string& ifMatch,
                   const char* contentType, std::string& error);

  // The url of `name` inside the folder.
  std::string fileUrl(const std::string& name) const;

  const DavSettings& settings() const { return settings_; }

 private:
  void addAuthorization(HttpRequest& request) const;

  Network& network_;
  DavSettings settings_;
};

}  // namespace paperos

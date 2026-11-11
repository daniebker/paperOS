#include "Epub.h"

#include <cctype>
#include <cstdlib>
#include <cstring>

#include <paperos/XhtmlParser.h>

namespace ereader {

namespace {

constexpr size_t kMaxPackageBytes = 2 * 1024 * 1024;

// One start, end or empty tag found by nextTag(). The name is lower case
// without its namespace prefix ("dc:title" -> "title").
struct XmlTag {
  std::string name;
  std::string attributes;
  bool closing = false;
  size_t contentStart = 0;  // just past the '>'
};

// Finds the next element tag at or after `pos`, skipping comments and
// declarations, and moves `pos` past it. Returns false at the end of the text.
bool nextTag(const std::string& xml, size_t& pos, XmlTag& tag) {
  for (;;) {
    const size_t open = xml.find('<', pos);
    if (open == std::string::npos) return false;

    if (xml.compare(open, 4, "<!--") == 0) {
      const size_t end = xml.find("-->", open + 4);
      if (end == std::string::npos) return false;
      pos = end + 3;
      continue;
    }

    // Find the closing '>', ignoring any inside attribute values.
    char quote = 0;
    size_t close = open + 1;
    for (; close < xml.size(); ++close) {
      const char c = xml[close];
      if (quote != 0) {
        if (c == quote) quote = 0;
      } else if (c == '"' || c == '\'') {
        quote = c;
      } else if (c == '>') {
        break;
      }
    }
    if (close >= xml.size()) return false;
    pos = close + 1;

    const char first = xml[open + 1];
    if (first == '!' || first == '?') continue;

    size_t nameStart = open + 1;
    tag.closing = first == '/';
    if (tag.closing) ++nameStart;
    size_t nameEnd = nameStart;
    while (nameEnd < close && !strchr(" \t\r\n/", xml[nameEnd])) ++nameEnd;

    tag.name = xml.substr(nameStart, nameEnd - nameStart);
    for (char& c : tag.name) {
      if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    }
    const size_t colon = tag.name.find(':');
    if (colon != std::string::npos) tag.name.erase(0, colon + 1);

    tag.attributes = xml.substr(nameEnd, close - nameEnd);
    tag.contentStart = pos;
    return true;
  }
}

// Value of an attribute, or "" if it is missing.
std::string attribute(const XmlTag& tag, const char* name) {
  const std::string& text = tag.attributes;
  const size_t nameSize = strlen(name);
  size_t pos = 0;
  while ((pos = text.find(name, pos)) != std::string::npos) {
    const bool startsName = pos == 0 || strchr(" \t\r\n", text[pos - 1]) != nullptr;
    size_t cursor = pos + nameSize;
    pos = cursor;
    if (!startsName) continue;

    while (cursor < text.size() && strchr(" \t\r\n", text[cursor])) ++cursor;
    if (cursor >= text.size() || text[cursor] != '=') continue;
    ++cursor;
    while (cursor < text.size() && strchr(" \t\r\n", text[cursor])) ++cursor;
    if (cursor >= text.size() || (text[cursor] != '"' && text[cursor] != '\'')) continue;
    const size_t end = text.find(text[cursor], cursor + 1);
    if (end == std::string::npos) return "";
    return text.substr(cursor + 1, end - cursor - 1);
  }
  return "";
}

std::string percentDecode(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '%' && i + 2 < text.size() && isxdigit(uint8_t(text[i + 1])) &&
        isxdigit(uint8_t(text[i + 2]))) {
      out += char(strtol(text.substr(i + 1, 2).c_str(), nullptr, 16));
      i += 2;
    } else {
      out += text[i];
    }
  }
  return out;
}

struct ManifestItem {
  std::string id;
  std::string href;
  std::string mediaType;
  std::string properties;
};

// File extension for an image the display can draw, or "" for anything else.
const char* imageExtension(const ManifestItem& item) {
  if (item.mediaType == "image/jpeg" || item.mediaType == "image/jpg") return ".jpg";
  if (item.mediaType == "image/png") return ".png";
  return "";
}

// Whether a space-separated list, such as a properties attribute, has `token`.
bool hasToken(const std::string& list, const char* token) {
  const size_t size = strlen(token);
  size_t pos = 0;
  while ((pos = list.find(token, pos)) != std::string::npos) {
    const bool starts = pos == 0 || isspace(uint8_t(list[pos - 1]));
    const bool ends = pos + size == list.size() || isspace(uint8_t(list[pos + size]));
    if (starts && ends) return true;
    pos += size;
  }
  return false;
}

bool mentionsCover(std::string text) {
  for (char& c : text) c = char(tolower(uint8_t(c)));
  return text.find("cover") != std::string::npos;
}

// The manifest's cover image, or nullptr. `coverId` is the item named by an
// EPUB 2 <meta name="cover">, "" if there is none.
const ManifestItem* findCover(const std::vector<ManifestItem>& manifest,
                              const std::string& coverId) {
  for (const ManifestItem& item : manifest) {
    if (hasToken(item.properties, "cover-image") && imageExtension(item)[0] != 0) return &item;
  }
  for (const ManifestItem& item : manifest) {
    if (!coverId.empty() && item.id == coverId && imageExtension(item)[0] != 0) return &item;
  }
  // Books that mark it neither way still tend to call it "cover".
  for (const ManifestItem& item : manifest) {
    if (imageExtension(item)[0] != 0 && (mentionsCover(item.id) || mentionsCover(item.href))) {
      return &item;
    }
  }
  return nullptr;
}

}  // namespace

std::string resolveHref(const std::string& basePath, const std::string& href) {
  const size_t lastSlash = basePath.rfind('/');
  std::string path = lastSlash == std::string::npos ? "" : basePath.substr(0, lastSlash + 1);
  path += percentDecode(href.substr(0, href.find('#')));

  std::vector<std::string> parts;
  size_t start = 0;
  while (start <= path.size()) {
    size_t end = path.find('/', start);
    if (end == std::string::npos) end = path.size();
    const std::string part = path.substr(start, end - start);
    if (part == "..") {
      if (!parts.empty()) parts.pop_back();
    } else if (!part.empty() && part != ".") {
      parts.push_back(part);
    }
    start = end + 1;
  }

  std::string resolved;
  for (const std::string& part : parts) {
    if (!resolved.empty()) resolved += '/';
    resolved += part;
  }
  return resolved;
}

bool Epub::open(std::unique_ptr<paperos::File> file) {
  chapters_.clear();
  title_.clear();
  author_.clear();
  coverExtension_.clear();
  if (!zip_.open(std::move(file))) return false;

  const ZipEntry* container = zip_.find("META-INF/container.xml");
  std::string xml;
  if (container == nullptr || !zip_.readToString(*container, xml, kMaxPackageBytes)) return false;

  std::string packagePath;
  size_t pos = 0;
  XmlTag tag;
  while (packagePath.empty() && nextTag(xml, pos, tag)) {
    if (!tag.closing && tag.name == "rootfile") packagePath = attribute(tag, "full-path");
  }
  if (packagePath.empty()) return false;

  const ZipEntry* package = zip_.find(packagePath);
  if (package == nullptr || !zip_.readToString(*package, xml, kMaxPackageBytes)) return false;
  return parsePackage(packagePath, xml);
}

bool Epub::parsePackage(const std::string& packagePath, const std::string& xml) {
  std::vector<ManifestItem> manifest;
  std::vector<std::string> spine;
  std::string coverId;

  size_t pos = 0;
  XmlTag tag;
  while (nextTag(xml, pos, tag)) {
    if (tag.closing) continue;
    if (tag.name == "item") {
      manifest.push_back({attribute(tag, "id"), attribute(tag, "href"),
                          attribute(tag, "media-type"), attribute(tag, "properties")});
    } else if (tag.name == "meta") {
      if (attribute(tag, "name") == "cover") coverId = attribute(tag, "content");
    } else if (tag.name == "itemref") {
      // Non-linear items are auxiliary, such as pop-up notes.
      if (attribute(tag, "linear") != "no") spine.push_back(attribute(tag, "idref"));
    } else if (tag.name == "title" || tag.name == "creator") {
      std::string& field = tag.name == "title" ? title_ : author_;
      if (field.empty()) {
        const size_t end = xml.find('<', tag.contentStart);
        if (end != std::string::npos) {
          field = paperos::xmlTextToAscii(xml.substr(tag.contentStart, end - tag.contentStart));
        }
      }
    }
  }

  // A spine item that is missing from the archive is skipped rather than
  // failing the whole book; a few real-world EPUBs have dangling references.
  for (const std::string& id : spine) {
    for (const ManifestItem& item : manifest) {
      if (item.id != id) continue;
      if (item.mediaType.compare(0, 6, "image/") == 0) break;
      const ZipEntry* entry = zip_.find(resolveHref(packagePath, item.href));
      if (entry != nullptr) chapters_.push_back(*entry);
      break;
    }
  }

  if (const ManifestItem* item = findCover(manifest, coverId)) {
    const ZipEntry* entry = zip_.find(resolveHref(packagePath, item->href));
    if (entry != nullptr) {
      cover_ = *entry;
      coverExtension_ = imageExtension(*item);
    }
  }
  return !chapters_.empty();
}

bool Epub::readCover(const ByteSink& sink) { return hasCover() && zip_.read(cover_, sink); }

bool Epub::readChapter(size_t index, const ByteSink& sink) {
  return zip_.read(chapters_[index], sink);
}

}  // namespace ereader

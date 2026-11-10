#pragma once

#include <cstdint>
#include <string>

namespace paperos {

// Closest ASCII spelling of a Unicode code point, because the display fonts
// are ASCII-only: curly quotes become straight, accented letters lose their
// accent, "ß" becomes "ss". Returns "" for invisible characters and "?" for
// anything with no ASCII equivalent, such as Cyrillic or CJK.
// The result is only valid until the next call.
const char* foldToAscii(uint32_t codePoint);

// Folds every character of UTF-8 `text` with foldToAscii(). Malformed bytes
// become "?".
std::string foldUtf8ToAscii(const std::string& text);

}  // namespace paperos

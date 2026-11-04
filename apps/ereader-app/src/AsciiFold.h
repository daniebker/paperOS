#pragma once

#include <cstdint>

namespace ereader {

// Closest ASCII spelling of a Unicode code point, because the display fonts
// are ASCII-only: curly quotes become straight, accented letters lose their
// accent, "ß" becomes "ss". Returns "" for invisible characters and "?" for
// anything with no ASCII equivalent, such as Cyrillic or CJK.
// The result is only valid until the next call.
const char* foldToAscii(uint32_t codePoint);

}  // namespace ereader

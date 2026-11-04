#include "AsciiFold.h"

namespace ereader {

namespace {

// Base letter for each code point from U+00C0 to U+017F, generated from the
// Unicode decompositions. Letters that need more than one character ('?'
// here) are handled in multiLetter().
const char kLatinBaseLetters[] =
    "AAAAAA?CEEEEIIIIDNOOOOOxOUUUUY??"
    "aaaaaa?ceeeeiiiidnooooo/ouuuuy?y"
    "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGg"
    "GgGgHhHhIiIiIiIiIi??JjKkkLlLlLlL"
    "lLlNnNnNnnNnOoOoOo??RrRrRrSsSsSs"
    "SsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";

const char* multiLetter(uint32_t cp) {
  switch (cp) {
    case 0x00C6: return "AE";
    case 0x00DE: return "Th";
    case 0x00DF: return "ss";
    case 0x00E6: return "ae";
    case 0x00FE: return "th";
    case 0x0132: return "IJ";
    case 0x0133: return "ij";
    case 0x0152: return "OE";
    case 0x0153: return "oe";
    default: return nullptr;
  }
}

const char* foldLatin1Symbol(uint32_t cp) {
  switch (cp) {
    case 0x00A0: return " ";
    case 0x00A1: return "!";
    case 0x00A2: return "c";
    case 0x00A3: return "GBP";
    case 0x00A5: return "JPY";
    case 0x00A7: return "S.";
    case 0x00A9: return "(c)";
    case 0x00AB: return "\"";
    case 0x00AD: return "";  // soft hyphen
    case 0x00AE: return "(R)";
    case 0x00B0: return " deg";
    case 0x00B4: return "'";
    case 0x00B7: return ".";
    case 0x00BB: return "\"";
    case 0x00BC: return "1/4";
    case 0x00BD: return "1/2";
    case 0x00BE: return "3/4";
    case 0x00BF: return "?";
    default: return "?";
  }
}

const char* foldGeneralPunctuation(uint32_t cp) {
  if (cp <= 0x200A) return " ";  // en quad .. hair space
  if (cp <= 0x200F) return "";   // zero-width and direction marks
  if (cp <= 0x2013) return "-";  // hyphens, en dash
  if (cp <= 0x2015) return "--";  // em dash, horizontal bar
  if (cp >= 0x2018 && cp <= 0x201B) return "'";
  if (cp >= 0x201C && cp <= 0x201F) return "\"";
  switch (cp) {
    case 0x2020:
    case 0x2021: return "+";
    case 0x2022: return "*";
    case 0x2026: return "...";
    case 0x2028:
    case 0x2029:
    case 0x202F:
    case 0x205F: return " ";
    case 0x2032: return "'";
    case 0x2033: return "\"";
    case 0x2039: return "<";
    case 0x203A: return ">";
    case 0x2060: return "";
    default: return "?";
  }
}

}  // namespace

const char* foldToAscii(uint32_t cp) {
  static char single[2] = {0, 0};

  if (cp < 0x20 || cp == 0x7F) return "";
  if (cp < 0x7F) {
    single[0] = char(cp);
    return single;
  }
  if (cp < 0xA0) return "";  // C1 control codes
  if (cp < 0xC0) return foldLatin1Symbol(cp);
  if (cp < 0x180) {
    if (const char* letters = multiLetter(cp)) return letters;
    single[0] = kLatinBaseLetters[cp - 0xC0];
    return single;
  }
  if (cp >= 0x0300 && cp <= 0x036F) return "";  // combining accents
  if (cp >= 0x2000 && cp <= 0x206F) return foldGeneralPunctuation(cp);
  switch (cp) {
    case 0x20AC: return "EUR";
    case 0x2122: return "(TM)";
    case 0x2212: return "-";
    case 0x3000: return " ";
    case 0xFB00: return "ff";
    case 0xFB01: return "fi";
    case 0xFB02: return "fl";
    case 0xFB03: return "ffi";
    case 0xFB04: return "ffl";
    case 0xFEFF: return "";  // byte order mark
    default: return "?";
  }
}

}  // namespace ereader

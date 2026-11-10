#include "Inflate.h"

#include <algorithm>
#include <cstring>
#include <memory>

namespace ereader {

namespace {

constexpr size_t kWindowSize = 32768;  // the maximum DEFLATE back-reference
constexpr size_t kWindowMask = kWindowSize - 1;
constexpr size_t kInputBufferSize = 1024;
constexpr int kMaxCodeBits = 15;
constexpr int kMaxLiteralCodes = 286;
constexpr int kMaxDistanceCodes = 30;
constexpr int kFixedLiteralCodes = 288;
constexpr int kCodeLengthCodes = 19;

constexpr uint16_t kLengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                      31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr uint8_t kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                      2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr uint16_t kDistanceBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,
                                        33,  49,  65,  97,  129, 193,  257,  385,  513,  769,
                                        1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr uint8_t kDistanceExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                        6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
constexpr uint8_t kCodeLengthOrder[kCodeLengthCodes] = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                                        11, 4,  12, 3, 13, 2, 14, 1, 15};

// Canonical Huffman code: how many codes have each length, and the symbols
// ordered by code.
struct Huffman {
  uint16_t count[kMaxCodeBits + 1];
  uint16_t symbol[kFixedLiteralCodes];
};

// Heap allocated by inflate(): the tables are too big for the loop task's stack.
class Inflater {
 public:
  Inflater(paperos::File& in, size_t compressedSize, const ByteSink& sink)
      : in_(in), remaining_(compressedSize), sink_(sink), window_(new uint8_t[kWindowSize]) {}

  bool run();

 private:
  uint8_t nextByte();
  uint32_t bits(int count);
  int bit();
  int decode(const Huffman& code);
  void put(uint8_t byte);
  void flush(size_t size);

  bool build(Huffman& code, const uint8_t* lengths, int symbols);
  bool stored();
  bool fixed();
  bool dynamic();
  bool codes();

  paperos::File& in_;
  size_t remaining_;
  const ByteSink& sink_;
  std::unique_ptr<uint8_t[]> window_;
  uint64_t total_ = 0;  // bytes produced so far

  uint8_t input_[kInputBufferSize];
  size_t inputPos_ = 0;
  size_t inputLen_ = 0;
  uint32_t bitBuffer_ = 0;
  int bitCount_ = 0;
  bool failed_ = false;

  Huffman literals_;
  Huffman distances_;
  uint8_t lengths_[kMaxLiteralCodes + kMaxDistanceCodes + kFixedLiteralCodes];
};

uint8_t Inflater::nextByte() {
  if (inputPos_ == inputLen_) {
    inputPos_ = 0;
    inputLen_ = remaining_ == 0 ? 0 : in_.read(input_, std::min(remaining_, kInputBufferSize));
    remaining_ -= inputLen_;
    if (inputLen_ == 0) {
      failed_ = true;
      return 0;
    }
  }
  return input_[inputPos_++];
}

uint32_t Inflater::bits(int count) {
  uint32_t value = bitBuffer_;
  while (bitCount_ < count) {
    value |= uint32_t(nextByte()) << bitCount_;
    bitCount_ += 8;
  }
  bitBuffer_ = value >> count;
  bitCount_ -= count;
  return value & ((1u << count) - 1);
}

int Inflater::bit() {
  if (bitCount_ == 0) {
    bitBuffer_ = nextByte();
    bitCount_ = 8;
  }
  const int value = bitBuffer_ & 1;
  bitBuffer_ >>= 1;
  --bitCount_;
  return value;
}

int Inflater::decode(const Huffman& code) {
  int value = 0;
  int first = 0;
  int index = 0;
  for (int length = 1; length <= kMaxCodeBits; ++length) {
    value |= bit();
    const int count = code.count[length];
    if (value - count < first) return code.symbol[index + (value - first)];
    index += count;
    first = (first + count) << 1;
    value <<= 1;
  }
  return -1;
}

void Inflater::flush(size_t size) {
  if (size > 0 && !sink_(window_.get(), size)) failed_ = true;
}

void Inflater::put(uint8_t byte) {
  window_[total_ & kWindowMask] = byte;
  ++total_;
  if ((total_ & kWindowMask) == 0) flush(kWindowSize);
}

bool Inflater::build(Huffman& code, const uint8_t* lengths, int symbols) {
  memset(code.count, 0, sizeof(code.count));
  for (int i = 0; i < symbols; ++i) ++code.count[lengths[i]];
  code.count[0] = 0;

  int left = 1;
  uint16_t offsets[kMaxCodeBits + 2] = {};
  for (int length = 1; length <= kMaxCodeBits; ++length) {
    left = (left << 1) - code.count[length];
    if (left < 0) return false;  // over-subscribed
    offsets[length + 1] = offsets[length] + code.count[length];
  }
  for (int i = 0; i < symbols; ++i) {
    if (lengths[i] != 0) code.symbol[offsets[lengths[i]]++] = uint16_t(i);
  }
  return true;
}

bool Inflater::stored() {
  bitBuffer_ = 0;
  bitCount_ = 0;
  const uint32_t length = nextByte() | (uint32_t(nextByte()) << 8);
  const uint32_t check = nextByte() | (uint32_t(nextByte()) << 8);
  if (failed_ || length != (~check & 0xFFFF)) return false;
  for (uint32_t i = 0; i < length && !failed_; ++i) put(nextByte());
  return !failed_;
}

bool Inflater::fixed() {
  int symbol = 0;
  for (; symbol < 144; ++symbol) lengths_[symbol] = 8;
  for (; symbol < 256; ++symbol) lengths_[symbol] = 9;
  for (; symbol < 280; ++symbol) lengths_[symbol] = 7;
  for (; symbol < kFixedLiteralCodes; ++symbol) lengths_[symbol] = 8;
  if (!build(literals_, lengths_, kFixedLiteralCodes)) return false;

  memset(lengths_, 5, kMaxDistanceCodes);
  return build(distances_, lengths_, kMaxDistanceCodes) && codes();
}

bool Inflater::dynamic() {
  const int literalCount = int(bits(5)) + 257;
  const int distanceCount = int(bits(5)) + 1;
  const int codeLengthCount = int(bits(4)) + 4;
  if (literalCount > kMaxLiteralCodes || distanceCount > kMaxDistanceCodes) return false;

  memset(lengths_, 0, kCodeLengthCodes);
  for (int i = 0; i < codeLengthCount; ++i) lengths_[kCodeLengthOrder[i]] = uint8_t(bits(3));
  if (!build(literals_, lengths_, kCodeLengthCodes)) return false;

  // The code lengths for both tables are themselves Huffman coded, with
  // symbols 16-18 meaning "repeat".
  const int total = literalCount + distanceCount;
  int index = 0;
  while (index < total) {
    const int symbol = decode(literals_);
    if (failed_ || symbol < 0) return false;
    if (symbol < 16) {
      lengths_[index++] = uint8_t(symbol);
      continue;
    }
    uint8_t previous = 0;
    int repeat = 0;
    if (symbol == 16) {
      if (index == 0) return false;
      previous = lengths_[index - 1];
      repeat = 3 + int(bits(2));
    } else if (symbol == 17) {
      repeat = 3 + int(bits(3));
    } else {
      repeat = 11 + int(bits(7));
    }
    if (index + repeat > total) return false;
    while (repeat-- > 0) lengths_[index++] = previous;
  }
  if (lengths_[256] == 0) return false;  // no end-of-block code

  return build(literals_, lengths_, literalCount) &&
         build(distances_, lengths_ + literalCount, distanceCount) && codes();
}

bool Inflater::codes() {
  for (;;) {
    int symbol = decode(literals_);
    if (failed_ || symbol < 0) return false;
    if (symbol < 256) {
      put(uint8_t(symbol));
      continue;
    }
    if (symbol == 256) return true;

    symbol -= 257;
    if (symbol >= 29) return false;
    int length = kLengthBase[symbol] + int(bits(kLengthExtra[symbol]));

    const int distanceSymbol = decode(distances_);
    if (failed_ || distanceSymbol < 0 || distanceSymbol >= kMaxDistanceCodes) return false;
    const uint32_t distance =
        kDistanceBase[distanceSymbol] + bits(kDistanceExtra[distanceSymbol]);
    if (distance > total_) return false;

    while (length-- > 0 && !failed_) put(window_[(total_ - distance) & kWindowMask]);
  }
}

bool Inflater::run() {
  int last = 0;
  do {
    last = int(bits(1));
    const uint32_t type = bits(2);
    bool ok = false;
    switch (type) {
      case 0: ok = stored(); break;
      case 1: ok = fixed(); break;
      case 2: ok = dynamic(); break;
      default: break;
    }
    if (!ok || failed_) return false;
  } while (!last);

  flush(size_t(total_ & kWindowMask));
  return !failed_;
}

}  // namespace

bool inflate(paperos::File& in, size_t compressedSize, const ByteSink& sink) {
  std::unique_ptr<Inflater> inflater(new Inflater(in, compressedSize, sink));
  return inflater->run();
}

}  // namespace ereader

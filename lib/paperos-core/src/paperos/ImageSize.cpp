#include "paperos/ImageSize.h"

#include <algorithm>

namespace paperos {

namespace {

uint32_t be16(const uint8_t* p) { return uint32_t(p[0]) << 8 | p[1]; }
uint32_t be32(const uint8_t* p) { return be16(p) << 16 | be16(p + 2); }
uint32_t le32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

bool readAt(File& file, uint32_t position, uint8_t* buffer, size_t size) {
  return file.seek(position) && file.read(buffer, size) == size;
}

// Walks the JPEG segments to the start-of-frame, which holds the size. EXIF
// data and thumbnails come before it, so it can be well into the file.
bool jpegSize(File& file, int& width, int& height) {
  uint32_t position = 2;  // after the FFD8 start-of-image marker
  uint8_t head[9];
  for (int segments = 0; segments < 256; ++segments) {
    if (!readAt(file, position, head, 4) || head[0] != 0xFF) return false;
    const uint8_t marker = head[1];
    if (marker == 0xFF) {  // padding
      ++position;
      continue;
    }
    const uint32_t length = be16(head + 2);
    if (length < 2) return false;
    // SOF0 to SOF15, except DHT (C4), JPG (C8) and DAC (CC).
    const bool startOfFrame =
        marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
    if (startOfFrame) {
      if (!readAt(file, position + 4, head, 5)) return false;
      height = int(be16(head + 1));
      width = int(be16(head + 3));
      return width > 0 && height > 0;
    }
    if (marker == 0xD9 || marker == 0xDA) return false;  // end of image, or data before a frame
    position += 2 + length;
  }
  return false;
}

}  // namespace

bool readImageSize(File& file, int& width, int& height) {
  uint8_t head[26];
  if (!readAt(file, 0, head, 2)) return false;

  if (head[0] == 0xFF && head[1] == 0xD8) return jpegSize(file, width, height);

  if (head[0] == 0x89 && head[1] == 'P') {  // PNG: the IHDR chunk comes first
    if (!readAt(file, 0, head, 24) || head[2] != 'N' || head[3] != 'G') return false;
    width = int(be32(head + 16));
    height = int(be32(head + 20));
    return width > 0 && height > 0;
  }

  if (head[0] == 'B' && head[1] == 'M') {  // BMP: BITMAPINFOHEADER after the file header
    if (!readAt(file, 0, head, 26)) return false;
    width = int(int32_t(le32(head + 18)));
    height = int(int32_t(le32(head + 22)));
    if (height < 0) height = -height;  // stored top-down
    return width > 0 && height > 0;
  }
  return false;
}

float coverScale(int imageW, int imageH, int screenW, int screenH) {
  if (imageW <= 0 || imageH <= 0) return 0.0f;
  return std::max(float(screenW) / float(imageW), float(screenH) / float(imageH));
}

}  // namespace paperos

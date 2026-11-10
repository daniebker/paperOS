#pragma once

#include <cstdint>

#include "paperos/FileSystem.h"

namespace paperos {

// Reads an image's width and height from its header, without decoding it.
// Supports JPEG, PNG and BMP, told apart by their content rather than the
// file name. Returns false if the file isn't one of those or is cut short.
bool readImageSize(File& file, int& width, int& height);

// Scale that makes an image of `imageW` x `imageH` cover a `screenW` x
// `screenH` screen, keeping its shape; whatever overflows is cropped.
float coverScale(int imageW, int imageH, int screenW, int screenH);

}  // namespace paperos

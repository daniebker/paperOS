#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

#include <paperos.h>

namespace ereader {

// Receives decompressed data in chunks. Return false to stop early.
using ByteSink = std::function<bool(const uint8_t* data, size_t size)>;

// Decompresses `compressedSize` bytes of raw DEFLATE data from the current
// position of `in`. Output is streamed through a 32 KB window, so an entry of
// any size can be read without holding it all in memory. Returns false if
// the data is corrupt or `sink` stopped.
bool inflate(paperos::File& in, size_t compressedSize, const ByteSink& sink);

}  // namespace ereader

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <vector>

#include "transport/base/TransportTypes.h"

namespace rtb {

// V0.1 keeps the bitmap pipeline intentionally small: grayscale threshold,
// 1bpp MSB-first row packing and bit mirroring. Image decoding / dithering is
// LATER (borrowed from mature projects, see docs/OPEN_SOURCE_AUDIT.md).
Result packGrayscale(const uint8_t* gray, size_t width, size_t height,
                     uint8_t threshold, std::vector<uint8_t>& rows,
                     size_t& row_bytes);

void mirrorRow(const uint8_t* in, uint8_t* out, size_t len);
void makeTestPattern(size_t width_bytes, size_t row_count,
                     std::vector<uint8_t>& rows);

}  // namespace rtb

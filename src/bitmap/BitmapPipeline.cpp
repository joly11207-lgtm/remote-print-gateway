#include "bitmap/BitmapPipeline.h"

#include <string.h>

#include "protocols/tiny_iprint/TinyProtocol.h"

namespace rtb {

Result packGrayscale(const uint8_t* gray, size_t width, size_t height,
                     uint8_t threshold, std::vector<uint8_t>& rows,
                     size_t& row_bytes) {
  if (!gray || width == 0 || height == 0) return Result::InvalidArgument;
  row_bytes = (width + 7) / 8;
  rows.assign(row_bytes * height, 0);
  for (size_t y = 0; y < height; ++y) {
    for (size_t x = 0; x < width; ++x) {
      const uint8_t pixel = gray[y * width + x];
      if (pixel < threshold) {  // dark pixel -> 1 bit
        rows[y * row_bytes + x / 8] =
            static_cast<uint8_t>(rows[y * row_bytes + x / 8] | (0x80u >> (x % 8)));
      }
    }
  }
  return Result::Ok;
}

void mirrorRow(const uint8_t* in, uint8_t* out, size_t len) {
  if (!in || !out) return;
  for (size_t i = 0; i < len; ++i) out[i] = TinyProtocol::mirrorByte(in[i]);
}

void makeTestPattern(size_t width_bytes, size_t row_count,
                     std::vector<uint8_t>& rows) {
  rows.assign(width_bytes * row_count, 0);
  for (size_t y = 0; y < row_count; ++y) {
    for (size_t x = 0; x < width_bytes; ++x) {
      if (y == 0 || y + 1 == row_count) {
        rows[y * width_bytes + x] = 0xFF;
      } else {
        rows[y * width_bytes + x] = ((x + y) & 1) ? 0xAA : 0x55;
      }
    }
  }
}

}  // namespace rtb

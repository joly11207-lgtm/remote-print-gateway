#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace rtb {

bool base64Decode(const std::string& input, std::vector<uint8_t>& out);

// Upper bound on the decoded size for `encoded_len` input characters. Used to
// size the destination ONCE, before decoding, instead of growing a vector byte
// by byte (repeated realloc while Wi-Fi + BLE already hold the heap).
size_t base64MaxDecodedSize(size_t encoded_len);

// In-place decode into a caller-owned buffer. No allocation at all.
// `data`/`len` may point straight into an HTTP body. Returns false on an
// invalid character, on a truncated final group, or if capacity is exceeded.
bool base64DecodeInto(const char* data, size_t len, uint8_t* out,
                      size_t capacity, size_t& out_len);

std::string base64Encode(const uint8_t* data, size_t len);

}  // namespace rtb

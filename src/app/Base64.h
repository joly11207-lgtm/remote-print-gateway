#pragma once

#include <stdint.h>
#include <string>
#include <vector>

namespace rtb {

bool base64Decode(const std::string& input, std::vector<uint8_t>& out);
std::string base64Encode(const uint8_t* data, size_t len);

}  // namespace rtb

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <string>

namespace rtb {

inline uint32_t fnv1a32(const uint8_t* data, size_t len) {
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < len; ++i) {
    hash ^= data[i];
    hash *= 16777619u;
  }
  return hash;
}

inline std::string hashKey(uint32_t hash) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%08X", hash);
  return std::string(buf);
}

}  // namespace rtb

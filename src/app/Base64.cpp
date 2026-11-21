#include "app/Base64.h"

namespace rtb {
namespace {

int decodeChar(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

}  // namespace

bool base64Decode(const std::string& input, std::vector<uint8_t>& out) {
  out.clear();
  uint32_t buffer = 0;
  int bits = 0;
  for (size_t i = 0; i < input.size(); ++i) {
    const char c = input[i];
    if (c == '=' || c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
    const int v = decodeChar(c);
    if (v < 0) return false;
    buffer = (buffer << 6) | static_cast<uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<uint8_t>((buffer >> bits) & 0xFF));
    }
  }
  return true;
}

size_t base64MaxDecodedSize(size_t encoded_len) {
  // 4 encoded characters carry 3 bytes; a trailing partial group of 2 or 3
  // characters carries 1 or 2 bytes. Padding and whitespace only ever REDUCE
  // the output, so this is a strict upper bound (over-estimates by at most 2
  // bytes, which the caller must tolerate when comparing against a hard limit).
  const size_t groups = encoded_len / 4;
  const size_t rest = encoded_len % 4;
  size_t bytes = groups * 3;
  if (rest >= 2) bytes += rest - 1;
  return bytes;
}

bool base64DecodeInto(const char* data, size_t len, uint8_t* out,
                      size_t capacity, size_t& out_len) {
  out_len = 0;
  if (!data || (!out && capacity > 0)) return false;
  uint32_t buffer = 0;
  int bits = 0;
  for (size_t i = 0; i < len; ++i) {
    const char c = data[i];
    if (c == '=' || c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
    const int v = decodeChar(c);
    if (v < 0) return false;
    buffer = (buffer << 6) | static_cast<uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (out_len >= capacity) return false;
      out[out_len++] = static_cast<uint8_t>((buffer >> bits) & 0xFF);
    }
  }
  // Well-formed base64 ends with 0, 2 or 4 leftover bits. 6 means the final
  // quartet was truncated mid-character.
  if (bits >= 6) return false;
  return true;
}

std::string base64Encode(const uint8_t* data, size_t len) {
  static const char* kTable =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  if (!data) return out;
  size_t i = 0;
  while (i + 3 <= len) {
    const uint32_t v = (static_cast<uint32_t>(data[i]) << 16) |
                       (static_cast<uint32_t>(data[i + 1]) << 8) | data[i + 2];
    out += kTable[(v >> 18) & 63];
    out += kTable[(v >> 12) & 63];
    out += kTable[(v >> 6) & 63];
    out += kTable[v & 63];
    i += 3;
  }
  const size_t rest = len - i;
  if (rest == 1) {
    const uint32_t v = static_cast<uint32_t>(data[i]) << 16;
    out += kTable[(v >> 18) & 63];
    out += kTable[(v >> 12) & 63];
    out += "==";
  } else if (rest == 2) {
    const uint32_t v = (static_cast<uint32_t>(data[i]) << 16) |
                       (static_cast<uint32_t>(data[i + 1]) << 8);
    out += kTable[(v >> 18) & 63];
    out += kTable[(v >> 12) & 63];
    out += kTable[(v >> 6) & 63];
    out += "=";
  }
  return out;
}

}  // namespace rtb

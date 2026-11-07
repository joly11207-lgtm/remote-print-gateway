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

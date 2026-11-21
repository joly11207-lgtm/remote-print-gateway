#include "app/JsonMini.h"

#include <stdlib.h>
#include <string.h>

namespace rtb {
namespace {

bool matchKeyAt(const char* json, size_t len, size_t pos, const char* key) {
  const size_t key_len = strlen(key);
  if (pos + key_len + 2 > len) return false;
  if (json[pos] != '"') return false;
  if (memcmp(json + pos + 1, key, key_len) != 0) return false;
  return json[pos + 1 + key_len] == '"';
}

// Position just after `"key"`, or npos when absent. No allocation.
size_t findKeyIn(const char* json, size_t len, const char* key) {
  if (!json || !key) return static_cast<size_t>(-1);
  const size_t key_len = strlen(key);
  if (key_len == 0 || len < key_len + 2) return static_cast<size_t>(-1);
  for (size_t i = 0; i + key_len + 2 <= len; ++i) {
    if (json[i] == '"' && matchKeyAt(json, len, i, key)) return i + key_len + 2;
  }
  return static_cast<size_t>(-1);
}

size_t valueStartIn(const char* json, size_t len, size_t key_end) {
  size_t colon = key_end;
  while (colon < len && json[colon] != ':') {
    // Stop at the next key: a ':' belonging to a later key must not be used.
    if (json[colon] == '"') return static_cast<size_t>(-1);
    ++colon;
  }
  if (colon >= len) return static_cast<size_t>(-1);
  ++colon;
  while (colon < len && (json[colon] == ' ' || json[colon] == '\t' ||
                         json[colon] == '\n' || json[colon] == '\r')) {
    ++colon;
  }
  return colon;
}

const size_t kNpos = static_cast<size_t>(-1);

}  // namespace

std::string jsonEscape(const char* value) {
  std::string out;
  for (const char* p = value ? value : ""; *p; ++p) {
    if (*p == '"' || *p == '\\') out += '\\';
    out += *p;
  }
  return out;
}

JsonFieldSlice jsonStringSliceIn(const char* json, size_t len, const char* key) {
  JsonFieldSlice out;
  out.found = false;
  out.data = nullptr;
  out.size = 0;
  out.escaped = false;

  const size_t key_end = findKeyIn(json, len, key);
  if (key_end == kNpos) return out;
  const size_t start = valueStartIn(json, len, key_end);
  if (start == kNpos || start >= len || json[start] != '"') return out;

  // Scan to the closing quote WITHOUT building anything. A backslash only sets
  // a flag: the caller must reject the field rather than unescape it into a
  // fresh string (base64 never legitimately contains escapes).
  size_t i = start + 1;
  while (i < len) {
    if (json[i] == '\\') {
      out.escaped = true;
      i += 2;
      continue;
    }
    if (json[i] == '"') {
      out.found = true;
      out.data = json + start + 1;
      out.size = i - (start + 1);
      return out;
    }
    ++i;
  }
  return out;  // unterminated string: treat as absent
}

std::string jsonStringFieldIn(const char* json, size_t len, const char* key) {
  const JsonFieldSlice slice = jsonStringSliceIn(json, len, key);
  if (!slice.found) return "";
  std::string out;
  out.reserve(slice.size);
  for (size_t i = 0; i < slice.size; ++i) {
    const char c = slice.data[i];
    if (c == '\\' && i + 1 < slice.size) {
      ++i;
      out += slice.data[i];
    } else {
      out += c;
    }
  }
  return out;
}

bool jsonBoolFieldIn(const char* json, size_t len, const char* key, bool fallback) {
  const size_t key_end = findKeyIn(json, len, key);
  if (key_end == kNpos) return fallback;
  const size_t start = valueStartIn(json, len, key_end);
  if (start == kNpos || start >= len) return fallback;
  if (start + 4 <= len && memcmp(json + start, "true", 4) == 0) return true;
  if (start + 5 <= len && memcmp(json + start, "false", 5) == 0) return false;
  return fallback;
}

int jsonIntFieldIn(const char* json, size_t len, const char* key, int fallback) {
  const size_t key_end = findKeyIn(json, len, key);
  if (key_end == kNpos) return fallback;
  const size_t start = valueStartIn(json, len, key_end);
  if (start == kNpos || start >= len) return fallback;
  if (json[start] != '-' && (json[start] < '0' || json[start] > '9')) return fallback;
  // Parse in place: no null-terminated copy of the document is required.
  bool negative = false;
  size_t i = start;
  if (json[i] == '-') {
    negative = true;
    ++i;
  }
  // The range check runs BEFORE the multiply. `long` is 32 bits on the ESP32
  // (and in the host builds), so "value * 10 + digit" would itself be signed
  // overflow (UB) for inputs like 3000000000. Out-of-range numbers are
  // reported as absent (the caller's fallback), never as a wrapped value.
  long value = 0;
  while (i < len && json[i] >= '0' && json[i] <= '9') {
    const long digit = json[i] - '0';
    if (value > (0x7FFFFFFFl - digit) / 10) return fallback;
    value = value * 10 + digit;
    ++i;
  }
  return static_cast<int>(negative ? -value : value);
}

// ---- std::string convenience overloads (short fields / tests) --------------

JsonFieldSlice jsonStringSlice(const std::string& json, const char* key) {
  return jsonStringSliceIn(json.data(), json.size(), key);
}

std::string jsonStringField(const std::string& json, const char* key) {
  return jsonStringFieldIn(json.data(), json.size(), key);
}

bool jsonBoolField(const std::string& json, const char* key, bool fallback) {
  return jsonBoolFieldIn(json.data(), json.size(), key, fallback);
}

int jsonIntField(const std::string& json, const char* key, int fallback) {
  return jsonIntFieldIn(json.data(), json.size(), key, fallback);
}

}  // namespace rtb

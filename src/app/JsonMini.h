#pragma once

#include <stddef.h>

#include <string>

namespace rtb {

// Minimal, dependency-free helpers for the flat JSON bodies accepted by the
// V0.1 HTTP debug API. Not a general JSON parser.
//
// Two families:
//   * `...In(const char*, size_t, ...)` operate on a caller-owned buffer with
//     NO allocation and no copy. The HTTP layer uses these so the 4 KB
//     rows_b64 field never becomes a second string.
//   * the std::string overloads delegate to them, for tests and short fields.
//
// A location inside an existing JSON document. `data` points INTO the document,
// so the caller must keep the source buffer alive.
// C++11 only: no std::string_view.
struct JsonFieldSlice {
  bool found;        // key present AND value is a properly terminated string
  const char* data;  // first character of the value (inside the source buffer)
  size_t size;       // number of raw characters between the quotes
  bool escaped;      // raw span contains a backslash escape -> not usable in place
};

JsonFieldSlice jsonStringSliceIn(const char* json, size_t len, const char* key);
JsonFieldSlice jsonStringSlice(const std::string& json, const char* key);

// Short fields: a copy is fine and the escape handling is kept.
std::string jsonStringFieldIn(const char* json, size_t len, const char* key);
std::string jsonStringField(const std::string& json, const char* key);

bool jsonBoolFieldIn(const char* json, size_t len, const char* key, bool fallback);
bool jsonBoolField(const std::string& json, const char* key, bool fallback);

int jsonIntFieldIn(const char* json, size_t len, const char* key, int fallback);
int jsonIntField(const std::string& json, const char* key, int fallback);

std::string jsonEscape(const char* value);

}  // namespace rtb

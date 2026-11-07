#include "app/JsonMini.h"

#include <stdlib.h>

namespace rtb {
namespace {

size_t findKey(const std::string& json, const char* key) {
  const std::string needle = std::string("\"") + key + "\"";
  return json.find(needle);
}

size_t valueStart(const std::string& json, size_t key_pos) {
  size_t colon = json.find(':', key_pos);
  if (colon == std::string::npos) return std::string::npos;
  ++colon;
  while (colon < json.size() &&
         (json[colon] == ' ' || json[colon] == '\t' || json[colon] == '\n' ||
          json[colon] == '\r')) ++colon;
  return colon;
}

}  // namespace

std::string jsonEscape(const char* value) {
  std::string out;
  for (const char* p = value ? value : ""; *p; ++p) {
    if (*p == '"' || *p == '\\') out += '\\';
    out += *p;
  }
  return out;
}

std::string jsonStringField(const std::string& json, const char* key) {
  const size_t key_pos = findKey(json, key);
  if (key_pos == std::string::npos) return "";
  const size_t start = valueStart(json, key_pos);
  if (start == std::string::npos || start >= json.size() || json[start] != '"') return "";
  std::string out;
  for (size_t i = start + 1; i < json.size(); ++i) {
    if (json[i] == '\\' && i + 1 < json.size()) {
      ++i;
      out += json[i];
    } else if (json[i] == '"') {
      return out;
    } else {
      out += json[i];
    }
  }
  return "";
}

bool jsonBoolField(const std::string& json, const char* key, bool fallback) {
  const size_t key_pos = findKey(json, key);
  if (key_pos == std::string::npos) return fallback;
  const size_t start = valueStart(json, key_pos);
  if (start == std::string::npos) return fallback;
  if (json.compare(start, 4, "true") == 0) return true;
  if (json.compare(start, 5, "false") == 0) return false;
  return fallback;
}

int jsonIntField(const std::string& json, const char* key, int fallback) {
  const size_t key_pos = findKey(json, key);
  if (key_pos == std::string::npos) return fallback;
  const size_t start = valueStart(json, key_pos);
  if (start == std::string::npos || start >= json.size()) return fallback;
  if (json[start] != '-' && (json[start] < '0' || json[start] > '9')) return fallback;
  return atoi(json.c_str() + start);
}

}  // namespace rtb

#pragma once

#include <string>

namespace rtb {

// Minimal, dependency-free helpers for the flat JSON bodies accepted by the
// V0.1 HTTP debug API. Not a general JSON parser.
std::string jsonStringField(const std::string& json, const char* key);
bool jsonBoolField(const std::string& json, const char* key, bool fallback);
int jsonIntField(const std::string& json, const char* key, int fallback);
std::string jsonEscape(const char* value);

}  // namespace rtb

#include "app/SerialControl.h"

#include <stdio.h>

#include "app/JsonMini.h"

namespace rtb {

constexpr size_t SerialControl::kMaxLineBytes;
constexpr int SerialControl::kProtocolVersion;

namespace {

bool isSpace(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

}  // namespace

void SerialControl::setIdentity(const std::string& gateway_id,
                                const std::string& hardware_family,
                                const std::string& firmware_version) {
  gateway_id_ = gateway_id;
  hardware_family_ = hardware_family;
  firmware_version_ = firmware_version;
}

std::string SerialControl::identityJson() const {
  // Field order is fixed: the host test compares the exact line, and a stable
  // shape makes the wire easier to read in a serial monitor.
  char version[16];
  snprintf(version, sizeof(version), "%d", kProtocolVersion);

  std::string out = "{\"type\":\"gateway_info\"";
  out += ",\"gateway_id\":\"" + jsonEscape(gateway_id_.c_str()) + "\"";
  out += ",\"hardware_family\":\"" + jsonEscape(hardware_family_.c_str()) + "\"";
  out += ",\"firmware_version\":\"" + jsonEscape(firmware_version_.c_str()) + "\"";
  out += ",\"protocol_version\":";
  out += version;
  out += "}";
  return out;
}

std::string SerialControl::errorJson(const char* code) const {
  // Never echoes the request back: an arbitrary command string must not be
  // reflected to the client (or into anything that later logs it).
  char version[16];
  snprintf(version, sizeof(version), "%d", kProtocolVersion);

  std::string out = "{\"type\":\"error\",\"error\":\"";
  out += code ? code : "unsupported_command";
  out += "\",\"protocol_version\":";
  out += version;
  out += "}";
  return out;
}

std::string SerialControl::handleLine(const char* line, size_t len) {
  if (!line || len == 0) return std::string();

  size_t begin = 0;
  size_t end = len;
  while (begin < end && isSpace(line[begin])) ++begin;
  while (end > begin && isSpace(line[end - 1])) --end;
  if (begin == end) return std::string();  // blank line: nothing to answer

  // A debug log line ("Gateway ready...", "[HEAP] ...") is not a command.
  if (line[begin] != '{') return std::string();

  const size_t length = end - begin;
  if (length > kMaxLineBytes) return errorJson("line_too_long");

  const std::string body(line + begin, length);
  const std::string type = jsonStringField(body, "type");
  if (type.empty()) return errorJson("malformed_request");
  if (type == "gateway_info") return identityJson();

  // scan / print / raw / firmware update / anything else: refused, no effect.
  return errorJson("unsupported_command");
}

std::string SerialControl::handleLine(const std::string& line) {
  return handleLine(line.data(), line.size());
}

}  // namespace rtb

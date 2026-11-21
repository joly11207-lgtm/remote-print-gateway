#include "app/SerialControl.h"

#include <stdio.h>

#include "app/JsonMini.h"

namespace rtb {

constexpr size_t SerialControl::kMaxLineBytes;
constexpr int SerialControl::kProtocolVersion;
constexpr uint32_t SerialControl::kScanDefaultMs;
constexpr uint32_t SerialControl::kScanMinMs;
constexpr uint32_t SerialControl::kScanMaxMs;

uint32_t SerialControl::clampScanDuration(int requested_ms) {
  if (requested_ms <= 0) return kScanDefaultMs;
  const uint32_t value = static_cast<uint32_t>(requested_ms);
  if (value < kScanMinMs) return kScanMinMs;
  if (value > kScanMaxMs) return kScanMaxMs;
  return value;
}

namespace {

bool isSpace(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

void appendVersion(std::string& out) {
  char version[16];
  snprintf(version, sizeof(version), "%d", SerialControl::kProtocolVersion);
  out += ",\"protocol_version\":";
  out += version;
}

}  // namespace

void SerialControl::setIdentity(const std::string& gateway_id,
                                const std::string& hardware_family,
                                const std::string& firmware_version) {
  gateway_id_ = gateway_id;
  hardware_family_ = hardware_family;
  firmware_version_ = firmware_version;
}

std::string SerialControl::infoLine() const {
  // Field order is fixed: the host test compares the exact line, and a stable
  // shape makes the wire easier to read in a serial monitor.
  std::string out = "{\"type\":\"gateway_info\"";
  out += ",\"gateway_id\":\"" + jsonEscape(gateway_id_.c_str()) + "\"";
  out += ",\"hardware_family\":\"" + jsonEscape(hardware_family_.c_str()) + "\"";
  out += ",\"firmware_version\":\"" + jsonEscape(firmware_version_.c_str()) + "\"";
  appendVersion(out);
  out += "}";
  return out;
}

std::string SerialControl::errorLine(const char* code) const {
  // Never echoes the request back: an arbitrary command string must not be
  // reflected to the client (or into anything that later logs it).
  std::string out = "{\"type\":\"error\",\"error\":\"";
  out += code ? code : "unsupported_command";
  out += "\"";
  appendVersion(out);
  out += "}";
  return out;
}

std::string SerialControl::scanLine(const SerialScanDevice* devices,
                                    size_t count,
                                    const char* result_name,
                                    unsigned long elapsed_ms,
                                    size_t scanned) const {
  std::string out = "{\"type\":\"scan_result\",\"result\":\"";
  out += result_name ? result_name : "OK";
  out += "\",\"transport\":\"ble\",\"scanned\":";
  out += std::to_string(scanned);
  out += ",\"elapsed_ms\":";
  out += std::to_string(elapsed_ms);
  appendVersion(out);
  out += ",\"devices\":[";

  for (size_t i = 0; i < count; ++i) {
    const SerialScanDevice& device = devices[i];
    if (i > 0) out += ',';
    out += "{\"name\":\"" + jsonEscape(device.name ? device.name : "") + "\"";
    out += ",\"address\":\"" + jsonEscape(device.address ? device.address : "") + "\"";
    out += ",\"rssi\":";
    out += std::to_string(device.rssi);

    // `services` carries only what the advertisement actually contained: the
    // BLE transport captures at most one service UUID, so the array has 0 or 1
    // element and is never padded to look richer than the evidence.
    out += ",\"service_uuid\":\"" + jsonEscape(device.service_uuid ? device.service_uuid : "") + "\"";
    out += ",\"services\":[";
    if (device.service_uuid && device.service_uuid[0]) {
      out += "\"" + jsonEscape(device.service_uuid) + "\"";
    }
    out += "]";

    out += ",\"fingerprint\":\"";
    out += jsonEscape(device.fingerprint ? device.fingerprint : "Unknown");
    out += "\",\"confidence\":";
    out += std::to_string(device.confidence);
    out += ",\"evidence\":\"";
    out += jsonEscape(device.evidence ? device.evidence : "none");
    out += "\",\"model_id\":\"";
    out += jsonEscape(device.model_id ? device.model_id : "unmatched");
    out += "\",\"match_reason\":\"";
    out += jsonEscape(device.match_reason ? device.match_reason : "none");
    out += "\"}";
  }

  out += "]}";
  return out;
}

SerialControl::Dispatch SerialControl::dispatch(const char* line, size_t len) const {
  Dispatch result;
  if (!line || len == 0) return result;

  size_t begin = 0;
  size_t end = len;
  while (begin < end && isSpace(line[begin])) ++begin;
  while (end > begin && isSpace(line[end - 1])) --end;
  if (begin == end) return result;  // blank line: nothing to answer

  // A debug log line ("Gateway ready...", "[HEAP] ...") is not a command.
  if (line[begin] != '{') return result;

  const size_t length = end - begin;
  if (length > kMaxLineBytes) {
    result.command = Command::kError;
    result.reply = errorLine("line_too_long");
    return result;
  }

  const std::string body(line + begin, length);
  const std::string type = jsonStringField(body, "type");
  if (type.empty()) {
    result.command = Command::kError;
    result.reply = errorLine("malformed_request");
    return result;
  }
  if (type == "gateway_info") {
    result.command = Command::kInfo;
    result.reply = infoLine();
    return result;
  }
  if (type == "scan") {
    // The only command that needs the radio: the caller runs it and answers.
    // `duration_ms` is a request, not an instruction: the caller clamps it.
    result.command = Command::kScan;
    result.duration_ms = jsonIntField(body, "duration_ms", 0);
    return result;
  }

  // print / raw / firmware_update / anything else: refused, no effect.
  result.command = Command::kError;
  result.reply = errorLine("unsupported_command");
  return result;
}

SerialControl::Dispatch SerialControl::dispatch(const std::string& line) const {
  return dispatch(line.data(), line.size());
}

}  // namespace rtb

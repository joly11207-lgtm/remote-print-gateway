#include "protocols/base/ProtocolFingerprint.h"

#include <cctype>
#include <string>

namespace rtb {
namespace {

char lowerChar(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

std::string lowerCopy(const std::string& s) {
  std::string out(s);
  for (size_t i = 0; i < out.size(); ++i) out[i] = lowerChar(out[i]);
  return out;
}

}  // namespace

bool containsIgnoreCase(const std::string& haystack, const std::string& needle) {
  if (needle.empty()) return false;
  return lowerCopy(haystack).find(lowerCopy(needle)) != std::string::npos;
}

bool nameMatchesPattern(const std::string& device_name, const char* pattern) {
  return containsIgnoreCase(device_name, pattern ? pattern : "");
}

ProtocolFingerprint fingerprintFromEndpoint(const Endpoint& endpoint) {
  ProtocolFingerprint fp;

  if (containsIgnoreCase(endpoint.service_uuid, "ae30") ||
      containsIgnoreCase(endpoint.write_uuid, "ae01") ||
      containsIgnoreCase(endpoint.notify_uuid, "ae02")) {
    fp.family = ProtocolFamily::TinyIPrint;
    fp.confidence = 95;
    fp.evidence = "gatt_uuid_ae30_ae01_ae02";
    return fp;
  }
  for (size_t i = 0; i < endpoint.advertised_services.size(); ++i) {
    if (containsIgnoreCase(endpoint.advertised_services[i], "ae30")) {
      fp.family = ProtocolFamily::TinyIPrint;
      fp.confidence = 100;
      fp.evidence = "advertised_service_ae30";
      return fp;
    }
  }

  // X18 advertises AF30 (its GATT print service is still AE30/AE01/AE02, which
  // only exists after connecting). Leaving this out is why an X18 showed up as
  // "unknown / no_matching_evidence" and only matched by a weak name hit.
  // The BLE stack may report the UUID in either case.
  if (containsIgnoreCase(endpoint.service_uuid, "af30")) {
    fp.family = ProtocolFamily::TinyIPrint;
    fp.confidence = 95;
    fp.evidence = "advertised_service_af30";
    return fp;
  }
  for (size_t i = 0; i < endpoint.advertised_services.size(); ++i) {
    if (containsIgnoreCase(endpoint.advertised_services[i], "af30")) {
      fp.family = ProtocolFamily::TinyIPrint;
      fp.confidence = 100;
      fp.evidence = "advertised_service_af30";
      return fp;
    }
  }

  static const char* const kTinyNames[] = {
      "GB01", "GB02", "GB03", "GT01", "MX05", "MX06", "MX10", "YT01",
      "SC03", "iPrint", "Pocket Printer", "WalkPrint", "FunPrint", "D1", "FC02",
      "X18"};
  for (size_t i = 0; i < sizeof(kTinyNames) / sizeof(kTinyNames[0]); ++i) {
    if (nameMatchesPattern(endpoint.name, kTinyNames[i])) {
      fp.family = ProtocolFamily::TinyIPrint;
      fp.confidence = 70;
      fp.evidence = "bluetooth_name_pattern";
      return fp;
    }
  }

  if ((endpoint.kind == TransportKind::BluetoothClassic ||
       endpoint.kind == TransportKind::MockClassic) &&
      (containsIgnoreCase(endpoint.name, "POS") ||
       containsIgnoreCase(endpoint.name, "ESC") ||
       containsIgnoreCase(endpoint.name, "Printer"))) {
    fp.family = ProtocolFamily::EscPos;
    fp.confidence = 60;
    fp.evidence = "classic_spp_name_heuristic";
    return fp;
  }

  fp.family = ProtocolFamily::Unknown;
  fp.confidence = 0;
  fp.evidence = "no_matching_evidence";
  return fp;
}

}  // namespace rtb

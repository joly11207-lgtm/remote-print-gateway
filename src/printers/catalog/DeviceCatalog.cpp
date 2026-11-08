#include "printers/catalog/DeviceCatalog.h"

#include <string>
#include <vector>

#include "printers/catalog/BuiltinProfiles.h"
#include "protocols/base/ProtocolFingerprint.h"

namespace rtb {
namespace {

std::string jsonString(const char* s) {
  std::string out = "\"";
  for (const char* p = s ? s : ""; *p; ++p) {
    if (*p == '"' || *p == '\\') out += '\\';
    out += *p;
  }
  out += '"';
  return out;
}

std::string boolText(bool v) { return v ? "true" : "false"; }

std::string stringArray(const char* const* items, size_t n) {
  std::string out = "[";
  for (size_t i = 0; i < n; ++i) {
    if (i) out += ",";
    out += jsonString(items[i]);
  }
  out += "]";
  return out;
}

std::string transportArray(uint8_t mask) {
  std::vector<std::string> names;
  if (mask & kSupportBle) names.push_back("ble");
  if (mask & kSupportBluetoothClassic) names.push_back("bluetooth_classic");
  if (mask & kSupportUsb) names.push_back("usb");
  if (mask & kSupportTcp) names.push_back("tcp");
  std::string out = "[";
  for (size_t i = 0; i < names.size(); ++i) {
    if (i) out += ",";
    out += jsonString(names[i].c_str());
  }
  out += "]";
  return out;
}

}  // namespace

size_t DeviceCatalog::count() { return kBuiltinProfileCount; }

const DeviceProfile& DeviceCatalog::at(size_t index) { return kBuiltinProfiles[index]; }

const DeviceProfile* DeviceCatalog::findByModelId(const char* model_id) {
  if (!model_id) return nullptr;
  for (size_t i = 0; i < kBuiltinProfileCount; ++i) {
    if (std::string(model_id) == kBuiltinProfiles[i].model_id) return &kBuiltinProfiles[i];
  }
  return nullptr;
}

CatalogMatch DeviceCatalog::match(const Endpoint& endpoint) {
  CatalogMatch m;
  m.fingerprint = fingerprintFromEndpoint(endpoint);

  int best_score = 0;
  for (size_t i = 0; i < kBuiltinProfileCount; ++i) {
    const DeviceProfile& p = kBuiltinProfiles[i];
    if (endpoint.kind != TransportKind::Unknown && !profileSupports(p, endpoint.kind)) continue;

    int score = 0;
    const char* reason = "fingerprint_only";
    if (m.fingerprint.family == p.protocol_family) score += 25;
    for (size_t n = 0; n < p.name_pattern_count; ++n) {
      if (nameMatchesPattern(endpoint.name, p.bluetooth_name_patterns[n])) {
        score += 60;
        reason = "bluetooth_name_pattern";
        break;
      }
    }
    if (!endpoint.service_uuid.empty() && p.service_uuid[0] != '\0' &&
        containsIgnoreCase(endpoint.service_uuid, p.service_uuid)) {
      score += 30;
      if (score >= 90) reason = "gatt_uuid_and_fingerprint";
    }
    if (p.implemented) score += 5;

    if (score > best_score) {
      best_score = score;
      m.profile = &p;
      m.match_reason = reason;
    }
  }

  m.score = best_score;
  if (best_score < 50) {
    m.profile = nullptr;
    m.match_reason = "insufficient_evidence";
  }
  return m;
}

void DeviceCatalog::toJson(std::string& out) {
  out = "[";
  for (size_t i = 0; i < kBuiltinProfileCount; ++i) {
    const DeviceProfile& p = kBuiltinProfiles[i];
    if (i) out += ",";
    out += "{\"model_id\":" + jsonString(p.model_id);
    out += ",\"display_name\":" + jsonString(p.display_name);
    out += ",\"protocol_family\":" + jsonString(protocolFamilyName(p.protocol_family));
    out += ",\"supported_transports\":" + transportArray(p.supported_transports);
    out += ",\"bluetooth_name_patterns\":" +
           stringArray(p.bluetooth_name_patterns, p.name_pattern_count);
    out += ",\"service_uuid\":" + jsonString(p.service_uuid);
    // Advertisement UUID is a separate, optional field. Only emitted when a
    // distinct advertised UUID was actually observed on hardware.
    if (p.advertised_service_uuid && p.advertised_service_uuid[0] != '\0') {
      out += ",\"advertised_service_uuid\":" + jsonString(p.advertised_service_uuid);
    }
    out += ",\"write_uuid\":" + jsonString(p.write_uuid);
    out += ",\"notify_uuid\":" + jsonString(p.notify_uuid);
    out += ",\"width_dots\":" + std::to_string(p.width_dots);
    out += ",\"use_lattice\":" + boolText(p.use_lattice);
    out += ",\"mirror_rows\":" + boolText(p.mirror_rows);
    out += ",\"energy\":" + std::to_string(p.energy);
    out += ",\"quality\":" + std::to_string(p.quality);
    out += ",\"speed\":" + std::to_string(p.speed);
    out += ",\"feed_dots\":" + std::to_string(p.feed_dots);
    out += ",\"row_delay_ms\":" + std::to_string(p.row_delay_ms);
    out += ",\"write_chunk\":" + std::to_string(p.write_chunk);
    out += ",\"implemented\":" + boolText(p.implemented);
    out += "}";
  }
  out += "]";
}

}  // namespace rtb

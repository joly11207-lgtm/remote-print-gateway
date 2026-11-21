#pragma once

#include <stddef.h>
#include <stdint.h>

#include <functional>
#include <string>
#include <vector>

#include "config/RtbConfig.h"

namespace rtb {

enum class TransportKind : uint8_t {
  Unknown = 0,
  Ble = 1,
  BluetoothClassic = 2,
  Usb = 3,
  Tcp = 4,
  Mock = 5,
  MockClassic = 6,
};

inline const char* transportKindName(TransportKind k) {
  switch (k) {
    case TransportKind::Ble: return "ble";
    case TransportKind::BluetoothClassic: return "bluetooth_classic";
    case TransportKind::Usb: return "usb";
    case TransportKind::Tcp: return "tcp";
    case TransportKind::Mock: return "mock";
    case TransportKind::MockClassic: return "mock_bluetooth_classic";
    default: return "unknown";
  }
}

inline TransportKind transportKindFromName(const char* name) {
  if (!name) return TransportKind::Unknown;
  const std::string s(name);
  if (s == "ble") return TransportKind::Ble;
  if (s == "bluetooth_classic" || s == "spp") return TransportKind::BluetoothClassic;
  if (s == "usb") return TransportKind::Usb;
  if (s == "tcp") return TransportKind::Tcp;
  if (s == "mock") return TransportKind::Mock;
  if (s == "mock_bluetooth_classic") return TransportKind::MockClassic;
  return TransportKind::Unknown;
}

enum class Result : int {
  Ok = 0,
  NotConnected = 1,
  Timeout = 2,
  IoError = 3,
  NotSupportedOnThisTarget = 4,
  NotFound = 5,
  NotImplemented = 6,
  Busy = 7,
  InvalidArgument = 8,
  CapabilityMismatch = 9,
};

inline const char* resultName(Result r) {
  switch (r) {
    case Result::Ok: return "OK";
    case Result::NotConnected: return "NOT_CONNECTED";
    case Result::Timeout: return "TIMEOUT";
    case Result::IoError: return "IO_ERROR";
    case Result::NotSupportedOnThisTarget: return RTB_ERR_NOT_SUPPORTED_ON_THIS_TARGET;
    case Result::NotFound: return "NOT_FOUND";
    case Result::NotImplemented: return RTB_ERR_NOT_IMPLEMENTED;
    case Result::Busy: return "BUSY";
    case Result::InvalidArgument: return "INVALID_ARGUMENT";
    case Result::CapabilityMismatch: return "CAPABILITY_MISMATCH";
    default: return "UNKNOWN";
  }
}

// A discovered device. No protocol-specific meaning is attached here.
struct Endpoint {
  TransportKind kind = TransportKind::Unknown;
  std::string address;
  std::string name;
  int rssi = -127;
  std::string service_uuid;
  std::string write_uuid;
  std::string notify_uuid;
  std::vector<std::string> advertised_services;
};

using NotificationCallback = std::function<void(const uint8_t* data, size_t len)>;

struct ScanRequest {
  uint32_t duration_ms = 3000;
  std::string name_filter;
  // Advertised-service filter. The Tiny/iPrint family is not consistent:
  // GB01-class units advertise AE30 while the X18 advertises AF30, so phase 1
  // must be able to accept either. Two named slots instead of a vector keeps
  // the request trivially copyable and the call sites obvious.
  std::string service_uuid_filter;
  std::string service_uuid_filter_alt;
  // Was 8. On a desk with a dozen BLE advertisers the result list filled up
  // before the printer was reached, so discovery failed at random and the
  // caller backed off for 30 s. Keep everything a short scan can see.
  size_t max_results = 32;
};

// Case-insensitive "does this advertised service UUID satisfy the request".
// Case matters here: the BLE stack returns lowercase UUIDs for some units and
// uppercase for others, so the previous exact `find("AE30")` was fragile.
inline bool scanServiceMatches(const ScanRequest& request, const std::string& service_uuid) {
  if (request.service_uuid_filter.empty() && request.service_uuid_filter_alt.empty()) {
    return true;
  }
  const size_t n = service_uuid.size();
  std::string lowered;
  lowered.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    const char c = service_uuid[i];
    lowered += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
  }
  const char* filters[2] = {request.service_uuid_filter.c_str(),
                            request.service_uuid_filter_alt.c_str()};
  for (size_t f = 0; f < 2; ++f) {
    if (!filters[f][0]) continue;
    std::string needle;
    for (const char* p = filters[f]; *p; ++p) {
      needle += (*p >= 'A' && *p <= 'Z') ? static_cast<char>(*p - 'A' + 'a') : *p;
    }
    if (lowered.find(needle) != std::string::npos) return true;
  }
  return false;
}

struct ScanResult {
  Result result = Result::Ok;
  std::vector<Endpoint> endpoints;
};

struct TransportCapabilities {
  bool available = false;
  bool supports_scan = false;
  bool supports_notifications = false;
  bool requires_notify_subscription = false;
  size_t max_write_chunk = 20;
  uint16_t recommended_row_delay_ms = 0;
  const char* unavailable_reason = nullptr;  // e.g. RTB_ERR_NOT_SUPPORTED_ON_THIS_TARGET
  const char* note = "";
};

}  // namespace rtb

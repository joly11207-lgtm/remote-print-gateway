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
  std::string service_uuid_filter;
  size_t max_results = 8;
};

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

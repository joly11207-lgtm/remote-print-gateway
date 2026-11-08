#pragma once

#include <stddef.h>
#include <stdint.h>

#include "protocols/base/ProtocolTypes.h"
#include "transport/base/TransportTypes.h"

namespace rtb {

enum TransportSupport : uint8_t {
  kSupportBle = 1u << 0,
  kSupportBluetoothClassic = 1u << 1,
  kSupportUsb = 1u << 2,
  kSupportTcp = 1u << 3,
};

// Device Profile = Transport information + Protocol information + geometry.
// Mirrors printers/profiles/*.json (validated by tests/test_profiles.py).
struct DeviceProfile {
  const char* model_id;
  const char* display_name;
  ProtocolFamily protocol_family;
  uint8_t supported_transports;
  const char* const* bluetooth_name_patterns;
  size_t name_pattern_count;
  const char* service_uuid;
  const char* write_uuid;
  const char* notify_uuid;
  uint16_t width_dots;
  bool use_lattice;
  bool mirror_rows;
  uint16_t energy;
  uint8_t quality;
  uint8_t speed;
  uint16_t feed_dots;
  uint8_t row_delay_ms;
  uint16_t write_chunk;
  bool implemented;  // only tiny_iprint profiles are implemented in V0.1
  // Advertisement-only UUID, deliberately kept SEPARATE from service_uuid
  // (the real GATT print service). X18 advertises AF30 but exposes AE30 in
  // GATT. Empty string when no distinct advertised UUID was observed.
  const char* advertised_service_uuid;
};

inline bool profileSupports(const DeviceProfile& p, TransportKind k) {
  switch (k) {
    case TransportKind::Ble:
      return (p.supported_transports & kSupportBle) != 0;
    case TransportKind::BluetoothClassic:
    case TransportKind::MockClassic:
      return (p.supported_transports & kSupportBluetoothClassic) != 0;
    case TransportKind::Usb:
      return (p.supported_transports & kSupportUsb) != 0;
    case TransportKind::Tcp:
      return (p.supported_transports & kSupportTcp) != 0;
    case TransportKind::Mock:
      return (p.supported_transports & kSupportBle) != 0;  // mock emulates a BLE link
    default:
      return false;
  }
}

inline ProtocolContext makeContext(const DeviceProfile& p) {
  ProtocolContext c;
  c.width_dots = p.width_dots;
  c.width_bytes = static_cast<uint16_t>((p.width_dots + 7u) / 8u);
  c.settings.energy = p.energy;
  c.settings.quality = p.quality;
  c.settings.speed = p.speed;
  c.settings.feed_dots = p.feed_dots;
  c.settings.row_delay_ms = p.row_delay_ms;
  c.settings.use_lattice = p.use_lattice;
  c.settings.mirror_rows = p.mirror_rows;
  return c;
}

}  // namespace rtb

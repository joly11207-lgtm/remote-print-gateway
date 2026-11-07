#pragma once

#include "transport/base/PrinterTransport.h"

namespace rtb {

// ============================================================================
// ARCHITECTURE RESERVATION ONLY - V0.1 DOES NOT IMPLEMENT CLASSIC BLUETOOTH.
// ============================================================================
// Why: both development boards (ESP32-S3 N16R8 / S3 SuperMini) are BLE-only
// silicon. ESP32-S3 builds must not reference any Classic Bluetooth API.
//
// FUTURE SPP/RFCOMM IMPLEMENTATION ENTRY POINTS (ESP32-WROOM-32E class boards):
//   [0] Only then set -DRTB_ENABLE_BT_CLASSIC_IMPL=1 for an ESP32 dual-mode
//       target. RtbConfig.h has a hard #error guard for ESP32-S3.
//   [1] scan(): enumerate/pair SPP devices, map each to an Endpoint with
//       kind = BluetoothClassic and the RFCOMM/address information.
//   [2] connect(): open the SPP/RFCOMM stream to Endpoint::address.
//   [3] write()/read(): move bytes over that stream; transport-level chunking
//       and buffering only, never any printer opcode semantics.
//   [4] subscribe(): optional background reader -> NotificationCallback.
//   [5] EscPosProtocol can then use this class without protocol changes, and
//       DeviceProfile.supported_transports already carries ["bluetooth_classic"].
//
// Until then every operation returns NOT_SUPPORTED_ON_THIS_TARGET on S3 and
// NOT_IMPLEMENTED on a chip that would be capable. No classic headers are
// included anywhere in this build.
class BluetoothClassicTransport : public PrinterTransport {
 public:
  TransportKind kind() const override;
  const char* name() const override { return "bluetooth_classic"; }

  Result scan(const ScanRequest& request, ScanResult& out) override;
  Result connect(const Endpoint& endpoint) override;
  void disconnect() override;
  Result write(const uint8_t* data, size_t len, size_t& written) override;
  Result read(uint8_t* buffer, size_t capacity, size_t& received,
              uint32_t timeout_ms = 0) override;
  Result subscribe(NotificationCallback callback) override;
  bool isConnected() const override;
  TransportCapabilities getCapabilities() const override;

  static bool hardwareSupported();        // chip-level capability detection
  static bool implementationEnabled();    // build-time compile condition
  static Result capabilityGate();         // NOT_SUPPORTED_ON_THIS_TARGET / NOT_IMPLEMENTED
};

}  // namespace rtb

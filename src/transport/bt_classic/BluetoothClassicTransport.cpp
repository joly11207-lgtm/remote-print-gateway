#include "transport/bt_classic/BluetoothClassicTransport.h"

#include "boards/BoardCapabilities.h"
#include "config/RtbConfig.h"

// Deliberately NO Classic Bluetooth headers (BluetoothSerial.h, esp_spp_api.h,
// esp_bt*.h) in this translation unit. That is the whole point of the V0.1
// compile condition: an ESP32-S3 build never references unsupported APIs.

#if RTB_ENABLE_BT_CLASSIC_IMPL
// Reserved for the future ESP32 dual-mode implementation. Left empty in V0.1.
// See BluetoothClassicTransport.h [0]..[5] for the exact entry points.
#endif

namespace rtb {

TransportKind BluetoothClassicTransport::kind() const {
  return TransportKind::BluetoothClassic;
}

bool BluetoothClassicTransport::hardwareSupported() {
  return boardCapabilities().bluetooth_classic;
}

bool BluetoothClassicTransport::implementationEnabled() {
  return RTB_ENABLE_BT_CLASSIC_IMPL != 0;
}

Result BluetoothClassicTransport::capabilityGate() {
  if (!hardwareSupported()) return Result::NotSupportedOnThisTarget;
  return Result::NotImplemented;  // no SPP/RFCOMM implementation in V0.1 yet
}

Result BluetoothClassicTransport::scan(const ScanRequest&, ScanResult& out) {
  out.endpoints.clear();
  out.result = capabilityGate();
  return out.result;
}

Result BluetoothClassicTransport::connect(const Endpoint&) { return capabilityGate(); }

void BluetoothClassicTransport::disconnect() {}

Result BluetoothClassicTransport::write(const uint8_t*, size_t, size_t& written) {
  written = 0;
  return capabilityGate();
}

Result BluetoothClassicTransport::read(uint8_t*, size_t, size_t& received, uint32_t) {
  received = 0;
  return capabilityGate();
}

Result BluetoothClassicTransport::subscribe(NotificationCallback) {
  return capabilityGate();
}

bool BluetoothClassicTransport::isConnected() const { return false; }

TransportCapabilities BluetoothClassicTransport::getCapabilities() const {
  TransportCapabilities c;
  c.available = false;
  c.supports_scan = false;
  c.supports_notifications = false;
  c.requires_notify_subscription = false;
  c.max_write_chunk = 0;
  c.unavailable_reason = hardwareSupported() ? RTB_ERR_NOT_IMPLEMENTED
                                            : RTB_ERR_NOT_SUPPORTED_ON_THIS_TARGET;
  c.note = hardwareSupported()
               ? "chip supports Classic, but SPP/RFCOMM transport is LATER"
               : "ESP32-S3 has no Bluetooth Classic radio";
  return c;
}

}  // namespace rtb

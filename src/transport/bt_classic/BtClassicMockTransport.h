#pragma once

#include "transport/mock/MockTransport.h"

namespace rtb {

// Software-only mock of an SPP/RFCOMM stream. It lets the Classic code path,
// DeviceProfile.supported_transports and EscPosProtocol-over-Classic be
// exercised on ESP32-S3 WITHOUT referencing any Classic Bluetooth API.
// It is explicitly labelled as a mock and must never be reported as a real
// Classic Bluetooth validation.
class BtClassicMockTransport : public MockTransport {
 public:
  BtClassicMockTransport();

  const char* name() const override { return "mock_bluetooth_classic"; }
  TransportCapabilities getCapabilities() const override;

  // Capability detection: true only on chips that actually have a Classic radio.
  static bool realHardwareSupported();
};

}  // namespace rtb

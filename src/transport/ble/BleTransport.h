#pragma once

#include <stddef.h>
#include <stdint.h>

#include "transport/base/PrinterTransport.h"

namespace rtb {

// Real BLE GATT transport for ESP32-S3 (Arduino-ESP32 Bluedroid BLE stack).
// PIMPL is used on purpose: this header contains NO Bluetooth headers, so
// protocol / catalog / host-test code compiles without any BLE dependency and
// an ESP32-S3 build never sees a Classic Bluetooth include.
//
// Service/characteristic discovery is driven by the DeviceProfile UUIDs
// (AE30 service, AE01 write, AE02 notify). The transport knows nothing about
// printer opcodes: all framing/CRC belongs to TinyProtocol.
class BleTransport : public PrinterTransport {
 public:
  BleTransport();
  ~BleTransport() override;

  TransportKind kind() const override;
  const char* name() const override { return "ble"; }

  Result scan(const ScanRequest& request, ScanResult& out) override;
  Result connect(const Endpoint& endpoint) override;
  void disconnect() override;
  Result write(const uint8_t* data, size_t len, size_t& written) override;
  Result read(uint8_t* buffer, size_t capacity, size_t& received,
              uint32_t timeout_ms = 0) override;
  Result subscribe(NotificationCallback callback) override;
  bool isConnected() const override;
  TransportCapabilities getCapabilities() const override;

  void setMaxWriteChunk(size_t chunk);
  bool notifySubscribed() const;

 public:
  // Implementation detail, public only so the .cpp trampoline can name it.
  struct Impl;

 private:
  Impl* impl_;
};

}  // namespace rtb

#pragma once

#include "transport/base/TransportTypes.h"

namespace rtb {

// Uniform transport contract. Protocol code only ever sees this interface.
// Implementations: ble/, bt_classic/, mock/. Future: usb/, tcp/.
class PrinterTransport {
 public:
  virtual ~PrinterTransport() {}

  virtual TransportKind kind() const = 0;
  virtual const char* name() const = 0;

  virtual Result scan(const ScanRequest& request, ScanResult& out) = 0;
  virtual Result connect(const Endpoint& endpoint) = 0;
  virtual void disconnect() = 0;

  // write() owns the transport-specific chunking (BLE MTU / SPP framing / TCP).
  virtual Result write(const uint8_t* data, size_t len, size_t& written) = 0;
  // read() drains buffered inbound bytes (BLE notifications / SPP stream).
  virtual Result read(uint8_t* buffer, size_t capacity, size_t& received,
                      uint32_t timeout_ms = 0) = 0;

  // ---- Notification callback lifetime contract ----------------------------
  // A NotificationCallback commonly captures a short-lived owner (PrintSession).
  // Implementations MUST:
  //   [1] replace (not append to) the stored callback on every subscribe(),
  //   [2] treat subscribe({}) (an empty std::function) as "unsubscribe" and
  //       drop the previous callback immediately,
  //   [3] drop the callback on disconnect(), because some stacks can deliver
  //       a late notification while/after the link is torn down,
  //   [4] drop the callback in their destructor.
  // Owners MUST unsubscribe (or be destroyed, which unsubscribes) before any
  // state the callback references goes out of scope.
  virtual Result subscribe(NotificationCallback callback) = 0;
  Result unsubscribe() { return subscribe(NotificationCallback()); }

  virtual bool isConnected() const = 0;
  virtual TransportCapabilities getCapabilities() const = 0;
};

}  // namespace rtb

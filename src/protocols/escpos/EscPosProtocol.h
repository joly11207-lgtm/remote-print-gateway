#pragma once

#include "protocols/base/StubProtocol.h"

namespace rtb {

// LATER: real ESC/POS implementation.
// Intended to run over BleTransport, BluetoothClassicTransport (SPP/RFCOMM),
// UsbTransport and TcpTransport without any protocol-side changes, because it
// only ever sees PrinterTransport. Not part of V0.1 scope.
class EscPosProtocol : public StubProtocol {
 public:
  ProtocolFamily family() const override { return ProtocolFamily::EscPos; }
  const char* name() const override { return "escpos"; }
};

}  // namespace rtb

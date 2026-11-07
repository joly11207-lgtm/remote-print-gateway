#pragma once

#include "protocols/base/ProtocolTypes.h"
#include "transport/base/PrinterTransport.h"
#include "transport/base/TransportTypes.h"

namespace rtb {

// Protocol owns ONLY: packet framing, CRC/checksum, init command, bitmap
// command, feed, status parsing. It never scans, connects, chunks for a radio,
// or owns session/connection state. It talks to the abstract PrinterTransport.
//
// Consequently TinyProtocol works today over BleTransport and MockTransport,
// and later an EscPosProtocol can use the same interface over
// BluetoothClassicTransport / UsbTransport / TcpTransport without changes.
class PrinterProtocol {
 public:
  virtual ~PrinterProtocol() {}

  virtual ProtocolFamily family() const = 0;
  virtual const char* name() const = 0;
  virtual ProtocolCapabilities capabilities() const = 0;

  virtual Result init(const ProtocolContext& ctx, PrinterTransport& transport) = 0;
  virtual Result printRow(const ProtocolContext& ctx, const uint8_t* row,
                          size_t row_bytes, PrinterTransport& transport) = 0;
  virtual Result finish(const ProtocolContext& ctx, PrinterTransport& transport) = 0;
  virtual Result feed(const ProtocolContext& ctx, uint16_t dots,
                      PrinterTransport& transport) = 0;
  virtual Result statusRequest(PrinterTransport& transport) = 0;
  virtual Result parseStatus(const uint8_t* data, size_t len,
                             PrinterStatus& out) const = 0;
};

}  // namespace rtb

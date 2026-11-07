#include "transport/bt_classic/BtClassicMockTransport.h"

#include "boards/BoardCapabilities.h"

namespace rtb {

BtClassicMockTransport::BtClassicMockTransport()
    : MockTransport(TransportKind::MockClassic) {
  setMaxWriteChunk(1024);  // SPP is a byte stream, not 20-byte GATT writes
}

bool BtClassicMockTransport::realHardwareSupported() {
  return boardCapabilities().bluetooth_classic;
}

TransportCapabilities BtClassicMockTransport::getCapabilities() const {
  TransportCapabilities c = MockTransport::getCapabilities();
  c.supports_notifications = false;
  c.requires_notify_subscription = false;
  c.max_write_chunk = 1024;
  c.note = realHardwareSupported()
               ? "software mock of SPP/RFCOMM; real Classic radio present on this chip"
               : "software mock of SPP/RFCOMM; real Classic radio ABSENT on ESP32-S3";
  return c;
}

}  // namespace rtb

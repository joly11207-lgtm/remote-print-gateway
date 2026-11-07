#pragma once

#include <stddef.h>
#include <stdint.h>

#include "protocols/base/PrinterProtocol.h"
#include "protocols/base/ProtocolTypes.h"
#include "transport/base/PrinterTransport.h"
#include "transport/base/TransportTypes.h"

namespace rtb {

// Tiny / iPrint / CatPrinter AE30 protocol family.
//
// Frame (transport independent):
//   51 78 <opcode> 00 <len_lo> <len_hi> <payload...> <crc8(payload)> FF
// CRC-8 (poly 0x07, init 0x00, MSB-first) over the payload only.
//
// Sources: Unlicense CatGFX (TheNitek), MIT ahmetozer bridge, CC0 COMMANDS.md
// (amber-sixel/catprinter contrib), MIT mini-print-studio protocol guide.
// See docs/OPEN_SOURCE_AUDIT.md. No GPL/copyleft code was copied.
class TinyProtocol : public PrinterProtocol {
 public:
  enum Opcode : uint8_t {
    kReverseFeed = 0xA0,
    kFeed = 0xA1,
    kPrintRow = 0xA2,
    kGetStatus = 0xA3,
    kQuality = 0xA4,
    kLattice = 0xA6,
    kDeviceInfo = 0xA8,
    kFlowControl = 0xAE,
    kEnergy = 0xAF,
    kSpeed = 0xBD,
    kDrawingMode = 0xBE,
    kPrintCompressed = 0xBF,
  };

  static const uint8_t kFrameMagic0 = 0x51;
  static const uint8_t kFrameMagic1 = 0x78;
  static const uint8_t kFrameEnd = 0xFF;
  static const size_t kFrameOverhead = 8;  // 2 magic + opcode + flag + len16 + 0x00 + crc + end
  static const size_t kMaxPayload = 255;
  // A bitmap row is one frame payload. Anything wider is rejected rather than
  // mirrored into a fixed-size stack buffer (see printRow()).
  static const size_t kMaxRowBytes = kMaxPayload;

  static uint8_t crc8(const uint8_t* data, size_t len);
  static uint8_t mirrorByte(uint8_t value);
  static Result buildFrame(uint8_t opcode, const uint8_t* payload, size_t len,
                           uint8_t* out, size_t capacity, size_t& out_len);
  static Result validateFrame(const uint8_t* frame, size_t len, uint8_t& opcode,
                              const uint8_t*& payload, size_t& payload_len);

  ProtocolFamily family() const override { return ProtocolFamily::TinyIPrint; }
  const char* name() const override { return "tiny_iprint"; }
  ProtocolCapabilities capabilities() const override;

  Result init(const ProtocolContext& ctx, PrinterTransport& transport) override;
  Result printRow(const ProtocolContext& ctx, const uint8_t* row, size_t row_bytes,
                  PrinterTransport& transport) override;
  Result finish(const ProtocolContext& ctx, PrinterTransport& transport) override;
  Result feed(const ProtocolContext& ctx, uint16_t dots,
              PrinterTransport& transport) override;
  Result statusRequest(PrinterTransport& transport) override;
  Result parseStatus(const uint8_t* data, size_t len, PrinterStatus& out) const override;

 private:
  Result sendCommand(PrinterTransport& transport, uint8_t opcode,
                     const uint8_t* payload, size_t len) const;
};

}  // namespace rtb

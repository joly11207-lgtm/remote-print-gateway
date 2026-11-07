#include "protocols/tiny_iprint/TinyProtocol.h"

#include <string.h>

#include <vector>

namespace rtb {
namespace {

const uint8_t kLatticeStart[11] = {0xAA, 0x55, 0x17, 0x38, 0x44, 0x5F,
                                   0x5F, 0x5F, 0x44, 0x38, 0x2C};
const uint8_t kLatticeEnd[11] = {0xAA, 0x55, 0x17, 0x00, 0x00, 0x00,
                                 0x00, 0x00, 0x00, 0x00, 0x17};

}  // namespace

uint8_t TinyProtocol::crc8(const uint8_t* data, size_t len) {
  uint8_t crc = 0;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = static_cast<uint8_t>((crc & 0x80) ? ((crc << 1) ^ 0x07) : (crc << 1));
    }
  }
  return crc;
}

uint8_t TinyProtocol::mirrorByte(uint8_t value) {
  uint8_t out = 0;
  for (uint8_t i = 0; i < 8; ++i) {
    if (value & (1u << i)) out = static_cast<uint8_t>(out | (1u << (7u - i)));
  }
  return out;
}

Result TinyProtocol::buildFrame(uint8_t opcode, const uint8_t* payload, size_t len,
                                uint8_t* out, size_t capacity, size_t& out_len) {
  out_len = 0;
  if (len > kMaxPayload) return Result::InvalidArgument;
  if (!out || (len > 0 && !payload)) return Result::InvalidArgument;
  if (capacity < kFrameOverhead + len) return Result::InvalidArgument;

  out[0] = kFrameMagic0;
  out[1] = kFrameMagic1;
  out[2] = opcode;
  out[3] = 0x00;  // host flag
  out[4] = static_cast<uint8_t>(len & 0xFF);
  out[5] = static_cast<uint8_t>((len >> 8) & 0xFF);
  if (len) memcpy(out + 6, payload, len);
  out[6 + len] = crc8(payload, len);
  out[7 + len] = kFrameEnd;
  out_len = kFrameOverhead + len;
  return Result::Ok;
}

Result TinyProtocol::validateFrame(const uint8_t* frame, size_t len, uint8_t& opcode,
                                   const uint8_t*& payload, size_t& payload_len) {
  payload = nullptr;
  payload_len = 0;
  opcode = 0;
  if (!frame || len < kFrameOverhead) return Result::InvalidArgument;
  if (frame[0] != kFrameMagic0 || frame[1] != kFrameMagic1) return Result::InvalidArgument;
  const size_t declared = static_cast<size_t>(frame[4]) | (static_cast<size_t>(frame[5]) << 8);
  if (declared + kFrameOverhead != len) return Result::InvalidArgument;
  if (frame[len - 1] != kFrameEnd) return Result::InvalidArgument;
  const uint8_t expected = crc8(frame + 6, declared);
  if (frame[len - 2] != expected) return Result::IoError;

  opcode = frame[2];
  payload = frame + 6;
  payload_len = declared;
  return Result::Ok;
}

Result TinyProtocol::sendCommand(PrinterTransport& transport, uint8_t opcode,
                                 const uint8_t* payload, size_t len) const {
  uint8_t frame[kFrameOverhead + kMaxPayload];
  size_t frame_len = 0;
  Result r = buildFrame(opcode, payload, len, frame, sizeof(frame), frame_len);
  if (r != Result::Ok) return r;
  size_t written = 0;
  r = transport.write(frame, frame_len, written);
  if (r != Result::Ok) return r;
  if (written != frame_len) return Result::IoError;
  return Result::Ok;
}

ProtocolCapabilities TinyProtocol::capabilities() const {
  ProtocolCapabilities c;
  c.implemented = true;
  c.framing = true;
  c.crc = true;
  c.init_command = true;
  c.bitmap_command = true;
  c.feed = true;
  c.status_parsing = true;
  c.note = "BLE-only validation pending: no physical printer available in V0.1";
  return c;
}

Result TinyProtocol::init(const ProtocolContext& ctx, PrinterTransport& transport) {
  // Wake / status request first (0xA3 [0x00]) and enforce notify subscription
  // at connect time in runtime/PrintSession (mandatory for iPrint devices).
  const uint8_t wake[1] = {0x00};
  Result r = sendCommand(transport, kGetStatus, wake, 1);
  if (r != Result::Ok) return r;

  const uint8_t quality[1] = {ctx.settings.quality};
  r = sendCommand(transport, kQuality, quality, 1);
  if (r != Result::Ok) return r;

  const uint8_t speed[1] = {ctx.settings.speed};
  r = sendCommand(transport, kSpeed, speed, 1);
  if (r != Result::Ok) return r;

  const uint8_t energy[2] = {static_cast<uint8_t>(ctx.settings.energy & 0xFF),
                             static_cast<uint8_t>(ctx.settings.energy >> 8)};
  r = sendCommand(transport, kEnergy, energy, 2);
  if (r != Result::Ok) return r;

  const uint8_t mode[1] = {ctx.settings.drawing_mode};
  r = sendCommand(transport, kDrawingMode, mode, 1);
  if (r != Result::Ok) return r;

  // 0xA6 lattice is required by GB01-family but known to crash SC03h.
  if (ctx.settings.use_lattice) {
    r = sendCommand(transport, kLattice, kLatticeStart, sizeof(kLatticeStart));
    if (r != Result::Ok) return r;
  }
  return Result::Ok;
}

Result TinyProtocol::printRow(const ProtocolContext& ctx, const uint8_t* row,
                              size_t row_bytes, PrinterTransport& transport) {
  if (!row || row_bytes != ctx.width_bytes) return Result::InvalidArgument;
  // Bounded by the frame payload limit. No fixed-size stack buffer: any row
  // width (including > 64 bytes) is either handled safely or rejected.
  if (row_bytes > kMaxRowBytes) return Result::InvalidArgument;
  if (!ctx.settings.mirror_rows) {
    return sendCommand(transport, kPrintRow, row, row_bytes);
  }
  std::vector<uint8_t> mirrored(row_bytes);
  for (size_t i = 0; i < row_bytes; ++i) mirrored[i] = mirrorByte(row[i]);
  return sendCommand(transport, kPrintRow, mirrored.data(), row_bytes);
}

Result TinyProtocol::finish(const ProtocolContext& ctx, PrinterTransport& transport) {
  if (ctx.settings.use_lattice) {
    Result r = sendCommand(transport, kLattice, kLatticeEnd, sizeof(kLatticeEnd));
    if (r != Result::Ok) return r;
  }
  return feed(ctx, ctx.settings.feed_dots, transport);
}

Result TinyProtocol::feed(const ProtocolContext& ctx, uint16_t dots,
                          PrinterTransport& transport) {
  (void)ctx;
  uint16_t remaining = dots;
  while (remaining > 0) {
    const uint8_t step = remaining > 100 ? 100 : static_cast<uint8_t>(remaining);
    // CatGFX / COMMANDS.md: first byte = rows, second byte = 0x00.
    const uint8_t payload[2] = {step, 0x00};
    Result r = sendCommand(transport, kFeed, payload, 2);
    if (r != Result::Ok) return r;
    remaining = static_cast<uint16_t>(remaining - step);
  }
  return Result::Ok;
}

Result TinyProtocol::statusRequest(PrinterTransport& transport) {
  const uint8_t payload[1] = {0x00};
  return sendCommand(transport, kGetStatus, payload, 1);
}

Result TinyProtocol::parseStatus(const uint8_t* data, size_t len,
                                 PrinterStatus& out) const {
  uint8_t opcode = 0;
  const uint8_t* payload = nullptr;
  size_t payload_len = 0;
  Result r = validateFrame(data, len, opcode, payload, payload_len);
  if (r != Result::Ok) return r;

  out = PrinterStatus();
  out.valid = true;
  out.source_opcode = "";
  if (opcode == kGetStatus && payload_len >= 1) {
    const uint8_t s = payload[0];
    out.raw = s;
    out.no_paper = (s & 0x01) != 0;
    out.cover_open = (s & 0x02) != 0;
    out.overheat = (s & 0x04) != 0;
    out.low_battery = (s & 0x08) != 0;
    out.ready = (s & 0x80) != 0 || (s & 0x0F) == 0;
    out.source_opcode = "A3";
  } else if (opcode == kFlowControl && payload_len >= 1) {
    out.flow_pause = payload[0] == 0x10;
    out.source_opcode = "AE";
  } else if (opcode == kDeviceInfo) {
    out.source_opcode = "A8";
  }
  return Result::Ok;
}

}  // namespace rtb

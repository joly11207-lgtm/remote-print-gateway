#pragma once

#include <stddef.h>
#include <stdint.h>

namespace rtb {

enum class ProtocolFamily : uint8_t {
  Unknown = 0,
  TinyIPrint = 1,
  EscPos = 2,
  PeriPage = 3,
  Paperang = 4,
};

inline const char* protocolFamilyName(ProtocolFamily f) {
  switch (f) {
    case ProtocolFamily::TinyIPrint: return "tiny_iprint";
    case ProtocolFamily::EscPos: return "escpos";
    case ProtocolFamily::PeriPage: return "peripage";
    case ProtocolFamily::Paperang: return "paperang";
    default: return "unknown";
  }
}

inline ProtocolFamily protocolFamilyFromName(const char* name) {
  if (!name) return ProtocolFamily::Unknown;
  const char* n = name;
  if (n[0] == 't') return ProtocolFamily::TinyIPrint;
  if (n[0] == 'e') return ProtocolFamily::EscPos;
  if (n[0] == 'p' && n[1] == 'e') return ProtocolFamily::PeriPage;
  if (n[0] == 'p' && n[1] == 'a') return ProtocolFamily::Paperang;
  return ProtocolFamily::Unknown;
}

// Stateless wire-format settings. Derived from a DeviceProfile by the catalog.
struct PrintSettings {
  uint16_t energy = 0x3E80;   // 0xAF, little endian on the wire
  uint8_t quality = 0x33;     // 0xA4 (0x33 = quality3 on GB01)
  uint8_t speed = 35;         // 0xBD (GB01 image mode)
  uint8_t drawing_mode = 0;   // 0xBE, 0 = image mode
  uint16_t feed_dots = 64;    // 0xA1, trailing feed
  uint16_t row_delay_ms = 10; // pacing owned by runtime, not by the protocol
  bool use_lattice = true;    // 0xA6; disabled on SC03h family (crashes)
  bool mirror_rows = true;    // MSB-first raster -> printer LSB-first row
};

struct ProtocolContext {
  uint16_t width_dots = 384;
  uint16_t width_bytes = 48;
  PrintSettings settings;
};

struct PrinterStatus {
  bool valid = false;
  uint8_t raw = 0;
  bool no_paper = false;
  bool cover_open = false;
  bool overheat = false;
  bool low_battery = false;
  bool ready = false;
  bool flow_pause = false;   // 0xAE [0x10] received from printer
  const char* source_opcode = "";
};

struct ProtocolFingerprint {
  ProtocolFamily family = ProtocolFamily::Unknown;
  int confidence = 0;         // 0 = no evidence
  const char* evidence = "none";
};

struct ProtocolCapabilities {
  bool implemented = false;
  bool framing = false;
  bool crc = false;
  bool init_command = false;
  bool bitmap_command = false;
  bool feed = false;
  bool status_parsing = false;
  const char* note = "";
};

}  // namespace rtb

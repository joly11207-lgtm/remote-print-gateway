#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>

namespace rtb {

// One discovered device, flattened to the fields the real BLE scanner can
// actually provide (src/transport/ble/BleTransport.cpp:61-72).
//
// Deliberately absent: manufacturer data, TX power, a full service list and any
// characteristic enumeration. The firmware cannot observe those, so they are
// not rendered as if it could. `service_uuid` is the single advertised service
// the scan captured; `characteristics` never appears because V1.2 does not
// connect to a printer.
struct SerialScanDevice {
  const char* name = "";
  const char* address = "";
  int rssi = -127;
  const char* service_uuid = "";
  const char* fingerprint = "Unknown";
  int confidence = 0;
  const char* evidence = "none";
  const char* model_id = "unmatched";
  const char* match_reason = "none";
};

// Minimal serial control channel (Website V1.1 identity, V1.2 discovery).
//
// Wire format: newline-delimited JSON on the same UART the firmware logs to.
// Exactly two commands are accepted:
//
//   {"type":"gateway_info"}
//     -> {"type":"gateway_info","gateway_id":"...","hardware_family":"...",
//         "firmware_version":"...","protocol_version":1}
//   {"type":"scan"}
//     -> {"type":"scan_result","result":"OK","transport":"ble","scanned":N,
//         "elapsed_ms":M,"devices":[...]}
//
// Everything else - print, raw, firmware update, arbitrary JSON - is refused
// with {"type":"error",...} and has no side effect. Human log lines are not
// commands: anything that does not start with '{' is ignored, so a machine
// client can filter on the leading brace and a serial monitor still reads the
// boot log it expects.
//
// This class owns no radio and no printer state: `scan` is only *reported* to
// the caller, which is the only place allowed to touch the transport.
class SerialControl {
 public:
  // Longest accepted command line. Anything longer is dropped and refused, so
  // a client cannot grow the firmware's line buffer.
  static constexpr size_t kMaxLineBytes = 256;
  static constexpr int kProtocolVersion = 1;

  enum class Command {
    kNone,    // blank line or human debug log: stay silent
    kInfo,    // Dispatch::reply is ready to print
    kScan,    // caller must run the radio, then call scanLine()
    kError,   // Dispatch::reply is ready to print
  };

  struct Dispatch {
    Command command = Command::kNone;
    std::string reply;
    // Requested scan duration in ms, 0 when the client did not ask. The caller
    // clamps it: a client can shorten or lengthen the scan, never drive it.
    int duration_ms = 0;
  };

  void setIdentity(const std::string& gateway_id,
                   const std::string& hardware_family,
                   const std::string& firmware_version);

  // Scan window bounds. A client may ask for a different value, but only this
  // range is ever granted: the scan blocks the main loop.
  static constexpr uint32_t kScanDefaultMs = 4000;
  static constexpr uint32_t kScanMinMs = 1000;
  static constexpr uint32_t kScanMaxMs = 8000;
  static uint32_t clampScanDuration(int requested_ms);

  // Classifies one line and builds the reply for everything that needs no I/O.
  Dispatch dispatch(const char* line, size_t len) const;
  Dispatch dispatch(const std::string& line) const;

  std::string infoLine() const;

  // Renders a scan_result line (no trailing newline, no embedded newline).
  std::string scanLine(const SerialScanDevice* devices,
                       size_t count,
                       const char* result_name,
                       unsigned long elapsed_ms,
                       size_t scanned) const;

  std::string errorLine(const char* code) const;

  const std::string& gatewayId() const { return gateway_id_; }
  const std::string& hardwareFamily() const { return hardware_family_; }
  const std::string& firmwareVersion() const { return firmware_version_; }

 private:
  std::string gateway_id_;
  std::string hardware_family_;
  std::string firmware_version_;
};

}  // namespace rtb

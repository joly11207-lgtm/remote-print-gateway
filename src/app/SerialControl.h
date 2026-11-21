#pragma once

#include <stddef.h>

#include <string>

namespace rtb {

// Minimal serial control channel (Website V1.1, USB identity handshake).
//
// Wire format: newline-delimited JSON on the same UART the firmware logs to.
// The ONLY accepted command is:
//
//   {"type":"gateway_info"}
//
// answered with one line:
//
//   {"type":"gateway_info","gateway_id":"gw-esp32-1a2b3c",
//    "hardware_family":"ESP32-WROOM-32E","firmware_version":"0.1.0",
//    "protocol_version":1}
//
// Everything else - scan, print, raw, firmware updates, arbitrary JSON - is
// refused with {"type":"error","error":"unsupported_command",...} and has no
// side effect of any kind. The caller echoes the returned line verbatim.
//
// Human log lines ("Remote Thermal Printer Bridge V0.1 ...", "[HEAP] ...") are
// NOT commands: anything that does not start with '{' is ignored, so a machine
// client can filter on the leading brace and a serial monitor still reads the
// boot log it expects.
//
// No Arduino dependencies: this is compiled by the host tests as-is.
class SerialControl {
 public:
  // Longest accepted command line. Anything longer is dropped and refused, so
  // a client cannot grow the firmware's line buffer.
  static constexpr size_t kMaxLineBytes = 256;
  static constexpr int kProtocolVersion = 1;

  void setIdentity(const std::string& gateway_id,
                   const std::string& hardware_family,
                   const std::string& firmware_version);

  // Returns the response line WITHOUT the trailing newline, or an empty string
  // when the input is not a machine command (blank line, debug log, comment).
  std::string handleLine(const char* line, size_t len);
  std::string handleLine(const std::string& line);

  const std::string& gatewayId() const { return gateway_id_; }
  const std::string& hardwareFamily() const { return hardware_family_; }
  const std::string& firmwareVersion() const { return firmware_version_; }

 private:
  std::string identityJson() const;
  std::string errorJson(const char* code) const;

  std::string gateway_id_;
  std::string hardware_family_;
  std::string firmware_version_;
};

}  // namespace rtb

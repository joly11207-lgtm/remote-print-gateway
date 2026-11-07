#pragma once

#include <stddef.h>

#include <string>

#include "boards/BoardCapabilities.h"
#include "protocols/base/ProtocolTypes.h"
#include "transport/base/TransportTypes.h"

namespace rtb {

struct CompatibilityInput {
  const BoardCapabilities* board = nullptr;
  TransportCapabilities ble;
  TransportCapabilities bluetooth_classic;
  TransportCapabilities classic_mock;
  TransportCapabilities mock;
  ProtocolCapabilities tiny;
  ProtocolCapabilities escpos;
  size_t profile_count = 0;
  bool wifi_connected = false;
  const char* printer_state = "offline";
  const char* active_transport = "none";
  size_t pending_jobs = 0;
};

// Machine-readable Compatibility Report. Explicitly records what has NOT been
// verified on hardware; V0.1 must never claim a successful real print.
std::string buildCompatibilityReport(const CompatibilityInput& input);

}  // namespace rtb

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "printers/catalog/DeviceCatalog.h"
#include "transport/base/PrinterTransport.h"

namespace rtb {

enum class DiscoveryStage : uint8_t {
  None = 0,
  Ae30Filtered = 1,       // matched in the first, AE30-filtered scan
  UnfilteredFallback = 2, // matched only after the unfiltered second scan
  NotFound = 3,
};

const char* discoveryStageName(DiscoveryStage stage);

struct DiscoveryResult {
  Result result = Result::NotFound;
  DiscoveryStage stage = DiscoveryStage::None;
  size_t scan_rounds = 0;
  size_t endpoints_seen = 0;
  CatalogMatch match;
  Endpoint endpoint;  // as advertised; UUIDs are filled by prepareConnectEndpoint
};

// Two-phase BLE discovery, transport-agnostic and free of connection state:
//   phase 1: advertisement filtered for AE30 (cheap, precise)
//   phase 2: if no implemented Tiny/iPrint profile was matched, scan again
//            with NO service UUID filter and let DeviceCatalog use name
//            patterns / advertisement evidence / protocol fingerprint.
// Some real AE30 printers support the service but do not advertise it.
// This function never connects, so host Mock tests can exercise it.
DiscoveryResult discoverPrinter(PrinterTransport& transport,
                                uint32_t filtered_duration_ms = 3000,
                                uint32_t fallback_duration_ms = 3000);

// Fills service/write/notify UUIDs from the resolved profile before connect().
// This is what keeps the final GATT service/characteristic check strict even
// when the device was found by name only in the fallback phase.
Endpoint prepareConnectEndpoint(const Endpoint& discovered, const DeviceProfile& profile);

}  // namespace rtb

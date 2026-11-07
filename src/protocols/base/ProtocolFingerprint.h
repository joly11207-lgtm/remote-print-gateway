#pragma once

#include "protocols/base/ProtocolTypes.h"
#include "transport/base/TransportTypes.h"

namespace rtb {

// Stateless identification from advertisement / GATT evidence only.
// It never claims that a printer was actually contacted.
ProtocolFingerprint fingerprintFromEndpoint(const Endpoint& endpoint);

bool containsIgnoreCase(const std::string& haystack, const std::string& needle);
bool nameMatchesPattern(const std::string& device_name, const char* pattern);

}  // namespace rtb

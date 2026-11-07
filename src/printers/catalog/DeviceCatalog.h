#pragma once

#include <stddef.h>

#include <string>

#include "printers/catalog/DeviceProfile.h"
#include "protocols/base/ProtocolFingerprint.h"
#include "protocols/base/ProtocolTypes.h"
#include "transport/base/TransportTypes.h"

namespace rtb {

struct CatalogMatch {
  const DeviceProfile* profile = nullptr;
  ProtocolFingerprint fingerprint;
  int score = 0;
  const char* match_reason = "none";
};

class DeviceCatalog {
 public:
  static size_t count();
  static const DeviceProfile& at(size_t index);
  static const DeviceProfile* findByModelId(const char* model_id);

  // Resolves an Endpoint into a DeviceProfile using transport support, protocol
  // fingerprint, GATT UUIDs and name patterns. Does no I/O.
  static CatalogMatch match(const Endpoint& endpoint);

  // Serialises the compiled-in catalog so tests/test_profiles.py can verify that
  // printers/profiles/*.json and the firmware table never drift apart.
  static void toJson(std::string& out);
};

}  // namespace rtb

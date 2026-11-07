#pragma once

#include <stddef.h>
#include <stdint.h>

#include "transport/base/TransportTypes.h"

namespace rtb {

// OTA basic INTERFACE reservation for V0.1. No firmware writer is implemented.
// Future entry point (LATER, after hardware and memory tests):
//   [1] choose a partition scheme with two OTA slots for the production module
//   [2] implement begin()/write()/end() with Update.h or ESP-IDF esp_ota_ops
//   [3] add authentication + integrity check before committing the image
class OtaService {
 public:
  bool interfaceReserved() const { return true; }
  bool available() const { return false; }  // not functional in V0.1
  const char* state() const { return "reserved_not_implemented"; }

  Result begin(size_t image_size);
  Result write(const uint8_t* data, size_t len);
  Result end();

  const char* entryPointNote() const {
    return "LATER: OTA writer, dual OTA slots, authenticated image commit";
  }

 private:
  size_t image_size_ = 0;
};

}  // namespace rtb

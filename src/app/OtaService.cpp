#include "app/OtaService.h"

namespace rtb {

Result OtaService::begin(size_t image_size) {
  image_size_ = image_size;
  // Interface only: no Update.h / esp_ota_ops reference in V0.1.
  return Result::NotImplemented;
}

Result OtaService::write(const uint8_t* data, size_t len) {
  (void)data;
  (void)len;
  return Result::NotImplemented;
}

Result OtaService::end() {
  image_size_ = 0;
  return Result::NotImplemented;
}

}  // namespace rtb

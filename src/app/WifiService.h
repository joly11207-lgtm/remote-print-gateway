#pragma once

#include <stdint.h>

#include <string>

namespace rtb {

// Wi-Fi STA with an AP fallback, plus the Wi-Fi/BLE coexistence handling
// borrowed from the MIT ahmetozer bridge: do not treat a WiFi drop during a
// BLE job as fatal, and kick a fresh WiFi.begin() when BLE releases the radio.
class WifiService {
 public:
  void begin();
  void tick(uint32_t now_ms);

  bool isConnected() const;
  bool isApMode() const;
  std::string localIp() const;
  const char* modeName() const;

  void setBleBusy(bool busy);

 private:
  void kickReconnect();

  bool sta_mode_ = false;
  bool ble_busy_ = false;
  bool was_ble_busy_ = false;
  uint32_t last_reconnect_ms_ = 0;
};

}  // namespace rtb

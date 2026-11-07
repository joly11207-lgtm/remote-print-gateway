#include "app/WifiService.h"

#include "config/RtbConfig.h"

#if RTB_ENABLE_WIFI && defined(ARDUINO)
#include <WiFi.h>
#include "app/WifiConfig.h"
#endif

namespace rtb {

#if RTB_ENABLE_WIFI && defined(ARDUINO)

void WifiService::begin() {
  if (RTB_WIFI_SSID[0] == '\0') {
    sta_mode_ = false;
    WiFi.mode(WIFI_AP);
    WiFi.softAP(RTB_AP_SSID);
    return;
  }
  sta_mode_ = true;
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(RTB_WIFI_SSID, RTB_WIFI_PASS);
}

void WifiService::kickReconnect() {
  if (!sta_mode_) return;
  WiFi.disconnect(false, true);
  WiFi.begin(RTB_WIFI_SSID, RTB_WIFI_PASS);
}

void WifiService::tick(uint32_t now_ms) {
  if (!sta_mode_ || ble_busy_) return;  // radio is owned by BLE right now
  if (was_ble_busy_) {
    // Just released by BLE: proactively restart the STA link.
    was_ble_busy_ = false;
    kickReconnect();
    last_reconnect_ms_ = now_ms;
    return;
  }
  if (WiFi.status() != WL_CONNECTED && now_ms - last_reconnect_ms_ > 10000) {
    kickReconnect();
    last_reconnect_ms_ = now_ms;
  }
}

bool WifiService::isConnected() const { return WiFi.status() == WL_CONNECTED; }
bool WifiService::isApMode() const { return !sta_mode_; }

std::string WifiService::localIp() const {
  return std::string(WiFi.localIP().toString().c_str());
}

const char* WifiService::modeName() const {
  if (!sta_mode_) return "ap";
  return isConnected() ? "sta_connected" : "sta_disconnected";
}

void WifiService::setBleBusy(bool busy) {
  if (ble_busy_ && !busy) was_ble_busy_ = true;
  ble_busy_ = busy;
}

#else  // host build: no radio

void WifiService::begin() {}
void WifiService::tick(uint32_t) {}
bool WifiService::isConnected() const { return false; }
bool WifiService::isApMode() const { return false; }
std::string WifiService::localIp() const { return "127.0.0.1"; }
const char* WifiService::modeName() const { return "host_disabled"; }
void WifiService::setBleBusy(bool busy) { ble_busy_ = busy; }

#endif

}  // namespace rtb

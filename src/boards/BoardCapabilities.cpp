#include "boards/BoardCapabilities.h"

#include "config/RtbConfig.h"

#if defined(ARDUINO)
#include <Arduino.h>
#endif

namespace rtb {

uint32_t detectFlashSize() {
#if defined(ARDUINO)
  const uint32_t detected = static_cast<uint32_t>(ESP.getFlashChipSize());
  return detected ? detected : RTB_TARGET_DEFAULT_FLASH;
#else
  return RTB_TARGET_DEFAULT_FLASH;
#endif
}

uint32_t detectPsramSize() {
#if defined(ARDUINO) && defined(BOARD_HAS_PSRAM)
  return static_cast<uint32_t>(ESP.getPsramSize());
#elif defined(RTB_HOST_TEST)
  return RTB_TARGET_HAS_PSRAM ? 8u * 1024u * 1024u : 0u;
#else
  return 0;
#endif
}

namespace {
BoardCapabilities makeCapabilities() {
  BoardCapabilities c;
  c.wifi = RTB_TARGET_CHIP_HAS_WIFI != 0;
  c.ble = RTB_TARGET_CHIP_HAS_BLE != 0;
  // ESP32-S3 is BLE-only silicon. ESP32 (WROOM-32E) is dual-mode.
  // This is a chip capability, NOT a claim that a Classic transport exists.
  c.bluetooth_classic = RTB_TARGET_CHIP_HAS_CLASSIC != 0;
  c.psram_size = detectPsramSize();
  c.psram = c.psram_size > 0;
  c.flash_size = detectFlashSize();
  c.ota = true;  // interface reserved in V0.1, see app/OtaService
  c.dual_core = RTB_TARGET_DUAL_CORE != 0;
  c.target_id = RTB_TARGET_ID;
  c.target_label = RTB_TARGET_LABEL;
  c.chip_name = RTB_TARGET_CHIP_NAME;
  return c;
}
}  // namespace

const BoardCapabilities& boardCapabilities() {
  static BoardCapabilities caps = makeCapabilities();
  return caps;
}

void refreshBoardCapabilities() {
  BoardCapabilities& caps = const_cast<BoardCapabilities&>(boardCapabilities());
  caps.psram_size = detectPsramSize();
  caps.psram = caps.psram_size > 0;
  caps.flash_size = detectFlashSize();
}

}  // namespace rtb

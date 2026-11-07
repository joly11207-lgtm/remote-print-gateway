#pragma once

#include <stdint.h>

namespace rtb {

// Hardware capability surface. Protocol and transport code must never assume a
// specific board; ask this structure instead.
struct BoardCapabilities {
  bool wifi;
  bool ble;
  bool bluetooth_classic;   // chip-level capability (ESP32 = true, ESP32-S3 = false)
  bool psram;               // PSRAM actually detected / enabled
  uint32_t psram_size;      // bytes
  uint32_t flash_size;      // bytes
  bool ota;                 // OTA interface available (V0.1: interface only)
  bool dual_core;
  const char* target_id;
  const char* target_label;
  const char* chip_name;
};

// Compile-time target constants + runtime flash/PSRAM detection.
const BoardCapabilities& boardCapabilities();

// Re-evaluates runtime-detected fields. Safe to call from setup() after PSRAM init.
void refreshBoardCapabilities();

uint32_t detectFlashSize();
uint32_t detectPsramSize();

}  // namespace rtb

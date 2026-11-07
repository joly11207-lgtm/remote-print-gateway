#pragma once

// Remote Thermal Printer Bridge V0.1
// Central build switches. Every target is selected explicitly by the build system.
// Nothing in this file (or anywhere else in V0.1) enables Bluetooth Classic APIs.

#define RTB_VERSION "0.1.0-skeleton"

#if defined(RTB_HOST_TEST)

#if defined(RTB_HOST_EMULATE_WROOM32E)
#define RTB_TARGET_ID               "host-test-emulating-wroom32e"
#define RTB_TARGET_LABEL            "HOST (WROOM-32E capability emulation)"
#define RTB_TARGET_CHIP_NAME        "HOST-EMULATED-ESP32"
#define RTB_TARGET_CHIP_HAS_WIFI    1
#define RTB_TARGET_CHIP_HAS_BLE     1
#define RTB_TARGET_CHIP_HAS_CLASSIC 1
#define RTB_TARGET_HAS_PSRAM        0
#define RTB_TARGET_DUAL_CORE        1
#define RTB_TARGET_DEFAULT_FLASH    (4u * 1024u * 1024u)
#else
#define RTB_TARGET_ID               "host-test-emulating-s3"
#define RTB_TARGET_LABEL            "HOST (ESP32-S3 capability emulation)"
#define RTB_TARGET_CHIP_NAME        "HOST-EMULATED-ESP32-S3"
#define RTB_TARGET_CHIP_HAS_WIFI    1
#define RTB_TARGET_CHIP_HAS_BLE     1
#define RTB_TARGET_CHIP_HAS_CLASSIC 0
#define RTB_TARGET_HAS_PSRAM        1
#define RTB_TARGET_DUAL_CORE        1
#define RTB_TARGET_DEFAULT_FLASH    (16u * 1024u * 1024u)
#endif

#elif defined(RTB_TARGET_S3_SUPERMINI)

#define RTB_TARGET_ID               "esp32-s3-supermini"
#define RTB_TARGET_LABEL            "ESP32-S3 SuperMini (8MB flash, no PSRAM)"
#define RTB_TARGET_CHIP_NAME        "ESP32-S3"
#define RTB_TARGET_CHIP_HAS_WIFI    1
#define RTB_TARGET_CHIP_HAS_BLE     1
#define RTB_TARGET_CHIP_HAS_CLASSIC 0
#define RTB_TARGET_HAS_PSRAM        0
#define RTB_TARGET_DUAL_CORE        1
#define RTB_TARGET_DEFAULT_FLASH    (8u * 1024u * 1024u)

#elif defined(RTB_TARGET_WROOM32E)

// Future production candidate. Target/board abstraction only: no board on the desk.
#define RTB_TARGET_ID               "esp32-wroom-32e-future"
#define RTB_TARGET_LABEL            "ESP32-WROOM-32E (future, NOT hardware verified)"
#define RTB_TARGET_CHIP_NAME        "ESP32"
#define RTB_TARGET_CHIP_HAS_WIFI    1
#define RTB_TARGET_CHIP_HAS_BLE     1
#define RTB_TARGET_CHIP_HAS_CLASSIC 1
#define RTB_TARGET_HAS_PSRAM        0
#define RTB_TARGET_DUAL_CORE        1
#define RTB_TARGET_DEFAULT_FLASH    (4u * 1024u * 1024u)

#else  // default: main development board

#define RTB_TARGET_ID               "esp32-s3-n16r8"
#define RTB_TARGET_LABEL            "ESP32-S3 N16R8 (16MB flash, 8MB OPI PSRAM)"
#define RTB_TARGET_CHIP_NAME        "ESP32-S3"
#define RTB_TARGET_CHIP_HAS_WIFI    1
#define RTB_TARGET_CHIP_HAS_BLE     1
#define RTB_TARGET_CHIP_HAS_CLASSIC 0
#define RTB_TARGET_HAS_PSRAM        1
#define RTB_TARGET_DUAL_CORE        1
#define RTB_TARGET_DEFAULT_FLASH    (16u * 1024u * 1024u)

#endif

// ---- Compile conditions -----------------------------------------------------
// V0.1 implements BLE only. Bluetooth Classic implementation is intentionally OFF
// for every target, including the future WROOM-32E target.
#ifndef RTB_ENABLE_BT_CLASSIC_IMPL
#define RTB_ENABLE_BT_CLASSIC_IMPL 0
#endif

#if RTB_ENABLE_BT_CLASSIC_IMPL && !RTB_TARGET_CHIP_HAS_CLASSIC
#error "Bluetooth Classic APIs cannot be enabled on ESP32-S3: BLE-only silicon. See src/transport/bt_classic/BluetoothClassicTransport.h"
#endif

#if defined(RTB_HOST_TEST)
#define RTB_ENABLE_BLE 0
#define RTB_ENABLE_WIFI 0
#else
#define RTB_ENABLE_BLE 1
#define RTB_ENABLE_WIFI 1
#endif

// V0.1 reserves the OTA interface but does not implement a firmware writer.
#define RTB_ENABLE_OTA_IMPL 0

#define RTB_ERR_NOT_SUPPORTED_ON_THIS_TARGET "NOT_SUPPORTED_ON_THIS_TARGET"
#define RTB_ERR_NOT_IMPLEMENTED             "NOT_IMPLEMENTED"

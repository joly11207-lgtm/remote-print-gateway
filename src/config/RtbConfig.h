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

// ---- HTTP print request limits ----------------------------------------------
// RTB_MAX_JOB_BITMAP_BYTES is the LOGICAL protocol ceiling and must stay equal
// to the PrintQueue default max_job_bytes (64 KB) so the parser never accepts a
// bitmap the queue would reject. It is NOT a statement about available heap;
// the runtime check lives in HeapBudget.h and answers 503, not 413.
//
// RTB_HTTP_MAX_BODY_BYTES applies to POST /api/print only, which is the SMALL /
// LEGACY JSON endpoint. Large print payloads MUST use POST /api/print/raw,
// because Arduino's WebServer mallocs the whole request body and then copies it
// into a String BEFORE our handler runs: that peak (2x body) is spent before any
// limit in this codebase can be evaluated, so a larger JSON ceiling would only
// move the crash, not prevent it.
//
// Why 8 KB:
//   * the largest legacy JSON job in the field is an X18 sheet, 64 rows x 48 B
//     = 3072 B -> 4096 base64 chars -> ~4.2 KB of body (576-dot heads: ~6.2 KB),
//     so 8 KB keeps every existing small client working with ~2x headroom;
//   * worst-case simultaneous footprint is bounded: transient 2 x 8192 B while
//     the framework parses, then 8192 B (String) + <= 6146 B (decoded bitmap)
//     = ~22 KB peak, which fits a WROOM-32E that reports ~45 KB free with
//     Wi-Fi + BLE up, while still leaving the queue and network stack room.
#define RTB_MAX_JOB_BITMAP_BYTES (64u * 1024u)
#define RTB_HTTP_MAX_BODY_BYTES  (8u * 1024u)

// Heap telemetry for hardware validation only ([HEAP] lines on the serial log).
// Off unless a target opts in with -DRTB_DEBUG_HEAP=1.
#ifndef RTB_DEBUG_HEAP
#define RTB_DEBUG_HEAP 0
#endif

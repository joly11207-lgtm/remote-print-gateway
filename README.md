# Remote Thermal Printer Bridge V0.1

ESP32-S3 based Wi-Fi -> BLE bridge for Tiny / iPrint / CatPrinter (AE30) thermal
printers. V0.1 is a **compile-verified + Mock-tested architecture skeleton**.

**There is no physical printer and no ESP32-WROOM-32E board on the desk.**
Nothing here claims a successful real print or a Classic Bluetooth validation.
Current tag: **`v0.1.2-pre-hardware`** (2026-11-08: fixed the real N16R8 board
`memory_type` from `opi_opi` to `qio_opi`; firmware flashed and boot/AP/HTTP/BLE
verified on hardware). `v0.1.1-pre-hardware` is kept as the pre-hardware baseline
that exposed the board-config error on its first real flash.

`v0.1.1-pre-hardware` (2026-11-07) covered callback lifetime, bounded bitmap
rows, two-phase BLE discovery, `uncertain_partial` mid-job semantics and
job_id-only dedup — no new features.

## NOW (V0.1)
ESP32-S3, Wi-Fi, BLE, Tiny/iPrint, HTTP JSON debug API, local print queue,
dedup, offline waiting, pre-print auto recovery, Mock Printer, Compatibility
Report, OTA interface reservation.

**Partial-print semantics:** a failure before any bitmap row is attempted stays
`waiting_printer` and may be auto-retried. Once print data was sent, the job is
frozen as `uncertain_partial` with `rows_attempted` / `rows_dispatched` /
`last_error`, excluded from `pending`, and **never auto-reprinted**. Physical
exactly-once is not guaranteed; a human must inspect the paper and, if wanted,
resubmit with a new `job_id`. That manual reprint is allowed even when the
content is identical.

**Dedup semantics:** `job_id` non-empty -> idempotency by `job_id` only
(`dedup_key = job_id`); identical content under a different `job_id` is a
separate, legal print. `job_id` empty -> `dedup_key = content_hash`, protecting
no-ID requests from accidental resubmission. A content hash never blocks a
valid `job_id`.

## LATER
Bluetooth Classic real implementation, ESC/POS, PeriPage, Paperang, Phomemo,
Cloudflare cloud, website/templates, user system, payment, PCB, enclosure.

## Hardware
- Main dev board: ESP32-S3 N16R8 (16 MB flash, 8 MB OPI PSRAM)
- Regression board: ESP32-S3 SuperMini (8 MB flash, no PSRAM, USB CDC)
- Future candidate: ESP32-WROOM-32E (Wi-Fi + BLE + Classic, OTA, queue).
  Flash/PSRAM part numbers are intentionally **not locked**.
- No external Classic Bluetooth module is used or planned by default.

## Layout
```
src/transport/base|ble|bt_classic|mock   Transport layer (I/O only)
src/protocols/base|tiny_iprint|escpos|peripage|paperang   Protocol layer
src/printers/catalog                     Device profiles + fingerprint matching
printers/profiles/*.json                 Canonical profile data
src/runtime                              Stateful session (pacing/flow/keepalive)
src/app                                  WiFi, HTTP, queue, report, OTA, orchestration
src/boards                               BoardCapabilities / capability detection
boards/                                  PlatformIO board definitions
tests/                                   Host Mock tests + profile consistency
tools/                                   Build / test scripts
docs/                                    Audit, test plan, V0.1 report
```

## Build (real firmware)
Dependencies: `pip install -r requirements.txt`, PlatformIO `espressif32@6.9.0`
(auto-installed by `pio`).

```powershell
powershell -ExecutionPolicy Bypass -File tools/build.ps1
```
Builds `s3_n16r8`, `s3_supermini` and `wroom32e_future`; logs in `build_logs/`.

Windows note: ESP32 xtensa GCC cannot read non-ASCII paths, so `tools/build.ps1`
mirrors the project to `C:\Users\Public\rtb-v01-build` and uses
`PLATFORMIO_CORE_DIR=C:\Users\Public\rtb-pio-core`. Override with `-Mirror` /
`-CoreDir`.

Board differences (recorded, not hidden):
| target | board | flash | PSRAM | partitions | USB |
|---|---|---|---|---|---|
| s3_n16r8 | esp32-s3-devkitc-1 | 16MB QUAD | OPI 8MB | default_16MB.csv | native USB / UART |
| s3_supermini | esp32-s3-supermini | 8MB | none | default_8MB.csv | native USB CDC on boot |
| wroom32e_future | esp32dev | 4MB | none | huge_app.csv | USB-UART |

`s3_n16r8` uses `board_build.arduino.memory_type = qio_opi` (QIO quad flash +
OPI PSRAM). The original `opi_opi` assumed **octal flash**, but the real module's
eFuse reports quad flash, so the first hardware flash aborted at `cpu_start` with
`Octal Flash option selected, but EFUSE not configured!`. No eFuse was changed.
Verified on hardware 2026-11-08, see `docs/V0.1_REPORT.md`.

`wroom32e_future` is **build/abstraction only**: no board present, no Classic
implementation, no hardware verification.

## Real hardware status (2026-11-08)
`s3_n16r8` flashed on COM6 and booted cleanly: no reboot loop, `setup()` runs,
flash `16777216` bytes (16 MB), PSRAM `8386279` bytes (~8 MB, `psram=true`),
`RTB-V0.1-AP` created, HTTP server answers `/api/status`,
`/api/capabilities`, `/api/scan` (8 real BLE advertisements seen). No panic /
Guru Meditation / watchdog / brownout. **No printer was connected; print
functionality is NOT validated.**

## Mock tests (no board required)
```powershell
powershell -ExecutionPolicy Bypass -File tools/run_mock_tests.ps1
```
Compiles the real C++ protocol/queue/transport code with Zig, runs the S3 and
WROOM-32E capability emulations, validates `printers/profiles/*.json` against
the compiled-in catalog, and runs **ASAN**, **UBSAN** and **ASAN+UBSAN
combined** regression suites, each with canaries proving the sanitizer
instrumentation is active. ASAN uses the Visual Studio BuildTools
`clang_rt.asan_dynamic-x86_64` runtime; the script fails loudly if it is
missing. Logs: `build_logs/*san*.log`.

Known V0.1 limitations (intentionally not fixed): HTTP debug API has **no
authentication**, the fallback AP `RTB-V0.1-AP` is **open**, and the queue is
**RAM-only** (lost on reboot).

## HTTP debug API (JSON only, not a website)
`GET /api/status`, `/api/capabilities`, `/api/compatibility`, `/api/queue`,
`/api/scan`, `GET|POST /api/ota` (501), `POST /api/print`, `/api/mock`,
`/api/connect`, `/api/disconnect`.

Example:
```bash
curl -X POST http://<ip>/api/mock -d '{"enabled":true,"offline":false}'
curl -X POST http://<ip>/api/print -d '{"job_id":"test-1","test":true}'
curl http://<ip>/api/queue
```

Wi-Fi credentials are optional: create `src/app/secrets.h` (gitignored) with
`RTB_WIFI_SSID` / `RTB_WIFI_PASS`; without it the board starts AP `RTB-V0.1-AP`.

See `docs/V0.1_REPORT.md`, `docs/OPEN_SOURCE_AUDIT.md` and
`docs/HARDWARE_TEST_PLAN.md`.

# Hardware arrival test plan

Trigger: the real CatPrinter / 喵喵机 (GB01-class) arrives. V0.1 currently has
**no physical printer** and **no ESP32-WROOM-32E board**.

Status 2026-11-08: **board-only steps 0 and 1 were executed** on the real
ESP32-S3 N16R8 (firmware flashed, boot/capabilities/AP/HTTP/BLE scanner
verified). Everything involving an actual printer (sections 2-7, except the
generic BLE scan smoke check) remains **unexecuted**. See
`docs/V0.1_REPORT.md` for the recorded hardware run.

This plan reflects `v0.1.2-pre-hardware`: two-phase discovery, bounded bitmap
rows, safe notification callback lifetime, `uncertain_partial` mid-job
semantics, `job_id`-only dedup, and the real-hardware `s3_n16r8` memory type
fix (`opi_opi` -> `qio_opi`). No feature/protocol/website work was added.

## 0. Preconditions
- ESP32-S3 N16R8 flashed with `s3_n16r8` build (`tools/build.ps1`).
  **Done 2026-11-08** with `memory_type=qio_opi`. The original `opi_opi`
  build aborted at `cpu_start` (`Octal Flash option selected, but EFUSE not
  configured!`) because the module has quad/QIO flash; no eFuse was changed.
- Printer fully charged, paper loaded, powered on (it auto-sleeps ~1 min idle).
- Serial monitor at 115200 and the board reachable over Wi-Fi STA or AP
  `RTB-V0.1-AP`.

## 1. Boot / capability check
1. Read serial boot banner; confirm no crash loop.
2. `GET /api/status` -> verify `chip`, `flash_size`, `psram_size`.
3. `GET /api/capabilities` -> verify:
   - `wifi=true`, `ble=true`, `bluetooth_classic=false`
   - `bluetooth_classic.available=false`, `unavailable_reason=NOT_SUPPORTED_ON_THIS_TARGET`
   - `verification.real_printer_tested=false` (must stay false until step 5 passes)
4. `POST /api/mock {"enabled":false}` -> switch to the real BLE transport.

**Executed 2026-11-08 (board only, no printer):** clean boot with banner, no
reboot loop in 35 s; `flash_size=16777216` (16 MB), `psram_size=8386279`
(~8 MB, `psram=true`); `wifi=true`, `ble=true`, `bluetooth_classic=false`;
AP `RTB-V0.1-AP` up and HTTP 200 on `/api/status` and `/api/capabilities`;
`/api/scan` returned 8 real BLE advertisements. Note the known stale report
flags in `docs/V0.1_REPORT.md` (`esp32_s3_n16r8_hardware_flashed=false` etc.).

## 2. BLE discovery / fingerprint
1. `GET /api/scan`; record `name`, `address`, `rssi`, `service_uuid`,
   `fingerprint`, `confidence`, `evidence`, `model_id`.
2. Verify service `AE30`, write `AE01`, notify `AE02` with nRF Connect as a
   cross-check.
3. If the printer supports AE30 but does **not advertise** it, verify the
   automatic connect path falls back: phase 1 scans with an `AE30` filter,
   phase 2 scans with no service UUID filter and matches by name pattern /
   advertisement evidence / fingerprint. Confirm the device is still matched
   to a Tiny/iPrint profile (host tests cover this; real radio is unverified).
4. Verify that connect still performs full GATT `AE30/AE01/AE02` discovery even
   after a name-only fallback match (the UUIDs come from the DeviceProfile).
5. If the name is not in the catalog, add a new `printers/profiles/*.json` +
   `BuiltinProfiles.cpp` entry (and re-run `tests/test_profiles.py`).
6. `POST /api/connect {"address":"<mac>","model_id":"<matched id>"}`.
7. Confirm `notifySubscribed=true` in the log (mandatory for iPrint family).

## 3. First real print (the gate)
1. `POST /api/print {"job_id":"hw-001","test":true}`.
2. Observe: does anything print at all? Is the checkerboard mirrored or
   reversed? Is it too light / too dark? Does the trailing feed look right?
3. Tune one variable at a time, re-run with a new `job_id`:
   - `mirror_rows` (bit reversal)
   - `energy`, `speed`, `quality`
   - `use_lattice` (if the printer hangs, disable it)
   - `row_delay_ms`, `write_chunk` (BLE stack lockup / truncated rows)
   - `feed_dots`
4. Only after a clean full-width test pattern do we call BLE printing
   "validated on hardware". Update the Compatibility Report verification flags
   in code at that point (never before).

## 4. Protocol / status validation
1. Capture the emitted frames from `MockTransport` tests and compare with a BLE
   sniffer trace if available.
2. Check `0xA3` status notifications (no paper / cover open / overheat / low
   battery / ready) and `0xA8` firmware info.
3. Check `0xAE` flow control (`0x10` pause / `0x00` resume) during a long job.
4. Confirm CRC-8 disagreements from `docs/OPEN_SOURCE_AUDIT.md` (feed CRC,
   payload byte order) against real behaviour.

## 5. Queue / offline / recovery (and partial-print semantics)
1. Power the printer OFF; enqueue 3 jobs with distinct `job_id`.
2. Re-post one of them with the SAME `job_id` -> expect `duplicate_job`.
2b. Re-post the same content under a DIFFERENT `job_id` -> expect `ok:true`
    (dedup is by `job_id` only; content hash never blocks a valid `job_id`).
    Also submit once with no `job_id`, then repeat -> expect `duplicate_job`.
3. `GET /api/queue` -> expect `waiting_printer` with `rows_attempted=0`.
4. Power printer ON; expect exponential-backoff scan (phase 1 AE30, phase 2
   unfiltered if needed), reconnect, and the jobs printing once, in order.
   This "no duplicate" expectation applies **only to failures that happen
   before any bitmap row is attempted**.
5. Power printer OFF **mid-job** (after rows have been sent). Expect
   `uncertain_partial`, NOT `waiting_printer`:
   - the job is terminal and is excluded from `pending`; `uncertain` >= 1;
   - `rows_attempted` / `rows_dispatched` and
     `last_error=uncertain_partial:<result>` are recorded;
   - **no automatic whole-job resend**, and **no claim of exactly-once**;
   - a human must inspect the physical paper; resubmit with a NEW `job_id`
     only if a reprint is actually wanted. That manual reprint with identical
     content MUST be accepted (not `duplicate_job`).
6. Confirm that other still-queued jobs can reconnect and print after one job
   has been frozen as `uncertain_partial`.

## 6. Wi-Fi/BLE coexistence
1. `ping` the board during a 300-row print; record latency/loss.
2. Call `GET /api/status` during printing; confirm HTTP server still responsive.
3. Watch for reason codes 201/202/39 and confirm `WifiService` reconnects.
4. If unstable, port the full dual-core split from `ahmetozer` (LATER).

## 7. S3 SuperMini regression
1. Flash `s3_supermini` target; verify USB CDC serial output, 8 MB flash,
   `psram=false`.
2. Repeat steps 1-5 and compare memory/heap headroom with N16R8.

## 8. Memory / part-number decision (only after real jobs)
1. Measure peak heap / PSRAM usage with realistic job sizes.
2. Decide Flash/PSRAM part numbers and the OTA partition scheme. **Do not lock
   a part number before this step.**

## 9. ESP32-WROOM-32E (explicitly NOT now)
Only after a real board exists:
1. Flash `wroom32e_future`, verify `bluetooth_classic=true` chip capability.
2. Confirm the Classic transport still reports `NOT_IMPLEMENTED`.
3. Implement SPP/RFCOMM per the entry points in
   `src/transport/bt_classic/BluetoothClassicTransport.h`, with the matching
   sdkconfig / dual-mode build.
4. Test with a real Classic SPP ESC/POS printer. Until then: no Classic claim.

## Acceptance criteria for leaving V0.1
- Step 3 test pattern prints correctly and repeatably on the real printer.
- Step 5 pre-print offline/recover produces no duplicate and no lost jobs;
  mid-job failures are frozen as `uncertain_partial` and never auto-reprinted.
- Both S3 boards compile and run; WROOM-32E remains a documented abstraction.
- HTTP debug API unauthenticated / open AP / RAM-only queue remain recorded as
  known V0.1 limitations; they are deliberately NOT fixed in this hotfix.

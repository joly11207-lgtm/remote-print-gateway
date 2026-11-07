# V0.1 Open-source audit

Audit date: 2026-11-06. All repositories were inspected via their public
README / docs / licence files. No repository was cloned into this project and
no third-party source file was vendored.

## Projects audited

| Project | Licence | What was used in V0.1 |
|---|---|---|
| `ahmetozer/Thermal-Cat-Printer-ESP32-Mqtt-ESCPOS` | MIT | Wi-Fi/BLE coexistence design (connect only on job, release radio after job, kick `WiFi.begin()` after BLE); command order `A3 -> A4 -> BD -> AF -> BE -> A6 -> A2* -> A6 -> A1`; GB01 test evidence. No code copied. |
| `TheNitek/CatGFX` | Unlicense (public domain) | Authoritative frame layout `51 78 <op> 00 <len> 00 <data> <crc8> FF`, CRC-8 table (poly `0x07`, init `0`), `0xA6` lattice constants, `MIRROR_TABLE` bit reversal, 20-byte GATT chunking, `{lines,0x00}` feed payload. Used as protocol *facts*, reimplemented compactly in our own code. |
| `FloksiPet/CatPrinter-ESP32-C3` | MIT | Reliability findings: >= 8 ms inter-row delay (BLE stack lockup below that), offline waiting + auto reconnect + duplicate-print protection idea, notify/stale-link handling. Its `PROTOCOL.md` was cross-checked and found inconsistent (see below). |
| `shubhambelbase/mini-print-studio` | MIT | Architecture shape `adapters/ (ble, bluetooth_classic, mock)` + `protocols/ (iprint, escpos, ...)`; CRC-8 poly 0x07 over payload only; 180-byte chunking and 10/25 ms pacing; mandatory notify subscription; 30 s `0xA3` watchdog; SC03h must NOT receive `0xA6`; queue states. |
| `Dejniel/TiMini-Print` | Apache-2.0 | `docs/architecture.md` package boundaries: protocol = stateless wire format, transport = I/O only, runtime/printing = stateful session, devices/catalog = no I/O. Adopted as our layering rule. No code copied; Apache-2.0 NOTICE/attribution would be required if code were ever vendored. |
| `NaitLee/Cat-Printer` (upstream protocol origin) | GPL-3.0-or-later for `printer.py` / `server.py` / `main.js`; CC0-1.0 for other parts | Only README + licence + public-domain protocol documentation (`amber-sixel/catprinter` `COMMANDS.md`) were read. **No GPL code was copied.** |
| Paperang / PeriPage related projects | mixed / unclear | **Studied only within licence-permitted scope.** V0.1 contains only empty stub headers (`PeriPageProtocol.h`, `PaperangProtocol.h`); no code, protocol tables or assets were copied. Real support is LATER and requires a fresh licence review. |

## Protocol facts adopted (and conflicts found)

| Topic | Conflict between sources | V0.1 decision |
|---|---|---|
| Frame length | `CatGFX`: `buffer[4]=len, buffer[5]=0`; `mini-print-studio`: `<len16le>` | Implemented 16-bit little-endian length; payload capped at 255 for V0.1 |
| CRC | `CatGFX` + `mini-print-studio`: CRC-8 poly `0x07`, init `0`, payload only. `FloksiPet/PROTOCOL.md` example claims `CRC8([00,1E])=0x5A`; standard algorithm gives `0x81` | Implemented standard CRC-8 (`0x81` for that vector, `0xF4` for `"123456789"`). Needs real-printer confirmation |
| Row width | `FloksiPet`: 50 bytes / 400 dots; `CatGFX`/`COMMANDS.md`/`mini-print-studio`: 48 bytes / 384 dots | 384 dots / 48 bytes, per `width_dots` in DeviceProfile |
| Feed payload | `CatGFX`/`COMMANDS.md`: `{lines, 0x00}`; `FloksiPet`: `{high, low}` | `{lines, 0x00}`, split into <= 100-dot chunks |
| Row bit order | `COMMANDS.md`: LSB-first, 1=dark; `mini-print-studio`: MSB-first, 1=black; `CatGFX` mirrors bytes | Profile flag `mirror_rows` (`true` on GB01 family, `false` on SC03h family) |
| `0xA6` lattice | Required by CatGFX / GB01; **crashes SC03h** per mini-print-studio | Profile flag `use_lattice`; lattice is skipped for SC03h |
| Pacing | >= 8 ms (FloksiPet), 10/25 ms + 4 KB bursts (mini-print-studio), 20-byte writes (CatGFX) | `row_delay_ms` per profile (10 ms default, hard floor 8 ms in `PrintSession`), `write_chunk` per profile (20 / 180) |
| Energy | `CatGFX` `{0x80,0x3E}` = `0x3E80` little-endian; `FloksiPet` says high-byte-first | Little-endian `{lo,hi}` (CatGFX / `COMMANDS.md`) |

## What was deliberately NOT rebuilt from scratch

- **BLE Cat Printer protocol**: all byte-level facts above come from mature
  implementations. Our `TinyProtocol.cpp` is a compact, independently written
  implementation (not copied source) so that it can be compiled and unit-tested
  on a host with no ESP32 and remain transport-agnostic. On hardware arrival, a
  vetted MIT/Unlicense implementation can replace that single file without
  touching any Transport/Queue/App code. This is recorded as a known deviation.
- **Bitmap pipeline**: V0.1 only does threshold -> 1bpp MSB-first rows -> mirror.
  Dithering / QR / barcode / rendering is LATER and will reuse the browser-side
  or Python pipelines from `ahmetozer` and `mini-print-studio` with attribution.
- **Wi-Fi/BLE coexistence**: single-loop `WifiService::setBleBusy()` +
  post-BLE reconnect kick (simplified from ahmetozer's dual-core split).
  Full dual-core FreeRTOS queue split is LATER.
- **queue / reconnect / dedup**: patterns from `mini-print-studio` (job states),
  `FloksiPet` (offline wait, auto restore) and `ahmetozer` (connect on demand).
- **protocol catalog / device profile**: boundary rules from `TiMini-Print`,
  JSON profile shape from this project's requirement.

## Licence compliance statement

- No GPL-licensed source was copied. Only protocol *facts* and public-domain
  documentation were used; facts/protocols are not copyrightable expression.
- MIT / Unlicense / Apache-2.0 projects are credited above. If any source file
  is vendored later (LATER phase), its licence and NOTICE must be added.
- Paperang / PeriPage material: studied only within licence-permitted scope;
  no implementation, table or binary was copied.

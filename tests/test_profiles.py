"""Consistency + licence-safety checks for V0.1 device profiles.

Usage: python tests/test_profiles.py <catalog_dump.exe>
"""
from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PROFILE_DIR = ROOT / "printers" / "profiles"
SRC_DIR = ROOT / "src"

REQUIRED_KEYS = {
    "model_id", "display_name", "protocol_family", "supported_transports",
    "bluetooth_name_patterns", "service_uuid", "write_uuid", "notify_uuid",
    "width_dots", "use_lattice", "mirror_rows", "energy", "quality", "speed",
    "feed_dots", "row_delay_ms", "write_chunk", "implemented",
}

FORBIDDEN_CLASSIC_INCLUDES = ("BluetoothSerial.h", "esp_spp_api.h", "esp_bt_main.h")


def load_profiles() -> dict:
    result = {}
    for path in sorted(PROFILE_DIR.glob("*.json")):
        data = json.loads(path.read_text(encoding="utf-8"))
        missing = REQUIRED_KEYS - set(data)
        assert not missing, f"{path.name}: missing keys {sorted(missing)}"
        assert data["supported_transports"], f"{path.name}: no transports"
        # An identification-only profile may legitimately have an unknown
        # print width; it must never be marked printable in that case.
        if data["width_dots"] == 0:
            assert not data["implemented"], \
                f"{path.name}: unknown width must not be printable"
        else:
            assert data["width_dots"] > 0, f"{path.name}: invalid width"
        if data["implemented"]:
            assert data["protocol_family"] == "tiny_iprint", \
                f"{path.name}: only tiny_iprint is implemented in V0.1"
            assert data["supported_transports"] == ["ble"], \
                f"{path.name}: implemented V0.1 profiles must be BLE-only"
        if "bluetooth_classic" in data["supported_transports"]:
            assert not data["implemented"], \
                f"{path.name}: Classic profile must stay unimplemented in V0.1"
        # The advertised UUID is NOT the GATT print service. Never let an
        # advertisement-only UUID leak into service_uuid.
        if "advertised_service_uuid" in data:
            assert data["advertised_service_uuid"] != data["service_uuid"], \
                f"{path.name}: advertised/GATT service UUID must be separated"
        if data["model_id"] == "x18_aijixing":
            assert data["service_uuid"] == "AE30", "X18 GATT print service is AE30"
            assert data["advertised_service_uuid"] == "AF30", \
                "X18 advertised service is AF30 (not the print service)"
            assert data["write_uuid"] == "AE01" and data["notify_uuid"] == "AE02"
            # 384 dots confirmed on real hardware (11 user-inspected sheets
            # plus one user-inspected power-cycle auto-recovery sheet).
            # implemented=true enables the automatic offline recovery path;
            # enabled only after the power-cycle recovery test passed.
            assert data["width_dots"] == 384, \
                "X18 confirmed print width is 384 dots"
            assert data["implemented"], \
                "X18 must be auto-discoverable for offline recovery"
        result[data["model_id"]] = data
    return result


def load_catalog_dump(executable: str) -> dict:
    # Force UTF-8: display_name may contain non-ASCII (e.g. X18 / 爱记星).
    output = subprocess.check_output([executable], text=True, encoding="utf-8")
    rows = json.loads(output)
    return {row["model_id"]: row for row in rows}


def check_no_classic_api_includes() -> int:
    hits = 0
    for path in SRC_DIR.rglob("*"):
        if path.suffix not in {".h", ".cpp", ".ino"}:
            continue
        for line in path.read_text(encoding="utf-8", errors="ignore").splitlines():
            stripped = line.strip()
            if not stripped.startswith("#include"):
                continue
            for header in FORBIDDEN_CLASSIC_INCLUDES:
                assert header not in stripped, \
                    f"Classic Bluetooth API referenced in {path}: {stripped}"
        hits += 1
    return hits


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: python tests/test_profiles.py <catalog_dump.exe>")
        return 2
    profiles = load_profiles()
    catalog = load_catalog_dump(sys.argv[1])
    assert set(profiles) == set(catalog), \
        f"profile drift: files={sorted(profiles)} firmware={sorted(catalog)}"
    for model, expected in profiles.items():
        assert expected == catalog[model], (
            f"{model}: JSON profile differs from compiled-in catalog\n"
            f" json     = {expected}\n firmware = {catalog[model]}"
        )
    files_scanned = check_no_classic_api_includes()
    print(f"PASS: {len(profiles)} profiles match firmware catalog; "
          f"no Classic Bluetooth #include in {files_scanned} source files")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

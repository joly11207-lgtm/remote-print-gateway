#!/usr/bin/env python3
"""Package built firmware for the in-browser installer (Website V1.2).

Turns PlatformIO output into the three things the site needs, with no manual
copying:

    web/firmware/releases/<version>/gateway-<board>.factory.bin
    web/firmware/releases/<version>/provenance.json
    web/firmware/manifest-<board>.json          (ESP Web Tools manifest)
    web/boards.js                                (board matrix + hashes)

The factory image is a *merged* bootloader + partition table + boot_app0 + app,
written at offset 0, which is what a blank board needs. A plain app-only
`firmware.bin` is an OTA image and must never be handed to a blank board.

Usage:
    python tools/build_web_firmware.py                  # verified boards only
    python tools/build_web_firmware.py --env s3_n16r8
    python tools/build_web_firmware.py --build-dir .pio/build --dry-run
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WEB = ROOT / "web"
FIRMWARE_DIR = WEB / "firmware"
BOARDS_JS = WEB / "boards.js"
VERSION_HEADER = ROOT / "src" / "config" / "RtbConfig.h"
ESPTOOL_RELATIVE = Path("packages") / "tool-esptoolpy" / "esptool.py"


@dataclass(frozen=True)
class Board:
    """One packaging target. Mirrors platformio.ini; `assert_matches_ini` keeps
    the two in sync so a flash_size change cannot silently ship a wrong image."""

    env: str
    slug: str                 # used in file names: gateway-<slug>.factory.bin
    label: str
    status: str               # "verified" | "experimental"
    chip_family: str          # ESP Web Tools chipFamily value
    esptool_chip: str         # esptool --chip value
    flash_size: str           # merge_bin --flash_size
    flash_mode: str
    flash_freq: str
    bootloader_offset: int
    flash_mb: int
    psram: str
    shipped: bool             # publish a binary + manifest for this board
    capabilities: tuple[str, ...] = ("wifi", "ble", "web_serial")
    bluetooth_classic: bool = False
    note: str = ""


# s3_n16r8 is the only board that has been flashed and driven on real hardware
# (gateway_info + BLE scan + X18 fingerprint). The other two compile and pass
# the host suites but have never been flashed or printed from, so they are
# published as experimental with no installer.
BOARDS: tuple[Board, ...] = (
    Board(
        env="s3_n16r8",
        slug="s3-n16r8",
        label="ESP32-S3 N16R8",
        status="verified",
        chip_family="ESP32-S3",
        esptool_chip="esp32s3",
        flash_size="16MB",
        flash_mode="qio",
        flash_freq="80m",
        bootloader_offset=0x0,
        flash_mb=16,
        psram="8MB OPI PSRAM",
        shipped=True,
        note="16MB flash, 8MB OPI PSRAM. Hardware verified with the real X18.",
    ),
    Board(
        env="s3_supermini",
        slug="s3-supermini",
        label="ESP32-S3 SuperMini",
        status="experimental",
        chip_family="ESP32-S3",
        esptool_chip="esp32s3",
        flash_size="8MB",
        flash_mode="qio",
        flash_freq="80m",
        bootloader_offset=0x0,
        flash_mb=8,
        psram="none",
        shipped=False,
        note="Compiles only. Not flashed, not scanned, not printed from.",
    ),
    Board(
        env="wroom32e_future",
        slug="wroom32e",
        label="ESP32-WROOM-32E",
        status="experimental",
        chip_family="ESP32",
        esptool_chip="esp32",
        flash_size="4MB",
        flash_mode="dio",
        flash_freq="40m",
        bootloader_offset=0x1000,
        flash_mb=4,
        psram="none",
        shipped=False,
        note="Compiles only. BLE is available; Bluetooth Classic is not implemented.",
    ),
)

PARTITIONS_OFFSET = 0x8000
BOOT_APP0_OFFSET = 0xE000
APP_OFFSET = 0x10000


def fail(message: str) -> None:
    print(f"FAIL: {message}", file=sys.stderr)
    raise SystemExit(1)


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def firmware_version() -> tuple[str, str]:
    """Returns (release version, exact version the device reports)."""
    text = VERSION_HEADER.read_text(encoding="utf-8")
    match = re.search(r'#define\s+RTB_VERSION\s+"([^"]+)"', text)
    if not match:
        fail("RTB_VERSION not found in src/config/RtbConfig.h")
    reported = match.group(1)
    release = re.match(r"\d+\.\d+\.\d+", reported)
    if not release:
        fail(f"RTB_VERSION {reported!r} has no x.y.z prefix")
    return release.group(0), reported


def git_commit() -> tuple[str, bool]:
    try:
        commit = subprocess.run(
            ["git", "rev-parse", "HEAD"], cwd=ROOT, capture_output=True, text=True, check=True
        ).stdout.strip()
        dirty = bool(
            subprocess.run(
                ["git", "status", "--porcelain"], cwd=ROOT, capture_output=True, text=True, check=True
            ).stdout.strip()
        )
        return commit, dirty
    except (subprocess.CalledProcessError, FileNotFoundError):
        return "unknown", True


def esptool_path() -> list[str] | None:
    """A command prefix that can run esptool, or None when it is unavailable."""
    core = os.environ.get("PLATFORMIO_CORE_DIR")
    if core:
        candidate = Path(core) / ESPTOOL_RELATIVE
        if candidate.is_file():
            return [sys.executable, str(candidate)]
    for base in (Path("C:/Users/Public/rtb-pio-core"), Path.home() / ".platformio"):
        candidate = base / ESPTOOL_RELATIVE
        if candidate.is_file():
            return [sys.executable, str(candidate)]
    if shutil.which("esptool.py"):
        return ["esptool.py"]
    return None


def resolve_build_dir(env: str, override: str | None) -> Path:
    if override:
        base = Path(override)
        base = base if base.is_absolute() else (ROOT / base)
        candidate = base / env if (base / env).is_dir() else base
        if not (candidate / "firmware.bin").is_file():
            fail(f"{candidate} has no firmware.bin")
        return candidate
    for candidate in (
        Path("C:/Users/Public/rtb-v01-build/.pio/build") / env,
        ROOT / ".pio" / "build" / env,
        ROOT / "build" / env,
    ):
        if (candidate / "firmware.bin").is_file():
            return candidate
    fail(f"no build output for {env}; run tools/build.ps1 first")


def assert_matches_ini(board: Board) -> None:
    """The shipped image must match what platformio.ini says the board is."""
    ini = (ROOT / "platformio.ini").read_text(encoding="utf-8")
    section = re.search(rf"\[env:{board.env}\](.*?)(?=\n\[|\Z)", ini, re.S)
    if not section:
        fail(f"[env:{board.env}] missing from platformio.ini")
    body = section.group(1)
    flash = re.search(r"board_build\.flash_size\s*=\s*(\S+)", body)
    if flash and flash.group(1).lower() != board.flash_size.lower():
        fail(f"{board.env}: platformio.ini flash_size {flash.group(1)} != {board.flash_size}")
    if board.env == "s3_n16r8" and "-DBOARD_HAS_PSRAM" not in body:
        fail("s3_n16r8 lost -DBOARD_HAS_PSRAM; the packaged image would differ from the tested one")


def framework_roots() -> list[Path]:
    roots = []
    core = os.environ.get("PLATFORMIO_CORE_DIR")
    if core:
        roots.append(Path(core))
    roots.extend([Path("C:/Users/Public/rtb-pio-core"), Path.home() / ".platformio"])
    return roots


def resolve_boot_app0(build_dir: Path) -> Path:
    """boot_app0.bin ships with the framework, not with our build output."""
    local = build_dir / "boot_app0.bin"
    if local.is_file():
        return local
    for root in framework_roots():
        candidate = (
            root / "packages" / "framework-arduinoespressif32" / "tools" / "partitions" / "boot_app0.bin"
        )
        if candidate.is_file():
            return candidate
    fail("boot_app0.bin not found in the build dir or the Arduino framework package")


def part_files(build_dir: Path, board: Board) -> list[tuple[int, Path]]:
    parts = [
        (board.bootloader_offset, build_dir / "bootloader.bin"),
        (PARTITIONS_OFFSET, build_dir / "partitions.bin"),
        (BOOT_APP0_OFFSET, resolve_boot_app0(build_dir)),
        (APP_OFFSET, build_dir / "firmware.bin"),
    ]
    missing = [str(path.name) for _, path in parts if not path.is_file()]
    if missing:
        fail(f"{build_dir} is missing {missing}; a factory image needs all four parts")
    return parts


def merge_factory(board: Board, build_dir: Path, out_file: Path, esptool: list[str]) -> None:
    out_file.parent.mkdir(parents=True, exist_ok=True)
    if out_file.exists():
        out_file.unlink()
    command = [
        *esptool,
        "--chip",
        board.esptool_chip,
        "merge_bin",
        "-o",
        str(out_file),
        "--flash_mode",
        board.flash_mode,
        "--flash_freq",
        board.flash_freq,
        "--flash_size",
        board.flash_size,
    ]
    for offset, path in part_files(build_dir, board):
        command.extend([hex(offset), str(path)])
    print(f"  merge: {' '.join(command[-6:])}")
    completed = subprocess.run(command, capture_output=True, text=True)
    if completed.returncode != 0 or not out_file.is_file():
        fail(f"esptool merge_bin failed:\n{completed.stdout}\n{completed.stderr}")


def build_board(board: Board, version: str, reported: str, esptool: list[str],
                build_dir_override: str | None, dry_run: bool) -> dict:
    assert_matches_ini(board)
    build_dir = resolve_build_dir(board.env, build_dir_override)
    release_dir = FIRMWARE_DIR / "releases" / version
    factory = release_dir / f"gateway-{board.slug}.factory.bin"
    manifest = FIRMWARE_DIR / f"manifest-{board.slug}.json"

    commit, dirty = git_commit()

    if dry_run:
        print(f"  [dry-run] would merge {build_dir} -> {factory}")
    else:
        merge_factory(board, build_dir, factory, esptool)

    digest = sha256_of(factory) if factory.is_file() else "0" * 64
    size = factory.stat().st_size if factory.is_file() else 0

    parts = []
    for offset, path in part_files(build_dir, board):
        parts.append(
            {
                "name": path.name,
                "offset": hex(offset),
                "size": path.stat().st_size,
                "sha256": sha256_of(path),
            }
        )

    provenance = {
        "board": board.slug,
        "env": board.env,
        "hardware_target": board.chip_family,
        "firmware_version": version,
        "firmware_version_reported": reported,
        "git_commit": commit,
        "git_dirty": dirty,
        "build_time": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "factory": {
            "path": factory.relative_to(FIRMWARE_DIR).as_posix(),
            "size": size,
            "sha256": digest,
            "flash": {
                "mode": board.flash_mode,
                "freq": board.flash_freq,
                "size": board.flash_size,
            },
        },
        "parts": parts,
        "note": "Merged factory image (bootloader + partitions + boot_app0 + app) at offset 0.",
    }
    if not dry_run:
        released = FIRMWARE_DIR / "releases" / version / "provenance.json"
        existing = []
        if released.is_file():
            try:
                existing = json.loads(released.read_text(encoding="utf-8"))
            except json.JSONDecodeError:
                existing = []
        entries = [entry for entry in existing if entry.get("board") != board.slug]
        entries.append(provenance)
        entries.sort(key=lambda entry: entry.get("board", ""))
        released.write_text(json.dumps(entries, indent=2) + "\n", encoding="utf-8")

    manifest_payload = {
        "name": f"Remote Print Gateway — {board.label}",
        "version": version,
        "new_install_prompt_erase": True,
        "builds": [
            {
                "chipFamily": board.chip_family,
                "parts": [
                    {
                        "path": factory.relative_to(FIRMWARE_DIR).as_posix(),
                        "offset": 0,
                    }
                ],
            }
        ],
    }
    if not dry_run:
        manifest.write_text(json.dumps(manifest_payload, indent=2) + "\n", encoding="utf-8")

    print(f"  {board.env}: factory {size} bytes, sha256 {digest[:16]}…")
    return {
        "provenance": provenance,
        "manifest": manifest_payload,
        "factory": factory,
        "digest": digest,
        "size": size,
    }


def board_entry(board: Board, packaged: dict | None, version: str) -> dict:
    entry = {
        "id": board.env,
        "label": board.label,
        "status": board.status,
        "chip_family": board.chip_family,
        "flash_mb": board.flash_mb,
        "psram": board.psram,
        "capabilities": list(board.capabilities),
        "bluetooth_classic": board.bluetooth_classic,
        "note": board.note,
        "installer": None,
        "download": None,
        "sha256": None,
        "size": None,
        "version": version if packaged else None,
    }
    if packaged:
        entry["installer"] = f"firmware/manifest-{board.slug}.json"
        entry["download"] = f"firmware/releases/{version}/gateway-{board.slug}.factory.bin"
        entry["sha256"] = packaged["digest"]
        entry["size"] = packaged["size"]
    return entry


def write_boards_js(entries: list[dict], meta: dict) -> None:
    payload = json.dumps(entries, indent=2)
    BOARDS_JS.write_text(
        "// Generated by tools/build_web_firmware.py - do not edit by hand.\n"
        "// Regenerate after every firmware build, then commit both this file and\n"
        "// the binaries it points at.\n"
        f"window.RTB_FIRMWARE = {json.dumps(meta, indent=2)};\n"
        f"window.RTB_BOARDS = {payload};\n",
        encoding="utf-8",
    )
    print(f"  wrote {BOARDS_JS.relative_to(ROOT)} ({len(entries)} boards)")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--env", action="append", help="package only this PlatformIO env")
    parser.add_argument("--build-dir", help="PlatformIO build root (default: auto-detect)")
    parser.add_argument("--dry-run", action="store_true", help="do not write any artefact")
    args = parser.parse_args()

    version, reported = firmware_version()
    commit, dirty = git_commit()
    esptool = esptool_path()
    if not esptool:
        fail("esptool not found; install PlatformIO or set PLATFORMIO_CORE_DIR")
    if dirty:
        print("NOTE: working tree is dirty; provenance records the exact commit anyway")

    print(f"=== packaging firmware {version} (device reports {reported}) ===")
    selected = [b for b in BOARDS if b.env in args.env] if args.env else list(BOARDS)
    if not selected:
        fail(f"no such env: {args.env}")

    packaged: dict[str, dict | None] = {board.env: None for board in selected}
    for board in selected:
        if not board.shipped:
            print(f"  {board.env}: {board.status}, not packaged (no binary is published)")
            continue
        packaged[board.env] = build_board(
            board, version, reported, esptool, args.build_dir, args.dry_run
        )

    entries = [board_entry(board, packaged.get(board.env), version) for board in selected]
    # Boards that were not selected this run keep their previous entry so a
    # single-env package never silently drops the rest of the matrix.
    if args.env and BOARDS_JS.is_file():
        try:
            existing = json.loads(
                re.search(
                    r"window\.RTB_BOARDS = (\[.*?\]);", BOARDS_JS.read_text(encoding="utf-8"), re.S
                ).group(1)
            )
        except (AttributeError, json.JSONDecodeError):
            existing = []
        keep = [entry for entry in existing if entry.get("id") not in packaged]
        entries = sorted(entries + keep, key=lambda entry: (entry["status"] != "verified", entry["id"]))

    meta = {
        "version": version,
        "firmware_version_reported": reported,
        "git_commit": commit,
        "git_dirty": dirty,
        "generated_at": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "installer": "esp-web-tools 10.5.2 (vendored under web/vendor/esp-web-tools)",
    }
    if args.dry_run:
        print(json.dumps(entries, indent=2))
    else:
        write_boards_js(entries, meta)

    print("=== done ===")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

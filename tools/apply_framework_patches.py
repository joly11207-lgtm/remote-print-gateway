#!/usr/bin/env python3
"""Apply the project's Arduino-ESP32 framework patches, idempotently.

Why this exists
---------------
The WebServer raw-stream fixes live in the PlatformIO framework package
(``packages/framework-arduinoespressif32``), which is NOT part of this repo and
is lost whenever PlatformIO re-installs packages or the project moves to another
machine. Editing it by hand is therefore not reproducible. This script makes the
edits a checked-in asset: ``patches/*.patch`` plus this applier, run before every
build.

Contract
--------
* Only runs against Arduino-ESP32 2.0.17. Any other version aborts the build.
* Only patches sources whose baseline still matches the patch context. If the
  framework was modified by someone else, the build fails instead of guessing.
* Already patched -> reports "already patched" and exits 0 (idempotent).
* Half-applied (one hunk present, another missing) -> hard failure.

Usage
-----
    python tools/apply_framework_patches.py                 # apply if needed
    python tools/apply_framework_patches.py --check         # verify only
    python tools/apply_framework_patches.py --core-dir C:\\Users\\Public\\rtb-pio-core

The core dir defaults to $PLATFORMIO_CORE_DIR, then ~/.platformio.
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from pathlib import Path

EXPECTED_VERSION = (2, 0, 17)
EXPECTED_VERSION_TEXT = "2.0.17"
FRAMEWORK_PACKAGE = "framework-arduinoespressif32"
VERSION_HEADER = Path("cores") / "esp32" / "esp_arduino_version.h"


def die(message: str) -> "None":
    print("[patches] ERROR: " + message, file=sys.stderr)
    sys.exit(1)


def resolve_core_dir(cli_value: str | None, project_root: Path) -> Path:
    if cli_value:
        return Path(cli_value).expanduser()
    env = os.environ.get("PLATFORMIO_CORE_DIR")
    if env:
        return Path(env).expanduser()
    default = Path.home() / ".platformio"
    return default


def read_framework_version(framework: Path) -> tuple[int, int, int] | None:
    header = framework / VERSION_HEADER
    if not header.is_file():
        return None
    text = header.read_bytes().decode("utf-8", errors="replace")
    values = []
    for part in ("MAJOR", "MINOR", "PATCH"):
        match = re.search(r"#define\s+ESP_ARDUINO_VERSION_" + part + r"\s+(\d+)", text)
        if match is None:
            return None
        values.append(int(match.group(1)))
    return (values[0], values[1], values[2])


def parse_patch(patch_file: Path) -> list[dict]:
    """Parse a unified diff into [{path, hunks:[{old_start, lines}]}]."""
    entries: list[dict] = []
    current: dict | None = None
    hunk: dict | None = None

    for raw in patch_file.read_bytes().decode("utf-8").splitlines():
        if raw.startswith("--- "):
            continue
        if raw.startswith("+++ "):
            target = raw[4:].strip()
            if target.startswith("b/"):
                target = target[2:]
            current = {"path": target, "hunks": []}
            entries.append(current)
            hunk = None
            continue
        if raw.startswith("@@"):
            match = re.match(r"@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@", raw)
            if match is None:
                die(f"{patch_file.name}: malformed hunk header: {raw!r}")
            if current is None:
                die(f"{patch_file.name}: hunk before +++ header")
            hunk = {"old_start": int(match.group(1)), "lines": []}
            current["hunks"].append(hunk)
            continue
        if hunk is None:
            continue
        if raw.startswith("\\"):  # "\ No newline at end of file"
            continue
        if not raw[:1] in (" ", "-", "+"):
            die(f"{patch_file.name}: unexpected diff line: {raw!r}")
        hunk["lines"].append(raw)

    if not entries:
        die(f"{patch_file.name}: no file entries found")
    for entry in entries:
        if not entry["hunks"]:
            die(f"{patch_file.name}: no hunks for {entry['path']}")
    return entries


def hunk_pattern(hunk: dict) -> list[str]:
    return [line[1:] for line in hunk["lines"] if line[:1] in (" ", "-")]


def hunk_replacement(hunk: dict) -> list[str]:
    return [line[1:] for line in hunk["lines"] if line[:1] in (" ", "+")]


def find_match(lines: list[str], pattern: list[str], hint: int) -> int | None:
    count = len(lines)
    size = len(pattern)
    if size == 0 or count < size:
        return None

    def matches_at(index: int) -> bool:
        return all(lines[index + offset] == expected for offset, expected in enumerate(pattern))

    start = max(0, hint)
    stop = min(count - size, start + 50)
    for index in range(start, stop + 1):
        if matches_at(index):
            return index
    for index in range(0, count - size + 1):
        if matches_at(index):
            return index
    return None


def contains_sequence(lines: list[str], pattern: list[str]) -> bool:
    size = len(pattern)
    if size == 0:
        return False
    return any(
        all(lines[index + offset] == expected for offset, expected in enumerate(pattern))
        for index in range(0, len(lines) - size + 1)
    )


def classify(lines: list[str], hunks: list[dict]) -> str:
    """Return 'applied', 'pending', 'partial' or 'mismatch'.

    Matching is done on the whole hunk (context + changed lines), never on the
    changed lines alone: HTTPUpload and HTTPRaw both contain
    ``uint8_t buf[HTTP_UPLOAD_BUFLEN];``, so a bare line test is ambiguous.
    """
    states: list[str] = []
    for hunk in hunks:
        before = hunk_pattern(hunk)       # context + removed  -> pristine source
        after = hunk_replacement(hunk)    # context + added    -> patched source
        has_before = contains_sequence(lines, before)
        has_after = contains_sequence(lines, after)
        if has_after and not has_before:
            states.append("after")
        elif has_before and not has_after:
            states.append("before")
        else:
            states.append("unknown")
    if all(state == "after" for state in states):
        return "applied"
    if all(state == "before" for state in states):
        return "pending"
    if "unknown" in states:
        return "mismatch"
    return "partial"


def apply_entry(framework: Path, entry: dict) -> str:
    target = framework / entry["path"]
    if not target.is_file():
        die(f"missing framework source: {target}")

    text = target.read_bytes().decode("utf-8")
    lines = text.split("\n")

    state = classify(lines, entry["hunks"])
    if state == "applied":
        return "already patched"
    if state == "partial":
        die(
            f"{entry['path']}: partially patched. Restore the pristine file "
            f"(or reinstall the framework package) and run again."
        )
    if state == "mismatch":
        die(
            f"{entry['path']}: baseline does not match {EXPECTED_VERSION_TEXT}. "
            f"The framework sources were modified by something other than this "
            f"patch set; refusing to touch them."
        )

    offset = 0
    for hunk in entry["hunks"]:
        pattern = hunk_pattern(hunk)
        replacement = hunk_replacement(hunk)
        hint = hunk["old_start"] - 1 + offset
        index = find_match(lines, pattern, hint)
        if index is None:
            die(
                f"{entry['path']}: baseline does not match {EXPECTED_VERSION_TEXT} "
                f"at hunk @@ -{hunk['old_start']}. Refusing to patch a modified file."
            )
        lines[index:index + len(pattern)] = replacement
        offset += len(replacement) - len(pattern)

    target.write_bytes("\n".join(lines).encode("utf-8"))
    return "applied"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--core-dir", default=None, help="PlatformIO core dir (default: $PLATFORMIO_CORE_DIR or ~/.platformio)")
    parser.add_argument("--check", action="store_true", help="verify only, never write")
    args = parser.parse_args()

    project_root = Path(__file__).resolve().parent.parent
    patch_dir = project_root / "patches"
    if not patch_dir.is_dir():
        die(f"patch directory not found: {patch_dir}")

    patch_files = sorted(patch_dir.glob("*.patch"))
    if not patch_files:
        die(f"no *.patch files in {patch_dir}")

    core_dir = resolve_core_dir(args.core_dir, project_root)
    framework = core_dir / "packages" / FRAMEWORK_PACKAGE
    if not framework.is_dir():
        die(
            f"framework package not found: {framework}\n"
            f"         Run a build once so PlatformIO installs it, or pass --core-dir."
        )

    version = read_framework_version(framework)
    expected = ".".join(str(part) for part in EXPECTED_VERSION)
    if version is None:
        die(f"cannot read ESP_ARDUINO_VERSION_* from {framework / VERSION_HEADER}")
    actual = ".".join(str(part) for part in version)
    if version != EXPECTED_VERSION:
        die(
            f"Arduino-ESP32 version is {actual}, this patch set targets {expected}.\n"
            f"         Do NOT upgrade silently: re-derive patches/*.patch for the new version."
        )
    print(f"[patches] framework: {framework}")
    print(f"[patches] version:   {actual} (expected {expected})")

    for patch_file in patch_files:
        if EXPECTED_VERSION_TEXT not in patch_file.name:
            die(f"{patch_file.name}: file name does not declare version {EXPECTED_VERSION_TEXT}")
        for entry in parse_patch(patch_file):
            if args.check:
                target = framework / entry["path"]
                if not target.is_file():
                    die(f"missing framework source: {target}")
                state = classify(target.read_bytes().decode("utf-8").split("\n"), entry["hunks"])
                print(f"[patches] {entry['path']}: {'patched' if state == 'applied' else state}")
                if state != "applied":
                    die(f"{entry['path']} is not patched (--check)")
                continue
            print(f"[patches] {entry['path']}: {apply_entry(framework, entry)}")

    print("[patches] OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())

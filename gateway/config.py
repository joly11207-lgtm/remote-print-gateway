"""Paths and constants shared by the gateway application."""

from __future__ import annotations

import os
from pathlib import Path

APP_NAME = "Remote Print Gateway"
APP_VERSION = "0.2.0"

ROOT = Path(__file__).resolve().parent.parent
# Shared with the ESP32 firmware: these profiles are the compiled-in catalog.
PROFILE_DIR = ROOT / "printers" / "profiles"
# Gateway-only profiles (e.g. a printer the firmware cannot print yet). They are
# deliberately kept out of printers/profiles so the firmware catalog test cannot
# mistake them for implemented support.
GATEWAY_PROFILE_DIR = Path(__file__).resolve().parent / "profiles"
WEB_DIR = ROOT / "web"
SCHEMA_PATH = Path(__file__).resolve().parent / "schema.sql"

# Overridable so the tests can point at a throw-away file.
DB_PATH = Path(
    os.environ.get("RTB_GATEWAY_DB", str(ROOT / "gateway" / "data" / "gateway.db"))
)

DEFAULT_USER_ID = "user-demo"
DEFAULT_GATEWAY_ID = "gw-sim-001"

# A similarity match below this is considered "not confident enough" and the
# case is escalated to Stage C (AI analyst).
SIMILARITY_THRESHOLD = 0.72
# A similarity match at or above this is accepted without the AI stage.
SIMILARITY_AUTO_ACCEPT = 0.90
# An AI answer below this confidence never reaches a safe test: the case stops
# for manual review instead of guessing.
AI_MIN_CONFIDENCE = 0.60

# HTTP API (V0.1: dev server only, no authentication yet).
HTTP_HOST = os.environ.get("RTB_HTTP_HOST", "127.0.0.1")
try:
    HTTP_PORT = int(os.environ.get("RTB_HTTP_PORT", "8080"))
except ValueError:
    HTTP_PORT = 8080

# Simulated firmware (test fixture) may only be consulted when this is on. The
# production path must never resolve a simulated release.
ALLOW_SIMULATED_FIRMWARE = os.environ.get(
    "RTB_ALLOW_SIMULATED_FIRMWARE", ""
).strip().lower() in {"1", "true", "yes", "on"}

# ---- real gateway (V0.2) ----------------------------------------------------
# The ESP32's LAN address lives HERE and only here: the browser never talks to
# the printer or to the gateway URL directly (no Web Bluetooth, no hard-coded
# IP in app.js). Pointing the deployment at a cloud relay later means setting
# RTB_REAL_GATEWAY_URL to that relay, not rewriting the UI.
REAL_GATEWAY_URL = os.environ.get("RTB_REAL_GATEWAY_URL", "").strip().rstrip("/")
REAL_GATEWAY_ID = os.environ.get("RTB_REAL_GATEWAY_ID", "gw-x18-001").strip()
REAL_GATEWAY_NAME = os.environ.get(
    "RTB_REAL_GATEWAY_NAME", "Home Gateway (ESP32-WROOM-32E)"
).strip()
REAL_HARDWARE_FAMILY = os.environ.get(
    "RTB_REAL_HARDWARE_FAMILY", "ESP32-WROOM-32E"
).strip()
# How long one HTTP call to the gateway may take, and how long the gateway
# should listen for BLE advertisements during one scan.
try:
    GATEWAY_HTTP_TIMEOUT = float(os.environ.get("RTB_GATEWAY_HTTP_TIMEOUT", "8"))
except ValueError:
    GATEWAY_HTTP_TIMEOUT = 8.0
try:
    GATEWAY_SCAN_DURATION_MS = int(os.environ.get("RTB_GATEWAY_SCAN_DURATION_MS", "4000"))
except ValueError:
    GATEWAY_SCAN_DURATION_MS = 4000


def real_gateway_configured() -> bool:
    return bool(REAL_GATEWAY_URL)

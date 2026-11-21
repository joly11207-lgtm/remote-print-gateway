"""SQLite access + first-run seeding.

The printer profiles are seeded from ``printers/profiles/*.json`` so the web
side and the ESP32 firmware keep a single source of truth for the six known
models.
"""

from __future__ import annotations

import json
import sqlite3
from datetime import datetime, timezone
from typing import Any, Iterable

from . import config
from .canonical import normalize_protocol, normalize_transport

# Brand / model are presentation data; the JSON profiles only carry a
# display_name and a model_id.
BRAND_MODEL = {
    "x18_aijixing": ("爱记星", "X18"),
    "gb01": ("GB", "GB01"),
    "gb02": ("GB", "GB02"),
    "mx06": ("MX", "MX06"),
    "sc03h": ("iPrint", "SC03h"),
    "escpos_spp_future": ("Generic", "ESC/POS SPP"),
    "memobird_g2d": ("MEMOBIRD", "G2D"),
}

# Directory label -> path. Gateway-only profiles never reach the firmware
# catalog, so "detected but not printable" stays visible instead of silently
# becoming firmware support.
PROFILE_SOURCES = (
    ("printers/profiles", config.PROFILE_DIR),
    ("gateway/profiles", config.GATEWAY_PROFILE_DIR),
)

GATEWAY = {
    "gateway_id": config.DEFAULT_GATEWAY_ID,
    "user_id": config.DEFAULT_USER_ID,
    "name": "Home Gateway (Simulator)",
    "hardware_family": "GW-ESP32-V1",
    "firmware_version": "1.2.3",
    "status": "online",
    "mode": "simulator",
    "base_url": None,
}

# The real ESP32 is registered only when RTB_REAL_GATEWAY_URL is set. Its
# firmware_version starts as "unknown" and is replaced by the live /api/status
# answer on every snapshot - the DB row is identity, not telemetry.
# The x18_aijixing profile IS declared here because the firmware ships it.
REAL_GATEWAY_CAPABILITIES = [
    ("transport", "ble", 1),
    ("protocol", "tiny_iprint", 1),
    ("engine", "profile_engine", 1),
    ("profile", "x18_aijixing", 1),
]


def real_gateway_row() -> dict[str, Any] | None:
    """The configured ESP32, or None when the deployment has no real gateway."""
    if not config.real_gateway_configured():
        return None
    return {
        "gateway_id": config.REAL_GATEWAY_ID,
        "user_id": config.DEFAULT_USER_ID,
        "name": config.REAL_GATEWAY_NAME,
        "hardware_family": config.REAL_HARDWARE_FAMILY,
        "firmware_version": "unknown",
        "status": "offline",
        "mode": "real",
        "base_url": config.REAL_GATEWAY_URL,
    }

# Deliberately no "profile" capability for x18_aijixing: that is what makes the
# demo exercise "Profile first" (profile update, no firmware update).
# Every capability name is a canonical identifier (see gateway/canonical.py).
GATEWAY_CAPABILITIES = [
    ("transport", "ble", 1),
    ("protocol", "tiny_iprint", 1),
    ("engine", "profile_engine", 1),
]

FIRMWARE_RELEASES = [
    (
        "1.2.3",
        "GW-ESP32-V1",
        "stable",
        {"transports": ["ble"], "protocols": ["tiny_iprint"]},
        "available",
        "2026-09-01T00:00:00+00:00",
    ),
    (
        "1.3.0-beta.1",
        "GW-ESP32-V1",
        "beta",
        {
            "transports": ["ble", "bluetooth_classic"],
            "protocols": ["tiny_iprint", "escpos"],
        },
        "available",
        "2026-10-15T00:00:00+00:00",
    ),
    (
        "1.4.0-internal.1",
        "GW-ESP32-V1",
        "internal",
        {
            "transports": ["ble", "bluetooth_classic", "tcp", "ipp"],
            "protocols": ["tiny_iprint", "escpos", "ipp"],
        },
        "available",
        "2026-11-01T00:00:00+00:00",
    ),
]


def now_iso() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def connect() -> sqlite3.Connection:
    config.DB_PATH.parent.mkdir(parents=True, exist_ok=True)
    conn = sqlite3.connect(config.DB_PATH)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA foreign_keys = ON")
    return conn


def _load_profiles() -> Iterable[dict[str, Any]]:
    for label, directory in PROFILE_SOURCES:
        for path in sorted(directory.glob("*.json")):
            data = json.loads(path.read_text(encoding="utf-8"))
            model_id = data["model_id"]
            brand, model = BRAND_MODEL.get(model_id, ("Unknown", model_id))
            transports = [
                normalize_transport(value)
                for value in (data.get("supported_transports") or ["ble"])
            ]
            transport = transports[0] if transports else "unknown"
            protocol = normalize_protocol(data.get("protocol_family"))
            parameters = {
                key: data.get(key)
                for key in (
                    "width_dots",
                    "use_lattice",
                    "mirror_rows",
                    "energy",
                    "quality",
                    "speed",
                    "feed_dots",
                    "row_delay_ms",
                    "write_chunk",
                )
            }
            implemented = bool(data.get("implemented"))
            parameters["implemented"] = implemented
            # A profile that has never printed on hardware can never be
            # VERIFIED/SUPPORTED, whatever its JSON claims.
            declared_status = str(data.get("status") or "").upper()
            if implemented:
                status = "VERIFIED"
            else:
                status = (
                    declared_status
                    if declared_status in {"AI_CANDIDATE", "EXPERIMENTAL"}
                    else "EXPERIMENTAL"
                )
            rules = {
                "transport": transport,
                "service_uuid": data.get("service_uuid") or "",
                "advertised_service_uuid": data.get("advertised_service_uuid") or "",
                "write_uuid": data.get("write_uuid") or "",
                "notify_uuid": data.get("notify_uuid") or "",
                "spp_uuid": data.get("spp_uuid") or "",
                "bluetooth_name_patterns": data.get("bluetooth_name_patterns") or [],
            }
            yield {
                "profile_id": model_id,
                "brand": brand,
                "model": model,
                "display_name": data.get("display_name") or model_id,
                "transport": transport,
                "protocol": protocol,
                "parameters_json": json.dumps(parameters, ensure_ascii=False),
                "fingerprint_rules_json": json.dumps(rules, ensure_ascii=False),
                "version": 1,
                "status": status,
                "source": f"{label}/{path.name}",
            }


# Columns added after V0.1. ALTER TABLE keeps existing dev databases usable.
GATEWAY_COLUMN_MIGRATIONS = (
    ("mode", "TEXT NOT NULL DEFAULT 'simulator'"),
    ("base_url", "TEXT"),
)


def migrate(conn: sqlite3.Connection) -> None:
    """Add post-V0.1 columns to databases created by an older schema."""
    existing = {row["name"] for row in conn.execute("PRAGMA table_info(gateways)")}
    for name, declaration in GATEWAY_COLUMN_MIGRATIONS:
        if name not in existing:
            conn.execute(f"ALTER TABLE gateways ADD COLUMN {name} {declaration}")
    conn.commit()


def _seed_gateway(conn: sqlite3.Connection, row: dict[str, Any]) -> None:
    conn.execute(
        "INSERT INTO gateways (gateway_id, user_id, name, hardware_family,"
        " firmware_version, status, last_seen, created_at, mode, base_url)"
        " VALUES (?,?,?,?,?,?,?,?,?,?)"
        " ON CONFLICT(gateway_id) DO UPDATE SET"
        " name = excluded.name, hardware_family = excluded.hardware_family,"
        " mode = excluded.mode, base_url = excluded.base_url",
        (
            row["gateway_id"],
            row["user_id"],
            row["name"],
            row["hardware_family"],
            row["firmware_version"],
            row["status"],
            now_iso(),
            now_iso(),
            row["mode"],
            row["base_url"],
        ),
    )


def seed(conn: sqlite3.Connection) -> None:
    _seed_gateway(conn, GATEWAY)
    for cap_type, cap_name, version in GATEWAY_CAPABILITIES:
        conn.execute(
            "INSERT OR IGNORE INTO gateway_capabilities"
            " (gateway_id, capability_type, capability_name, version) VALUES (?,?,?,?)",
            (GATEWAY["gateway_id"], cap_type, cap_name, version),
        )

    real = real_gateway_row()
    if real is not None:
        _seed_gateway(conn, real)
        for cap_type, cap_name, version in REAL_GATEWAY_CAPABILITIES:
            conn.execute(
                "INSERT OR IGNORE INTO gateway_capabilities"
                " (gateway_id, capability_type, capability_name, version) VALUES (?,?,?,?)",
                (real["gateway_id"], cap_type, cap_name, version),
            )

    for profile in _load_profiles():
        conn.execute(
            "INSERT OR IGNORE INTO printer_profiles (profile_id, brand, model,"
            " display_name, transport, protocol, parameters_json,"
            " fingerprint_rules_json, version, status, source)"
            " VALUES (:profile_id,:brand,:model,:display_name,:transport,:protocol,"
            ":parameters_json,:fingerprint_rules_json,:version,:status,:source)",
            profile,
        )

    for version, family, channel, caps, status, released in FIRMWARE_RELEASES:
        conn.execute(
            "INSERT OR IGNORE INTO firmware_releases (firmware_version,"
            " hardware_family, channel, capabilities_json, status, released_at)"
            " VALUES (?,?,?,?,?,?)",
            (
                version,
                family,
                channel,
                json.dumps(caps, ensure_ascii=False),
                status,
                released,
            ),
        )
    conn.commit()


def initialize() -> None:
    schema = config.SCHEMA_PATH.read_text(encoding="utf-8")
    with connect() as conn:
        conn.executescript(schema)
        migrate(conn)
        seed(conn)

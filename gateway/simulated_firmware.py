"""Simulated future firmware, for demos and tests only.

Answers "what if a firmware really implemented Classic SPP + Memobird?" without
claiming that such firmware exists. Rows are written with ``simulated = 1`` and
are invisible to ``capability.resolve`` unless the caller explicitly passes
``include_simulated=True``. Nothing here is added to the shipped catalog in
``db.FIRMWARE_RELEASES``.
"""

from __future__ import annotations

import json
import sqlite3

from .canonical import is_canonical_protocol, is_canonical_transport

SIMULATED_RELEASE: dict[str, object] = {
    "firmware_version": "1.3.0-beta.1",
    "hardware_family": "GW-ESP32-V1",
    "channel": "beta",
    "capabilities": {
        "transports": ["bluetooth_classic"],
        "protocols": ["memobird"],
    },
    "released_at": "2026-10-15T00:00:00+00:00",
}


def install(conn: sqlite3.Connection) -> dict[str, object]:
    """Insert the simulated release. Idempotent."""
    caps = SIMULATED_RELEASE["capabilities"]
    assert isinstance(caps, dict)
    for transport in caps["transports"]:
        if not is_canonical_transport(transport):
            raise ValueError(f"non-canonical simulated transport: {transport}")
    for protocol in caps["protocols"]:
        if not is_canonical_protocol(protocol):
            raise ValueError(f"non-canonical simulated protocol: {protocol}")

    conn.execute(
        "INSERT OR IGNORE INTO firmware_releases (firmware_version, hardware_family,"
        " channel, capabilities_json, status, simulated, released_at)"
        " VALUES (?,?,?,?,?,?,?)",
        (
            SIMULATED_RELEASE["firmware_version"],
            SIMULATED_RELEASE["hardware_family"],
            SIMULATED_RELEASE["channel"],
            json.dumps(caps, ensure_ascii=False),
            "available",
            1,
            SIMULATED_RELEASE["released_at"],
        ),
    )
    conn.commit()
    return dict(SIMULATED_RELEASE)


def uninstall(conn: sqlite3.Connection) -> int:
    cursor = conn.execute("DELETE FROM firmware_releases WHERE simulated = 1")
    conn.commit()
    return cursor.rowcount


def installed(conn: sqlite3.Connection) -> bool:
    row = conn.execute(
        "SELECT 1 FROM firmware_releases WHERE simulated = 1 LIMIT 1"
    ).fetchone()
    return row is not None


# The capabilities the gateway would report only after actually running the
# (not yet written, not yet verified) Classic SPP + Memobird firmware.
GATEWAY_UPGRADE = (
    ("transport", "bluetooth_classic"),
    ("protocol", "memobird"),
)


def emulate_gateway_upgrade(conn: sqlite3.Connection, gateway_id: str) -> dict[str, object]:
    """Pretend the gateway finished updating. Fixture tooling, never product logic.

    This writes gateway capability rows only; it does not claim any firmware
    release supports Memobird, and it does not execute anything against a printer.
    """
    added: list[str] = []
    for capability_type, name in GATEWAY_UPGRADE:
        cursor = conn.execute(
            "INSERT OR IGNORE INTO gateway_capabilities (gateway_id, capability_type,"
            " capability_name, version) VALUES (?,?,?,?)",
            (gateway_id, capability_type, name, 1),
        )
        if cursor.rowcount:
            added.append(f"{capability_type}:{name}")
    conn.commit()
    return {
        "gateway_id": gateway_id,
        "simulated": True,
        "added": added,
        "capabilities": [f"{kind}:{name}" for kind, name in GATEWAY_UPGRADE],
    }

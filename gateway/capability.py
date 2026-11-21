"""Capability Resolver: profile update vs firmware update.

The rule the product is built on is "Profile first": a new printer that only
needs different parameters is a profile deployment, never a firmware release.
Firmware is only required when the gateway genuinely lacks the transport or the
protocol.
"""

from __future__ import annotations

import json
import sqlite3
from dataclasses import dataclass, field
from typing import Any

from .canonical import normalize_protocol, normalize_transport

PATH_PROFILE_ONLY = "PROFILE_ONLY"
PATH_FIRMWARE = "FIRMWARE_AND_PROFILE"
PATH_BLOCKED = "BLOCKED"


@dataclass
class CapabilityDecision:
    transport: str
    protocol: str
    profile_id: str | None
    gateway_firmware_version: str
    hardware_family: str
    gateway_supports_transport: bool = False
    gateway_supports_protocol: bool = False
    gateway_has_profile: bool = False
    requires_firmware_update: bool = False
    requires_profile_update: bool = False
    required_capabilities: list[str] = field(default_factory=list)
    minimum_firmware_version: str | None = None
    minimum_firmware_channel: str | None = None
    # True when a firmware release exists that closes the gap (or when there is
    # no gap at all). False means the printer is understood but no shipped
    # firmware can talk to it yet.
    firmware_available: bool = True
    minimum_firmware_simulated: bool = False
    path: str = PATH_PROFILE_ONLY
    reason_codes: list[str] = field(default_factory=list)

    def as_dict(self) -> dict[str, Any]:
        return {
            "transport": self.transport,
            "protocol": self.protocol,
            "profile_id": self.profile_id,
            "gateway_firmware_version": self.gateway_firmware_version,
            "hardware_family": self.hardware_family,
            "gateway_supports_transport": self.gateway_supports_transport,
            "gateway_supports_protocol": self.gateway_supports_protocol,
            "gateway_has_profile": self.gateway_has_profile,
            "requires_firmware_update": self.requires_firmware_update,
            "requires_profile_update": self.requires_profile_update,
            "required_capabilities": list(self.required_capabilities),
            "minimum_firmware_version": self.minimum_firmware_version,
            "minimum_firmware_channel": self.minimum_firmware_channel,
            "firmware_available": self.firmware_available,
            "minimum_firmware_simulated": self.minimum_firmware_simulated,
            "path": self.path,
            "reason_codes": list(self.reason_codes),
        }


def _version_key(version: str) -> tuple[tuple[int, ...], int]:
    core, _, pre = version.partition("-")
    numbers = tuple(int(part) for part in core.split(".") if part.isdigit())
    if not pre:
        rank = 2
    elif pre.startswith("beta"):
        rank = 1
    else:
        rank = 0
    return (numbers, rank)


def _capabilities(conn: sqlite3.Connection, gateway_id: str) -> dict[str, set[str]]:
    rows = conn.execute(
        "SELECT capability_type, capability_name FROM gateway_capabilities WHERE gateway_id = ?",
        (gateway_id,),
    ).fetchall()
    caps: dict[str, set[str]] = {}
    for row in rows:
        caps.setdefault(row["capability_type"], set()).add(row["capability_name"])
    return caps


def _minimum_release(
    conn: sqlite3.Connection,
    hardware_family: str,
    transport: str,
    protocol: str,
    include_simulated: bool = False,
) -> sqlite3.Row | None:
    rows = conn.execute(
        "SELECT * FROM firmware_releases WHERE hardware_family = ? AND status = 'available'",
        (hardware_family,),
    ).fetchall()
    matching = []
    for row in rows:
        if row["simulated"] and not include_simulated:
            # A demo fixture must never satisfy a real capability question.
            continue
        caps = json.loads(row["capabilities_json"])
        transports = {normalize_transport(str(t)) for t in caps.get("transports", [])}
        protocols = {normalize_protocol(str(p)) for p in caps.get("protocols", [])}
        if normalize_transport(transport) in transports and normalize_protocol(protocol) in protocols:
            matching.append(row)
    if not matching:
        return None
    # A real release always wins over a simulated one at the same version.
    matching.sort(key=lambda r: (_version_key(r["firmware_version"]), r["simulated"]))
    return matching[0]


def resolve(
    conn: sqlite3.Connection,
    gateway_id: str,
    transport: str,
    protocol: str,
    profile_id: str | None,
    include_simulated: bool = False,
) -> CapabilityDecision:
    gateway = conn.execute(
        "SELECT * FROM gateways WHERE gateway_id = ?", (gateway_id,)
    ).fetchone()
    if gateway is None:
        raise KeyError(f"unknown gateway: {gateway_id}")

    # Capability names are stored in a mix of spellings ("BLE" in the gateway
    # capability rows, "ble" in profiles), so every comparison happens on the
    # canonical identifiers, never on raw strings.
    transport = normalize_transport(transport)
    protocol = normalize_protocol(protocol)

    caps = _capabilities(conn, gateway_id)
    decision = CapabilityDecision(
        transport=transport,
        protocol=protocol,
        profile_id=profile_id,
        gateway_firmware_version=gateway["firmware_version"],
        hardware_family=gateway["hardware_family"],
    )

    gateway_transports = {normalize_transport(name) for name in caps.get("transport", set())}
    gateway_protocols = {normalize_protocol(name) for name in caps.get("protocol", set())}
    decision.gateway_supports_transport = transport in gateway_transports
    decision.gateway_supports_protocol = protocol in gateway_protocols
    decision.gateway_has_profile = bool(profile_id) and profile_id in caps.get("profile", set())

    if decision.gateway_supports_transport:
        decision.reason_codes.append("gateway_supports_transport")
    else:
        decision.reason_codes.append("gateway_missing_transport")
        decision.required_capabilities.append(f"transport:{transport}")
    if decision.gateway_supports_protocol:
        decision.reason_codes.append("gateway_supports_protocol")
    else:
        decision.reason_codes.append("gateway_missing_protocol")
        decision.required_capabilities.append(f"protocol:{protocol}")
    if decision.gateway_has_profile:
        decision.reason_codes.append("profile_already_deployed")
    else:
        decision.reason_codes.append("profile_not_deployed")

    # An UNSUPPORTED profile is a dead end: no amount of updating helps.
    profile_status = None
    if profile_id:
        row = conn.execute(
            "SELECT status FROM printer_profiles WHERE profile_id = ?", (profile_id,)
        ).fetchone()
        profile_status = row["status"] if row else None
    if profile_status == "UNSUPPORTED":
        decision.path = PATH_BLOCKED
        decision.reason_codes.append("profile_marked_unsupported")
        return decision

    decision.requires_firmware_update = not (
        decision.gateway_supports_transport and decision.gateway_supports_protocol
    )
    decision.requires_profile_update = not decision.gateway_has_profile

    if decision.requires_firmware_update:
        release = _minimum_release(
            conn, decision.hardware_family, transport, protocol, include_simulated
        )
        if release is None:
            decision.path = PATH_BLOCKED
            decision.firmware_available = False
            decision.reason_codes.append("no_firmware_release_provides_capability")
            return decision
        decision.minimum_firmware_version = release["firmware_version"]
        decision.minimum_firmware_channel = release["channel"]
        decision.minimum_firmware_simulated = bool(release["simulated"])
        decision.firmware_available = True
        decision.path = PATH_FIRMWARE
    elif decision.requires_profile_update:
        decision.path = PATH_PROFILE_ONLY
    else:
        decision.path = PATH_PROFILE_ONLY
        decision.reason_codes.append("nothing_to_deploy")

    return decision

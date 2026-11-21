"""Fingerprint normalisation.

A gateway reports whatever its radio stack produced; the compatibility engine
needs one canonical shape. Normalisation here is deterministic and versioned:
everything downstream compares normalised fingerprints only, so a future
gateway firmware can add fields without invalidating stored cases.
"""

from __future__ import annotations

from typing import Any

from .canonical import (
    SPP_UUID_SHORT,
    normalize_transport,
    normalize_uuid,
)

__all__ = [
    "NORMALIZER_VERSION",
    "normalize_transport",
    "normalize_uuid",
    "normalize_fingerprint",
    "summary_line",
]

NORMALIZER_VERSION = 1


def _uuid_list(values: Any) -> list[str]:
    if not values:
        return []
    if isinstance(values, str):
        values = [values]
    seen: list[str] = []
    for raw in values:
        norm = normalize_uuid(str(raw))
        if norm and norm not in seen:
            seen.append(norm)
    return seen


def normalize_fingerprint(raw: dict[str, Any]) -> dict[str, Any]:
    """Turn gateway raw data into the canonical fingerprint shape.

    Only discovery data decides the transport. A user's answer ("it connects
    over Wi-Fi") is a hint about their own experience, not evidence: the real
    link may end up being IPP, raw TCP, ESC/POS over TCP or something else, so
    it is stored on the case and never used here.
    """
    advertised = _uuid_list(raw.get("advertised_services"))
    services = _uuid_list(raw.get("gatt_services") or raw.get("services"))
    writes = _uuid_list(raw.get("write_characteristics"))
    notifies = _uuid_list(raw.get("notify_characteristics"))
    spp = _uuid_list(raw.get("spp_uuid") or raw.get("spp_uuids"))

    transport = normalize_transport(raw.get("transport"))
    transport_source = "gateway_reported"
    if SPP_UUID_SHORT in spp:
        transport = "bluetooth_classic"
        transport_source = "spp_uuid"
    elif transport == "unknown" and (services or advertised):
        transport = "ble"
        transport_source = "inferred_from_gatt"

    name = str(raw.get("name") or "")
    features = {
        "ble_gatt": bool(services or advertised),
        "classic_spp": bool(spp) or transport == "bluetooth_classic",
        "ae30_service": "ae30" in services,
        "ae01_write": "ae01" in writes,
        "ae02_notify": "ae02" in notifies,
        "af30_advertised": "af30" in advertised,
        "ipp": "ipp" in str(raw).lower(),
    }
    if "ipp" in str(raw).lower():
        transport = "ipp"
        transport_source = "ipp_hint"

    rssi = raw.get("rssi")
    return {
        "normalizer_version": NORMALIZER_VERSION,
        "transport": transport,
        "transport_source": transport_source,
        "name": name,
        "address": str(raw.get("address") or ""),
        "rssi": int(rssi) if isinstance(rssi, (int, float)) else None,
        "advertised_service_uuids": advertised,
        "service_uuids": services,
        "write_uuids": writes,
        "notify_uuids": notifies,
        "spp_uuids": spp,
        "features": features,
        "raw_keys": sorted(raw.keys()),
    }


def summary_line(normalized: dict[str, Any]) -> str:
    """Short human line for the UI / logs."""
    parts = [normalized.get("transport", "unknown").upper()]
    if normalized.get("name"):
        parts.append(normalized["name"])
    for key in (
        "advertised_service_uuids",
        "service_uuids",
        "write_uuids",
        "notify_uuids",
    ):
        values = normalized.get(key) or []
        if values:
            parts.append(",".join(values))
    return " · ".join(parts)

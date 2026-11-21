"""Canonical transport / protocol identifiers.

This module is the single source of truth for the two vocabularies that every
other module compares. Anything arriving from a radio stack, a profile JSON or a
client request is mapped here at the boundary; nothing downstream is allowed to
keep its own spelling or to "tolerate" a variant on the fly.

Canonical transports: ``ble``, ``bluetooth_classic``, ``tcp``, ``ipp``, ``usb``.
Canonical protocols:  ``tiny_iprint``, ``escpos``, ``ipp``, ``memobird``,
``unknown``.

Both normalisers are total: an unrecognised input becomes ``unknown`` rather
than raising, because a gateway must be able to record what it does not know.
"""

from __future__ import annotations

TRANSPORTS: tuple[str, ...] = ("ble", "bluetooth_classic", "tcp", "ipp", "usb")
PROTOCOLS: tuple[str, ...] = ("tiny_iprint", "escpos", "ipp", "memobird", "unknown")

UNKNOWN = "unknown"

# Bluetooth base UUID. ``AF30``, ``af30`` and
# ``0000af30-0000-1000-8000-00805f9b34fb`` are the same identifier.
BASE_UUID_SUFFIX = "-0000-1000-8000-00805f9b34fb"
SPP_UUID = "00001101-0000-1000-8000-00805f9b34fb"
SPP_UUID_SHORT = "1101"

# Legacy / vendor spellings seen in gateway reports, firmware release JSON and
# profile files. Wi-Fi is a link medium, not a transport: the gateway reaches a
# network printer over tcp.
TRANSPORT_ALIASES: dict[str, str] = {
    "ble": "ble",
    "bluetooth_le": "ble",
    "bluetooth le": "ble",
    "bluetooth low energy": "ble",
    "bluetooth-le": "ble",
    "gatt": "ble",
    "bt_classic": "bluetooth_classic",
    "bluetooth_classic": "bluetooth_classic",
    "bluetooth classic": "bluetooth_classic",
    "classic": "bluetooth_classic",
    "spp": "bluetooth_classic",
    "tcp": "tcp",
    "raw_tcp": "tcp",
    "raw tcp": "tcp",
    "socket": "tcp",
    "network": "tcp",
    "lan": "tcp",
    "wifi": "tcp",
    "wi-fi": "tcp",
    "ipp": "ipp",
    "usb": "usb",
    UNKNOWN: UNKNOWN,
    "": UNKNOWN,
}

PROTOCOL_ALIASES: dict[str, str] = {
    "tiny_iprint": "tiny_iprint",
    "tiny iprint": "tiny_iprint",
    "tiny": "tiny_iprint",
    "iprint": "tiny_iprint",
    "catprinter": "tiny_iprint",
    "escpos": "escpos",
    "esc/pos": "escpos",
    "esc_pos": "escpos",
    "esc-pos": "escpos",
    "spp": "escpos",
    "ipp": "ipp",
    "memobird": "memobird",
    UNKNOWN: UNKNOWN,
    "": UNKNOWN,
}


def normalize_transport(value: str | None) -> str:
    """Map any transport spelling onto ``TRANSPORTS`` (or ``unknown``)."""
    if value is None:
        return UNKNOWN
    return TRANSPORT_ALIASES.get(str(value).strip().lower(), UNKNOWN)


def normalize_protocol(value: str | None) -> str:
    """Map any protocol spelling onto ``PROTOCOLS`` (or ``unknown``)."""
    if value is None:
        return UNKNOWN
    return PROTOCOL_ALIASES.get(str(value).strip().lower(), UNKNOWN)


def normalize_uuid(value: str | None) -> str:
    """Collapse the Bluetooth base UUID to its 16-bit short form.

    ``AF30``, ``af30`` and ``0000af30-0000-1000-8000-00805f9b34fb`` all become
    ``af30``; the SPP UUID becomes ``1101``. Non-Bluetooth identifiers (SSIDs,
    custom strings) are only lower-cased so comparisons stay stable.

    Both sides of every comparison go through this function - a profile rule
    must never be compared as a raw string against a normalised fingerprint.
    """
    if not value:
        return ""
    text = value.strip().lower()
    if len(text) == 36 and text.endswith(BASE_UUID_SUFFIX):
        head = text[:8]
        if head.startswith("0000"):
            return head[4:]
    return text


def is_canonical_transport(value: str) -> bool:
    return value in TRANSPORTS


def is_canonical_protocol(value: str) -> bool:
    return value in PROTOCOLS

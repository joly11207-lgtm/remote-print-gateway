"""Gateway Simulator.

V0.1 must be demonstrable without hardware. These three devices are the
canonical samples: a known-answer BLE printer (X18), a reserved Bluetooth
Classic sample (G2D) and a genuinely unknown device.
"""

from __future__ import annotations

from typing import Any

SIMULATED_DEVICES: dict[str, dict[str, Any]] = {
    "x18": {
        "key": "x18",
        "label": "X18 (known answer, pretend unknown)",
        "raw": {
            "name": "X18-47E3",
            "transport": "ble",
            "rssi": -52,
            "address": "e3:47:88:2e:e1:2c",
            "advertised_services": ["AF30"],
            "gatt_services": ["AE30"],
            "write_characteristics": ["AE01"],
            "notify_characteristics": ["AE02"],
        },
    },
    "g2d": {
        "key": "g2d",
        "label": "G2D (Bluetooth Classic, NEXT sample)",
        "raw": {
            "name": "G2D",
            "transport": "bluetooth_classic",
            "rssi": -61,
            "address": "11:22:33:44:55:66",
            "spp_uuid": "00001101-0000-1000-8000-00805F9B34FB",
        },
    },
    "unknown": {
        "key": "unknown",
        "label": "Unknown printer (no matching evidence)",
        "raw": {
            "name": "XP-460B",
            "transport": "ble",
            "rssi": -78,
            "address": "dd:0d:30:3a:f4:53",
            "advertised_services": ["FFE0"],
            "gatt_services": ["FFE0"],
            "write_characteristics": ["FFE1"],
            "notify_characteristics": ["FFE2"],
        },
    },
}


def list_devices() -> list[dict[str, Any]]:
    return [
        {"key": device["key"], "label": device["label"], "raw": device["raw"]}
        for device in SIMULATED_DEVICES.values()
    ]


def resolve_device_key(
    brand: str = "", model: str = "", device_key: str | None = None
) -> str:
    """Pick which simulated device answers this case.

    An explicit key always wins (the UI uses it for demos). Otherwise the brand
    and model text is matched the way a real scan would have to guess.
    """
    if device_key:
        key = device_key.strip().lower()
        if key in SIMULATED_DEVICES:
            return key
    text = f"{brand} {model}".lower()
    if "x18" in text or "爱记星" in text:
        return "x18"
    if "g2d" in text:
        return "g2d"
    return "unknown"


def scan(device_key: str) -> dict[str, Any]:
    device = SIMULATED_DEVICES.get(device_key)
    if device is None:
        raise KeyError(f"unknown simulated device: {device_key}")
    return dict(device["raw"])

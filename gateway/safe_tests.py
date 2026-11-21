"""Safe Test Registry.

Only tests registered here may ever be executed against a printer. Neither the
AI analyst nor a client request can name an arbitrary payload: the engine looks
the id up in this table and rejects everything else.
"""

from __future__ import annotations

from dataclasses import dataclass

from .canonical import (
    is_canonical_protocol,
    is_canonical_transport,
    normalize_protocol,
    normalize_transport,
)


@dataclass(frozen=True)
class SafeTestSpec:
    safe_test_id: str
    protocol: str
    transport: str
    description: str
    steps: tuple[str, ...]
    # Human-readable form of the only frame the test may send. Declaring it in
    # the registry keeps payloads out of client requests entirely.
    command: str = ""


REGISTRY: dict[str, SafeTestSpec] = {}


def _register(spec: SafeTestSpec) -> None:
    # The registry is the execution whitelist; a non-canonical id would make
    # selection depend on spelling, so reject it at import time.
    if not is_canonical_protocol(spec.protocol) or not is_canonical_transport(spec.transport):
        raise ValueError(f"non-canonical safe test identity: {spec.safe_test_id}")
    REGISTRY[spec.safe_test_id] = spec


_register(
    SafeTestSpec(
        safe_test_id="SAFE_TEST_TINY_STATUS_V1",
        protocol="tiny_iprint",
        transport="ble",
        description="Read printer status through the Tiny/iPrint status query.",
        steps=(
            "connect over BLE",
            "write a status request frame",
            "read the status reply",
            "disconnect",
        ),
    )
)
_register(
    SafeTestSpec(
        safe_test_id="SAFE_TEST_MEMOBIRD_INFO_V1",
        protocol="memobird",
        transport="bluetooth_classic",
        description=(
            "Query Memobird device information over the Classic SPP channel;"
            " prints nothing and is not executed in V0.1."
        ),
        steps=(
            "connect over Bluetooth Classic SPP",
            "send CMD 0x01 info query",
            "read reply",
            "disconnect",
        ),
        command="CMD 0x01",
    )
)
_register(
    SafeTestSpec(
        safe_test_id="SAFE_TEST_IPP_ATTRIBUTES_V1",
        protocol="ipp",
        transport="ipp",
        description="Fetch IPP printer attributes over the network; prints nothing.",
        steps=("connect over IPP", "Get-Printer-Attributes", "disconnect"),
    )
)


class UnknownSafeTest(KeyError):
    """Raised for any id that is not in the whitelist."""


def is_allowed(safe_test_id: str) -> bool:
    return safe_test_id in REGISTRY


def get(safe_test_id: str) -> SafeTestSpec:
    try:
        return REGISTRY[safe_test_id]
    except KeyError as error:
        raise UnknownSafeTest(safe_test_id) from error


def recommend(protocol: str, transport: str) -> SafeTestSpec | None:
    """Pick the registered test for a (protocol, transport) pair.

    Never synthesises a new one, and never hands out a test whose transport does
    not match: a BLE status query against an SPP printer would be meaningless.
    """
    want_protocol = normalize_protocol(protocol)
    want_transport = normalize_transport(transport)
    for spec in REGISTRY.values():
        if spec.protocol == want_protocol and spec.transport == want_transport:
            return spec
    return None


def as_list() -> list[dict[str, object]]:
    return [
        {
            "safe_test_id": spec.safe_test_id,
            "protocol": spec.protocol,
            "transport": spec.transport,
            "description": spec.description,
            "steps": list(spec.steps),
            "command": spec.command,
        }
        for spec in REGISTRY.values()
    ]

"""Gateway sources: one interface, two implementations.

The compatibility engine consumes a *normalized fingerprint*; it must not know
whether that fingerprint came from a simulator, a local ESP32 or a future cloud
gateway. Everything transport-specific lives behind this module:

    GatewayAdapter
    |-- SimulatorGatewayAdapter   (V0.1 fixtures, still used for development)
    +-- RealGatewayAdapter        (ESP32 over LAN, the V0.2 path)

Five calls are all a gateway has to support:

    GET  /api/status      -> identity + transports + protocols
    POST /api/scan        -> discovery + GATT evidence (raw fingerprint)
    POST /api/safe-test   -> execute one registry-approved test, parse the reply

Deliberately no printing, no job queue and no cloud protocol: V0.2 proves the
identification boundary only.
"""

from __future__ import annotations

import json
import socket
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from typing import Any, Callable, Protocol

from . import config, simulator

MODE_SIMULATOR = "simulator"
MODE_REAL = "real"

SAFE_TEST_TINY_STATUS_V1 = "SAFE_TEST_TINY_STATUS_V1"


# --------------------------------------------------------------------------- #
# errors: the API maps these to explicit HTTP codes, never to "unsupported"
# --------------------------------------------------------------------------- #
class GatewayError(RuntimeError):
    """Base class for anything the gateway layer can fail with."""


class GatewayOffline(GatewayError):
    """No route to the gateway (refused, unreachable, DNS, socket error)."""


class GatewayTimeout(GatewayError):
    """The gateway accepted the connection but did not answer in time."""


class GatewayInvalidResponse(GatewayError):
    """The gateway answered with something the contract does not allow."""


class PrinterNotFound(GatewayError):
    """The gateway scanned successfully and found nothing to identify."""


# --------------------------------------------------------------------------- #
# value objects
# --------------------------------------------------------------------------- #
@dataclass(frozen=True)
class GatewayStatus:
    gateway_id: str
    hardware_family: str
    firmware_version: str
    online: bool
    supported_transports: tuple[str, ...]
    supported_protocols: tuple[str, ...]
    source: str
    raw: dict[str, Any] = field(default_factory=dict, compare=False)

    def as_dict(self) -> dict[str, Any]:
        return {
            "gateway_id": self.gateway_id,
            "hardware_family": self.hardware_family,
            "firmware_version": self.firmware_version,
            "online": self.online,
            "status": "online" if self.online else "offline",
            "supported_transports": list(self.supported_transports),
            "supported_protocols": list(self.supported_protocols),
            "source": self.source,
        }


@dataclass(frozen=True)
class ScanResult:
    """One identified printer, already in the gateway's raw fingerprint shape."""

    raw: dict[str, Any]
    elapsed_ms: int
    gateway_id: str
    scanned: int = 0

    def as_dict(self) -> dict[str, Any]:
        return {
            "gateway_id": self.gateway_id,
            "elapsed_ms": self.elapsed_ms,
            "scanned": self.scanned,
            "raw_fingerprint": self.raw,
        }


@dataclass(frozen=True)
class SafeTestExecution:
    """Outcome of one executed safe test (pass/fail + what the printer said)."""

    status: str
    elapsed_ms: int
    detail: dict[str, Any] = field(default_factory=dict)

    @property
    def passed(self) -> bool:
        return self.status == "passed"


class GatewayAdapter(Protocol):
    gateway_id: str
    mode: str

    def status(self) -> GatewayStatus: ...

    def scan(self) -> ScanResult: ...

    def run_safe_test(self, spec: Any, target: dict[str, Any]) -> SafeTestExecution: ...


# --------------------------------------------------------------------------- #
# simulator (V0.1 fixtures, unchanged behaviour)
# --------------------------------------------------------------------------- #
class SimulatorGatewayAdapter:
    """Answer from ``simulator.SIMULATED_DEVICES`` instead of a radio."""

    mode = MODE_SIMULATOR

    def __init__(
        self,
        gateway_id: str,
        *,
        identity: dict[str, Any] | None = None,
        device_key: str | None = None,
        scan: Callable[[str], dict[str, Any]] = simulator.scan,
        outcome: str | None = None,
    ) -> None:
        self.gateway_id = gateway_id
        self._identity = dict(identity or {})
        self._device_key = device_key
        self._scan = scan
        # The simulator cannot execute anything, so the caller's requested
        # outcome stands in for a real result. Real adapters ignore this.
        self._outcome = "failed" if outcome == "failed" else "passed"

    def status(self) -> GatewayStatus:
        return GatewayStatus(
            gateway_id=self.gateway_id,
            hardware_family=str(self._identity.get("hardware_family") or "GW-ESP32-V1"),
            firmware_version=str(self._identity.get("firmware_version") or "0.0.0"),
            online=str(self._identity.get("status") or "online") == "online",
            supported_transports=("ble",),
            supported_protocols=("tiny_iprint",),
            source=MODE_SIMULATOR,
            raw=dict(self._identity),
        )

    def scan(self) -> ScanResult:
        key = self._device_key or "unknown"
        return ScanResult(raw=dict(self._scan(key)), elapsed_ms=0, gateway_id=self.gateway_id)

    def run_safe_test(self, spec: Any, target: dict[str, Any]) -> SafeTestExecution:
        # The simulator has no printer attached: the run is recorded, not
        # executed. test_gateway_engine.py keeps asserting exactly this.
        return SafeTestExecution(
            status=self._outcome,
            elapsed_ms=0,
            detail={
                "steps": list(getattr(spec, "steps", ()) or ()),
                "description": getattr(spec, "description", ""),
                "simulated": True,
                "note": "V0.1 has no printer attached; the run is recorded, not executed.",
            },
        )


# --------------------------------------------------------------------------- #
# real ESP32 over the LAN
# --------------------------------------------------------------------------- #
class RealGatewayAdapter:
    """Talk to the ESP32 debug API. The URL never leaves this class.

    ``opener`` is injectable so the tests can drive the exact same parsing and
    error mapping without a radio or a socket.
    """

    mode = MODE_REAL

    def __init__(
        self,
        gateway_id: str,
        base_url: str,
        *,
        timeout: float | None = None,
        scan_duration_ms: int | None = None,
        opener: Callable[..., Any] | None = None,
    ) -> None:
        if not base_url:
            raise GatewayOffline("real gateway has no base_url configured")
        self.gateway_id = gateway_id
        self._base_url = base_url.rstrip("/")
        self._timeout = config.GATEWAY_HTTP_TIMEOUT if timeout is None else timeout
        self._scan_duration_ms = (
            config.GATEWAY_SCAN_DURATION_MS if scan_duration_ms is None else scan_duration_ms
        )
        self._opener = opener or urllib.request.urlopen

    # ------------------------------------------------------------------ plumbing
    def _request(self, method: str, path: str, payload: dict[str, Any] | None = None) -> dict[str, Any]:
        url = self._base_url + path
        data = None if payload is None else json.dumps(payload).encode("utf-8")
        request = urllib.request.Request(
            url,
            data=data,
            method=method,
            headers={"Content-Type": "application/json", "Accept": "application/json"},
        )
        try:
            with self._opener(request, timeout=self._timeout) as response:
                body = response.read()
        except urllib.error.HTTPError as error:
            # The gateway is alive but rejected the call: a contract problem,
            # not an offline gateway.
            raise GatewayInvalidResponse(
                f"gateway answered HTTP {error.code} for {path}"
            ) from error
        except (urllib.error.URLError, socket.timeout, TimeoutError, OSError) as error:
            reason = getattr(error, "reason", error)
            if isinstance(reason, (socket.timeout, TimeoutError)) or isinstance(
                error, (socket.timeout, TimeoutError)
            ):
                raise GatewayTimeout(f"gateway timed out on {path}") from error
            raise GatewayOffline(f"gateway unreachable: {reason}") from error

        try:
            decoded = json.loads(body.decode("utf-8"))
        except (UnicodeDecodeError, ValueError) as error:
            raise GatewayInvalidResponse(f"gateway returned non-JSON for {path}") from error
        if not isinstance(decoded, dict):
            raise GatewayInvalidResponse(f"gateway returned a non-object for {path}")
        return decoded

    @staticmethod
    def _strings(value: Any) -> tuple[str, ...]:
        if not isinstance(value, list):
            return ()
        return tuple(str(item) for item in value if isinstance(item, (str, int)))

    # --------------------------------------------------------------------- calls
    def status(self) -> GatewayStatus:
        payload = self._request("GET", "/api/status")
        hardware = payload.get("hardware_family")
        firmware = payload.get("firmware_version")
        if not isinstance(hardware, str) or not hardware:
            raise GatewayInvalidResponse("gateway status has no hardware_family")
        if not isinstance(firmware, str) or not firmware:
            raise GatewayInvalidResponse("gateway status has no firmware_version")
        transports = self._strings(payload.get("supported_transports"))
        protocols = self._strings(payload.get("supported_protocols"))
        if not transports or not protocols:
            raise GatewayInvalidResponse("gateway status has no transports/protocols")
        online = payload.get("online")
        if online is None:
            online = str(payload.get("status") or "online") == "online"
        return GatewayStatus(
            gateway_id=self.gateway_id,
            hardware_family=hardware,
            firmware_version=firmware,
            online=bool(online),
            supported_transports=transports,
            supported_protocols=protocols,
            source=MODE_REAL,
            raw=payload,
        )

    def scan(self) -> ScanResult:
        payload = self._request(
            "POST", "/api/scan", {"duration_ms": self._scan_duration_ms, "inspect": True}
        )
        devices = payload.get("devices")
        if not isinstance(devices, list):
            raise GatewayInvalidResponse("gateway scan has no devices[]")
        candidates = [item for item in devices if isinstance(item, dict)]
        if not candidates:
            raise PrinterNotFound("no printer found in range")

        # Prefer a device whose GATT was actually inspected (service/write/notify
        # confirmed by a real connect), then the strongest signal.
        def rank(item: dict[str, Any]) -> tuple[int, int]:
            verified = 1 if item.get("gatt_verified") else 0
            rssi = item.get("rssi")
            return (verified, int(rssi) if isinstance(rssi, (int, float)) else -999)

        chosen = max(candidates, key=rank)
        raw = {
            "name": str(chosen.get("name") or ""),
            "transport": str(chosen.get("transport") or "ble"),
            "address": str(chosen.get("address") or ""),
            "rssi": chosen.get("rssi"),
            "advertised_services": list(self._strings(chosen.get("advertised_services"))),
            "gatt_services": list(self._strings(chosen.get("gatt_services"))),
            "write_characteristics": list(self._strings(chosen.get("write_characteristics"))),
            "notify_characteristics": list(self._strings(chosen.get("notify_characteristics"))),
        }
        elapsed = payload.get("elapsed_ms")
        return ScanResult(
            raw=raw,
            elapsed_ms=int(elapsed) if isinstance(elapsed, (int, float)) else 0,
            gateway_id=self.gateway_id,
            scanned=len(candidates),
        )

    def run_safe_test(self, spec: Any, target: dict[str, Any]) -> SafeTestExecution:
        safe_test_id = getattr(spec, "safe_test_id", None) or SAFE_TEST_TINY_STATUS_V1
        payload = self._request(
            "POST",
            "/api/safe-test",
            {"safe_test_id": safe_test_id, "address": str(target.get("address") or "")},
        )
        result = str(payload.get("status") or "").strip().lower()
        if result not in {"passed", "failed"}:
            raise GatewayInvalidResponse(
                f"gateway safe-test returned status={result!r}"
            )
        elapsed = payload.get("elapsed_ms")
        detail = {
            "steps": list(getattr(spec, "steps", ()) or ()),
            "description": getattr(spec, "description", ""),
            "simulated": False,
            "safe_test_id": safe_test_id,
            "gateway_response": payload,
        }
        return SafeTestExecution(
            status=result,
            elapsed_ms=int(elapsed) if isinstance(elapsed, (int, float)) else 0,
            detail=detail,
        )


# --------------------------------------------------------------------------- #
# factory
# --------------------------------------------------------------------------- #
def gateway_mode(row: Any) -> str:
    """Read the mode column defensively (older DB rows have no value)."""
    try:
        value = row["mode"]
    except (KeyError, IndexError, TypeError):
        value = None
    return MODE_REAL if str(value or "").lower() == MODE_REAL else MODE_SIMULATOR


def for_gateway(
    row: Any,
    *,
    device_key: str | None = None,
    opener: Callable[..., Any] | None = None,
    simulate_outcome: str | None = None,
) -> GatewayAdapter:
    """Build the adapter described by a ``gateways`` row."""
    mode = gateway_mode(row)
    gateway_id = row["gateway_id"]
    if mode == MODE_REAL:
        try:
            base_url = row["base_url"]
        except (KeyError, IndexError, TypeError):
            base_url = None
        return RealGatewayAdapter(gateway_id, str(base_url or ""), opener=opener)
    return SimulatorGatewayAdapter(
        gateway_id,
        identity=dict(row),
        device_key=device_key,
        outcome=simulate_outcome,
    )

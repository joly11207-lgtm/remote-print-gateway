"""Adapter-level tests for V0.2 (real gateway wiring).

The real ESP32 is never contacted: the HTTP layer is driven through an injected
opener, and the engine is driven through an injected adapter. That is deliberate
- a fake adapter passing here is NOT evidence that a real X18 answered. Hardware
evidence only comes from tests/acceptance (see the V0.2 report).

Covered:
  * simulator adapter (unchanged V0.1 behaviour)
  * real adapter HTTP parsing + every error class
  * engine real-mode flow: scan -> x18_aijixing -> safe test -> printer instance
  * gateway offline / printer not found / invalid response / safe test failed
  * API: real gateway listing, offline snapshot, device_key refused

Usage: python tests/test_gateway_adapters.py
"""

from __future__ import annotations

import io
import json
import os
import socket
import sqlite3
import sys
import tempfile
import threading
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
os.environ["RTB_GATEWAY_DB"] = str(Path(tempfile.mkdtemp(prefix="rtb-adapt-")) / "gateway.db")
os.environ.pop("RTB_REAL_GATEWAY_URL", None)
os.environ.pop("RTB_AI_ENDPOINT", None)
os.environ.pop("RTB_ALLOW_SIMULATED_FIRMWARE", None)

from gateway import (  # noqa: E402
    adapters,
    api,
    config,
    db,
    engine,
    fingerprint,
    safe_tests,
    simulator,
)

REAL_GATEWAY_ID = "gw-real-test"
X18_RAW = dict(simulator.SIMULATED_DEVICES["x18"]["raw"])


def section(title: str) -> None:
    print(f"\n== {title} ==")


# --------------------------------------------------------------------------- #
# fake HTTP plumbing
# --------------------------------------------------------------------------- #
class FakeResponse:
    def __init__(self, payload: dict | bytes) -> None:
        self._body = (
            payload if isinstance(payload, bytes) else json.dumps(payload).encode("utf-8")
        )

    def read(self) -> bytes:
        return self._body

    def __enter__(self) -> "FakeResponse":
        return self

    def __exit__(self, *exc: object) -> None:
        return None


class FakeOpener:
    """Records requests and answers from a table, or raises a chosen error."""

    def __init__(self, routes: dict[tuple[str, str], dict | bytes], error: Exception | None = None):
        self.routes = routes
        self.error = error
        self.calls: list[tuple[str, str, dict | None, float]] = []

    def __call__(self, request, timeout=None):
        method = request.get_method()
        path = urllib.request.urlparse(request.full_url).path
        body = json.loads(request.data.decode("utf-8")) if request.data else None
        self.calls.append((method, path, body, timeout))
        if self.error is not None:
            raise self.error
        payload = self.routes.get((method, path))
        if payload is None:
            raise urllib.error.HTTPError(request.full_url, 404, "not found", {}, None)
        return FakeResponse(payload)


STATUS_OK = {
    "gateway_id": "gw-esp32-123456",
    "hardware_family": "ESP32-WROOM-32E",
    "firmware_version": "0.2.0",
    "online": True,
    "supported_transports": ["ble"],
    "supported_protocols": ["tiny_iprint"],
}

SCAN_OK = {
    "result": "Ok",
    "transport": "ble",
    "elapsed_ms": 3120,
    "scanned": 3,
    "devices": [
        {
            "name": "XP-460B",
            "address": "dd:0d:30:3a:f4:53",
            "rssi": -78,
            "advertised_services": ["0000ffe0-0000-1000-8000-00805f9b34fb"],
            "gatt_verified": False,
            "model_id": "unmatched",
        },
        {
            "name": "X18-47E3",
            "address": "e3:47:88:2e:e1:2c",
            "rssi": -52,
            "advertised_services": ["0000af30-0000-1000-8000-00805f9b34fb"],
            "gatt_verified": True,
            "gatt_services": ["AE30"],
            "write_characteristics": ["AE01"],
            "notify_characteristics": ["AE02"],
            "model_id": "x18_aijixing",
        },
    ],
}

SAFE_TEST_PASS = {
    "ok": True,
    "safe_test_id": "SAFE_TEST_TINY_STATUS_V1",
    "status": "passed",
    "ready": True,
    "raw_status": 0,
    "opcode": "A3",
    "elapsed_ms": 420,
}


class FakeRealAdapter:
    """Engine-side double: same interface, no HTTP."""

    mode = adapters.MODE_REAL

    def __init__(self, gateway_id, *, raw=None, scan_error=None, test_status="passed"):
        self.gateway_id = gateway_id
        self._raw = dict(raw if raw is not None else X18_RAW)
        self._scan_error = scan_error
        self._test_status = test_status
        self.scans = 0
        self.tests = 0

    def status(self):
        return adapters.GatewayStatus(
            gateway_id=self.gateway_id,
            hardware_family="ESP32-WROOM-32E",
            firmware_version="0.2.0",
            online=True,
            supported_transports=("ble",),
            supported_protocols=("tiny_iprint",),
            source=adapters.MODE_REAL,
        )

    def scan(self):
        self.scans += 1
        if self._scan_error is not None:
            raise self._scan_error
        return adapters.ScanResult(
            raw=dict(self._raw), elapsed_ms=3120, gateway_id=self.gateway_id, scanned=3
        )

    def run_safe_test(self, spec, target):
        self.tests += 1
        return adapters.SafeTestExecution(
            status=self._test_status,
            elapsed_ms=420,
            detail={
                "steps": list(spec.steps),
                "description": spec.description,
                "simulated": False,
                "gateway_response": {"status": self._test_status},
            },
        )


# --------------------------------------------------------------------------- #
# 1. simulator adapter (V0.1 must not change)
# --------------------------------------------------------------------------- #
def check_simulator_adapter() -> None:
    section("simulator adapter keeps the V0.1 shape")

    adapter = adapters.for_gateway(
        {
            "gateway_id": db.GATEWAY["gateway_id"],
            "mode": "simulator",
            "hardware_family": db.GATEWAY["hardware_family"],
            "firmware_version": db.GATEWAY["firmware_version"],
            "status": "online",
        },
        device_key="x18",
    )
    assert adapter.mode == adapters.MODE_SIMULATOR
    status = adapter.status()
    assert status.online and status.source == "simulator"

    scan = adapter.scan()
    assert scan.raw == simulator.scan("x18"), scan.raw
    assert fingerprint.normalize_fingerprint(scan.raw)["transport"] == "ble"

    spec = engine.safe_tests.get("SAFE_TEST_TINY_STATUS_V1")
    run = adapter.run_safe_test(spec, {})
    assert run.status == "passed" and run.detail["simulated"] is True, run
    print("  scan/test -> simulated x18, detail.simulated=True")


# --------------------------------------------------------------------------- #
# 2. real adapter: parsing + error mapping
# --------------------------------------------------------------------------- #
def check_real_adapter_http() -> None:
    section("real adapter parses the ESP32 contract")

    opener = FakeOpener(
        {
            ("GET", "/api/status"): STATUS_OK,
            ("POST", "/api/scan"): SCAN_OK,
            ("POST", "/api/safe-test"): SAFE_TEST_PASS,
        }
    )
    adapter = adapters.RealGatewayAdapter(
        "gw-real", "http://192.0.2.10", timeout=6, scan_duration_ms=4000, opener=opener
    )

    status = adapter.status()
    assert status.hardware_family == "ESP32-WROOM-32E"
    assert status.firmware_version == "0.2.0"
    assert status.supported_transports == ("ble",)
    assert status.online is True

    scan = adapter.scan()
    assert scan.raw["name"] == "X18-47E3", scan.raw
    assert any("af30" in value.lower() for value in scan.raw["advertised_services"]), scan.raw
    assert scan.raw["gatt_services"] == ["AE30"]
    assert scan.raw["write_characteristics"] == ["AE01"]
    assert scan.raw["notify_characteristics"] == ["AE02"]
    assert scan.scanned == 2
    # The GATT-verified device wins over a stronger/advertised-only one.
    print(f"  scan -> {scan.raw['name']} verified={scan.scanned} candidates")
    assert opener.calls[1][2]["duration_ms"] == 4000
    assert opener.calls[1][3] == 6, "timeout must be forwarded"

    normalized = fingerprint.normalize_fingerprint(scan.raw)
    assert normalized["transport"] == "ble"
    assert normalized["features"]["ae30_service"] is True
    assert normalized["features"]["af30_advertised"] is True
    assert normalized["address"] == "e3:47:88:2e:e1:2c"

    spec = engine.safe_tests.get("SAFE_TEST_TINY_STATUS_V1")
    run = adapter.run_safe_test(spec, normalized)
    assert run.status == "passed" and run.detail["simulated"] is False
    assert opener.calls[2][2]["safe_test_id"] == "SAFE_TEST_TINY_STATUS_V1"
    assert opener.calls[2][2]["address"] == "e3:47:88:2e:e1:2c"
    print("  safe test -> passed (no simulation flag, address forwarded)")

    # A failed test is a normal answer, not a transport error.
    failing = adapters.RealGatewayAdapter(
        "gw-real",
        "http://192.0.2.10",
        opener=FakeOpener({("POST", "/api/safe-test"): {**SAFE_TEST_PASS, "status": "failed"}}),
    )
    failed = failing.run_safe_test(spec, normalized)
    assert failed.status == "failed"
    print("  safe test -> failed is parsed, not raised")


def check_real_adapter_errors() -> None:
    section("real adapter error classes")

    def build(error: Exception) -> adapters.RealGatewayAdapter:
        return adapters.RealGatewayAdapter(
            "gw-real", "http://192.0.2.10", opener=FakeOpener({}, error=error)
        )

    # Connection refused -> offline (the "Gateway is offline" case).
    try:
        build(urllib.error.URLError(ConnectionRefusedError(10061, "refused"))).status()
        raise AssertionError("expected GatewayOffline")
    except adapters.GatewayOffline as error:
        print(f"  refused -> GatewayOffline ({error})")

    # Timeout -> a distinct class (504, not 503).
    try:
        build(socket.timeout("timed out")).status()
        raise AssertionError("expected GatewayTimeout")
    except adapters.GatewayTimeout as error:
        print(f"  timeout -> GatewayTimeout ({error})")

    # Non-JSON body -> invalid response.
    try:
        adapters.RealGatewayAdapter(
            "gw-real", "http://192.0.2.10", opener=FakeOpener({("GET", "/api/status"): b"<html>"})
        ).status()
        raise AssertionError("expected GatewayInvalidResponse")
    except adapters.GatewayInvalidResponse as error:
        print(f"  garbage -> GatewayInvalidResponse ({error})")

    # Missing contract fields -> invalid response, never "printer unsupported".
    try:
        adapters.RealGatewayAdapter(
            "gw-real",
            "http://192.0.2.10",
            opener=FakeOpener({("GET", "/api/status"): {"hardware_family": "ESP32"}}),
        ).status()
        raise AssertionError("expected GatewayInvalidResponse")
    except adapters.GatewayInvalidResponse as error:
        print(f"  missing fields -> GatewayInvalidResponse ({error})")

    # Empty scan -> PrinterNotFound (a retryable condition).
    try:
        adapters.RealGatewayAdapter(
            "gw-real",
            "http://192.0.2.10",
            opener=FakeOpener({("POST", "/api/scan"): {"devices": []}}),
        ).scan()
        raise AssertionError("expected PrinterNotFound")
    except adapters.PrinterNotFound as error:
        print(f"  no devices -> PrinterNotFound ({error})")

    # Unknown safe-test status -> invalid response.
    try:
        adapters.RealGatewayAdapter(
            "gw-real",
            "http://192.0.2.10",
            opener=FakeOpener({("POST", "/api/safe-test"): {"status": "maybe"}}),
        ).run_safe_test(engine.safe_tests.get("SAFE_TEST_TINY_STATUS_V1"), {})
        raise AssertionError("expected GatewayInvalidResponse")
    except adapters.GatewayInvalidResponse as error:
        print(f"  odd status -> GatewayInvalidResponse ({error})")

    # Fail closed: a real row with no URL is offline, not "simulated".
    try:
        adapters.RealGatewayAdapter("gw-real", "")
        raise AssertionError("expected GatewayOffline")
    except adapters.GatewayOffline:
        print("  no base_url -> GatewayOffline (fail closed)")


# --------------------------------------------------------------------------- #
# 3. engine real-mode flow
# --------------------------------------------------------------------------- #
def _register_real_gateway(conn: sqlite3.Connection, base_url: str = "http://192.0.2.10") -> None:
    conn.execute(
        "INSERT OR REPLACE INTO gateways (gateway_id, user_id, name, hardware_family,"
        " firmware_version, status, last_seen, created_at, mode, base_url)"
        " VALUES (?,?,?,?,?,?,?,?,?,?)",
        (
            REAL_GATEWAY_ID,
            config.DEFAULT_USER_ID,
            "Home Gateway (real)",
            "ESP32-WROOM-32E",
            "unknown",
            "offline",
            db.now_iso(),
            db.now_iso(),
            "real",
            base_url,
        ),
    )
    for cap_type, cap_name in (
        ("transport", "ble"),
        ("protocol", "tiny_iprint"),
        ("engine", "profile_engine"),
        ("profile", "x18_aijixing"),
    ):
        conn.execute(
            "INSERT OR IGNORE INTO gateway_capabilities"
            " (gateway_id, capability_type, capability_name, version) VALUES (?,?,?,1)",
            (REAL_GATEWAY_ID, cap_type, cap_name),
        )
    conn.commit()


def check_real_engine_flow() -> None:
    section("engine: real scan -> x18_aijixing -> safe test -> printer instance")

    conn = db.connect()
    try:
        _register_real_gateway(conn)

        case = engine.create_case(
            conn, REAL_GATEWAY_ID, brand="爱记星", model="X18", connection_hint="bluetooth"
        )
        assert case["scan_source"]["kind"] == "gateway", case["scan_source"]
        assert case["raw_fingerprint"] == {}, "a real case must not pre-fabricate a fingerprint"
        assert case["status"] == engine.CREATED

        # device_key is a simulator selector: refused on a real gateway.
        try:
            engine.create_case(conn, REAL_GATEWAY_ID, device_key="x18")
            raise AssertionError("device_key must be refused for a real gateway")
        except engine.EngineError as error:
            print(f"  device_key refused: {error}")

        adapter = FakeRealAdapter(REAL_GATEWAY_ID)
        report = engine.scan_case(conn, case["case_id"], adapter=adapter)
        assert report["status"] == engine.READY_FOR_TEST, report["status"]
        assert report["candidate_profile"] == "x18_aijixing"
        assert report["requires_firmware_update"] is False
        assert report["compatibility_status"] == engine.COMPAT_SUPPORTED
        assert report["recommended_safe_test"]["safe_test_id"] == "SAFE_TEST_TINY_STATUS_V1"
        assert report["recommended_safe_test"]["executable"] is True
        assert report["scan_source"]["kind"] == "gateway"
        assert report["scan"]["elapsed_ms"] == 3120
        print(
            f"  scan -> {report['status']} profile={report['candidate_profile']}"
            f" fw_update={report['requires_firmware_update']}"
        )

        # A client-chosen outcome is not accepted for a real gateway.
        try:
            engine.run_test(conn, case["case_id"], outcome="passed", adapter=adapter)
            raise AssertionError("outcome must be rejected for a real gateway")
        except engine.EngineError as error:
            print(f"  client outcome refused: {error}")

        tested = engine.run_test(conn, case["case_id"], adapter=adapter)
        assert tested["status"] == engine.TEST_PASSED, tested["status"]
        assert tested["safe_test_run"]["simulated"] is False
        assert tested["safe_test_run"]["safe_test_id"] == "SAFE_TEST_TINY_STATUS_V1"
        print(f"  test -> {tested['status']} simulated={tested['safe_test_run']['simulated']}")

        completed = engine.complete_case(conn, case["case_id"])
        assert completed["status"] == engine.COMPLETED
        printer = conn.execute(
            "SELECT * FROM printer_instances WHERE printer_id = ?",
            (completed["printer_id"],),
        ).fetchone()
        assert printer["gateway_id"] == REAL_GATEWAY_ID
        assert printer["profile_id"] == "x18_aijixing"
        assert printer["physical_address"] == X18_RAW["address"]
        assert printer["display_name"]
        print(
            f"  complete -> printer={printer['printer_id']}"
            f" profile={printer['profile_id']} address={printer['physical_address']}"
        )
    finally:
        conn.close()


def check_real_engine_failures() -> None:
    section("engine: offline / not found / safe test failed, case stays retryable")

    conn = db.connect()
    try:
        _register_real_gateway(conn)

        # --- printer not found: case stays CREATED, nothing is created.
        case = engine.create_case(conn, REAL_GATEWAY_ID, brand="爱记星", model="X18")
        try:
            engine.scan_case(
                conn,
                case["case_id"],
                adapter=FakeRealAdapter(
                    REAL_GATEWAY_ID, scan_error=adapters.PrinterNotFound("no printer found")
                ),
            )
            raise AssertionError("expected PrinterNotFound")
        except adapters.PrinterNotFound:
            pass
        stored = engine.get_case(conn, case["case_id"])
        assert stored["status"] == engine.CREATED, stored["status"]
        assert conn.execute(
            "SELECT COUNT(*) AS n FROM printer_instances WHERE case_id = ?",
            (case["case_id"],),
        ).fetchone()["n"] == 0
        print("  not found -> case stays CREATED, retry allowed, no printer row")

        # Retry with a working adapter succeeds.
        report = engine.scan_case(
            conn, case["case_id"], adapter=FakeRealAdapter(REAL_GATEWAY_ID)
        )
        assert report["status"] == engine.READY_FOR_TEST
        print("  retry -> READY_FOR_TEST")

        # --- gateway offline: same contract, distinct error type.
        offline_case = engine.create_case(conn, REAL_GATEWAY_ID, brand="爱记星", model="X18")
        try:
            engine.scan_case(
                conn,
                offline_case["case_id"],
                adapter=FakeRealAdapter(
                    REAL_GATEWAY_ID, scan_error=adapters.GatewayOffline("refused")
                ),
            )
            raise AssertionError("expected GatewayOffline")
        except adapters.GatewayOffline:
            pass
        stored = engine.get_case(conn, offline_case["case_id"])
        assert stored["status"] == engine.CREATED
        assert stored["candidate_profile"] is None  # never "unsupported"
        print("  offline -> case stays CREATED, no candidate, no verdict")

        # --- safe test failed: TEST_FAILED, profile untouched, retry allowed.
        failing = FakeRealAdapter(REAL_GATEWAY_ID, test_status="failed")
        failed = engine.run_test(conn, case["case_id"], adapter=failing)
        assert failed["status"] == engine.TEST_FAILED, failed["status"]
        assert failed["candidate_profile"] == "x18_aijixing", "a failed test is not a match error"
        assert failed["requires_firmware_update"] is False
        assert failed["next_action"] == engine.NEXT_MANUAL_REVIEW
        retried = engine.run_test(conn, case["case_id"], adapter=FakeRealAdapter(REAL_GATEWAY_ID))
        assert retried["status"] == engine.TEST_PASSED
        print("  test failed -> TEST_FAILED (profile kept), retry -> TEST_PASSED")

        # --- gateway unreachable during the test: an API error, not TEST_FAILED.
        reachable_case = engine.create_case(conn, REAL_GATEWAY_ID, brand="爱记星", model="X18")
        engine.scan_case(conn, reachable_case["case_id"], adapter=FakeRealAdapter(REAL_GATEWAY_ID))

        class OfflineAdapter(FakeRealAdapter):
            def run_safe_test(self, spec, target):
                raise adapters.GatewayTimeout("timed out")

        try:
            engine.run_test(conn, reachable_case["case_id"], adapter=OfflineAdapter(REAL_GATEWAY_ID))
            raise AssertionError("expected GatewayTimeout")
        except adapters.GatewayTimeout:
            pass
        still_ready = engine.get_case(conn, reachable_case["case_id"])
        assert still_ready["status"] == engine.READY_FOR_TEST
        print("  test transport error -> case stays READY_FOR_TEST, error surfaces to the API")
    finally:
        conn.close()


# --------------------------------------------------------------------------- #
# 4. API layer with a real (unreachable) gateway row
# --------------------------------------------------------------------------- #
class Server:
    def __init__(self) -> None:
        self._server = api.make_server("127.0.0.1", 0, allow_simulated_firmware=False)
        self._server.daemon_threads = True
        threading.Thread(target=self._server.serve_forever, daemon=True).start()
        self.base = f"http://127.0.0.1:{self._server.server_address[1]}"

    def get(self, path: str) -> tuple[int, dict]:
        return self._call("GET", path)

    def post(self, path: str, body: dict | None = None) -> tuple[int, dict]:
        return self._call("POST", path, body)

    def _call(self, method: str, path: str, body: dict | None = None) -> tuple[int, dict]:
        data = None if body is None else json.dumps(body).encode("utf-8")
        request = urllib.request.Request(self.base + path, data=data, method=method)
        if data is not None:
            request.add_header("Content-Type", "application/json")
        try:
            with urllib.request.urlopen(request, timeout=20) as response:
                return response.status, json.loads(response.read().decode("utf-8"))
        except urllib.error.HTTPError as error:
            return error.code, json.loads(error.read().decode("utf-8"))

    def close(self) -> None:
        self._server.shutdown()
        self._server.server_close()


def check_api_real_gateway() -> None:
    section("API: real gateway is listed, offline is 503/offline - never 'unsupported'")

    conn = db.connect()
    try:
        # A closed port on loopback: reachable TCP stack, nothing listening.
        _register_real_gateway(conn, base_url="http://127.0.0.1:9")
    finally:
        conn.close()

    server = Server()
    try:
        status, body = server.get("/api/gateways")
        assert status == 200, body
        ids = [item["gateway_id"] for item in body["gateways"]]
        assert REAL_GATEWAY_ID in ids
        real = next(item for item in body["gateways"] if item["gateway_id"] == REAL_GATEWAY_ID)
        assert real["mode"] == "real"
        assert body["gateways"][0]["mode"] == "real", "real gateways are listed first"
        print(f"  GET /api/gateways -> {ids}")

        status, snapshot = server.get(f"/api/gateways/{REAL_GATEWAY_ID}")
        assert status == 200, snapshot
        assert snapshot["status"] == "offline"
        assert snapshot["reachable"] is False
        assert snapshot["error"] == "GATEWAY_OFFLINE"
        print(f"  offline snapshot -> reachable=False error={snapshot['error']}")

        status, created = server.post(
            "/api/compatibility/cases",
            {"gateway_id": REAL_GATEWAY_ID, "brand": "爱记星", "model": "X18"},
        )
        assert status == 200, created
        case_id = created["case_id"]
        assert created["scan_source"]["kind"] == "gateway"

        status, error = server.post(f"/api/compatibility/cases/{case_id}/scan")
        assert status == 503 and error["error"] == "GATEWAY_OFFLINE", (status, error)
        print(f"  scan with gateway down -> {status} {error['error']}")

        status, stored = server.get(f"/api/compatibility/cases/{case_id}")
        assert stored["status"] == engine.CREATED
        assert stored["candidate_profile"] is None
        print("  case -> still CREATED (retry, no verdict)")

        status, refused = server.post(
            "/api/compatibility/cases",
            {"gateway_id": REAL_GATEWAY_ID, "device_key": "x18"},
        )
        assert status == 422 and refused["error"] == "ACTION_NOT_ALLOWED", (status, refused)
        print(f"  device_key on real gateway -> {status} {refused['error']}")
    finally:
        server.close()


def main() -> int:
    db.initialize()
    check_simulator_adapter()
    check_real_adapter_http()
    check_real_adapter_errors()
    check_real_engine_flow()
    check_real_engine_failures()
    check_api_real_gateway()
    print("\nPASS: adapters V0.2 (simulator, real HTTP contract, error classes, real-mode engine flow)")
    print("NOTE: all gateway responses above are synthetic; real X18 evidence is a hardware run.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

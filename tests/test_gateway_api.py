"""HTTP API integration tests (real sockets, no hardware, no network).

The API is expected to be a pure transport over gateway.engine: these tests
assert both the documented flows and that nothing is re-decided in the handler.

Usage: python tests/test_gateway_api.py
"""

from __future__ import annotations

import json
import os
import sys
import tempfile
import threading
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
os.environ["RTB_GATEWAY_DB"] = str(Path(tempfile.mkdtemp(prefix="rtb-api-")) / "gateway.db")
os.environ.pop("RTB_AI_ENDPOINT", None)
os.environ.pop("RTB_AI_API_KEY", None)
os.environ.pop("RTB_ALLOW_SIMULATED_FIRMWARE", None)

from gateway import (  # noqa: E402  (import after the DB path is pinned)
    api,
    capability,
    db,
    engine,
    simulated_firmware,
)

GATEWAY_ID = db.GATEWAY["gateway_id"]
CASE_PATH = "/api/compatibility/cases"


def section(title: str) -> None:
    print(f"\n== {title} ==")


class Server:
    def __init__(self, allow_simulated_firmware: bool = False) -> None:
        self._server = api.make_server(
            "127.0.0.1", 0, allow_simulated_firmware=allow_simulated_firmware
        )
        self._server.daemon_threads = True
        self._thread = threading.Thread(target=self._server.serve_forever, daemon=True)
        self._thread.start()
        self.base = f"http://127.0.0.1:{self._server.server_address[1]}"

    def call(self, method: str, path: str, body: dict | None = None) -> tuple[int, dict]:
        data = None if body is None else json.dumps(body).encode("utf-8")
        request = urllib.request.Request(self.base + path, data=data, method=method)
        if data is not None:
            request.add_header("Content-Type", "application/json")
        try:
            with urllib.request.urlopen(request, timeout=10) as response:
                return response.status, json.loads(response.read().decode("utf-8"))
        except urllib.error.HTTPError as error:
            return error.code, json.loads(error.read().decode("utf-8"))

    def call_raw(self, method: str, path: str, raw: bytes) -> tuple[int, dict]:
        request = urllib.request.Request(self.base + path, data=raw, method=method)
        request.add_header("Content-Type", "application/json")
        try:
            with urllib.request.urlopen(request, timeout=10) as response:
                return response.status, json.loads(response.read().decode("utf-8"))
        except urllib.error.HTTPError as error:
            return error.code, json.loads(error.read().decode("utf-8"))

    def close(self) -> None:
        self._server.shutdown()
        self._server.server_close()


def check_gateway(server: Server) -> None:
    section("GET /api/gateways/{gateway_id}")

    status, body = server.call("GET", f"/api/gateways/{GATEWAY_ID}")
    print(f"  {status} transports={body['supported_transports']} protocols={body['supported_protocols']}")
    assert status == 200
    for key in (
        "gateway_id",
        "name",
        "status",
        "hardware_family",
        "firmware_version",
        "supported_transports",
        "supported_protocols",
        "last_seen",
    ):
        assert key in body, key
    assert body["supported_transports"] == ["ble"]
    assert body["supported_protocols"] == ["tiny_iprint"]

    status, body = server.call("GET", "/api/gateways/gw-does-not-exist")
    assert status == 404 and body["error"] == "GATEWAY_NOT_FOUND", body
    print("  unknown gateway -> 404 GATEWAY_NOT_FOUND")

    status, body = server.call("GET", "/api/health")
    assert status == 200 and body["status"] == "ok"


def check_x18(server: Server) -> dict:
    section("X18: create -> scan -> test -> complete over HTTP")

    status, case = server.call(
        "POST",
        CASE_PATH,
        {
            "gateway_id": GATEWAY_ID,
            "brand": "爱记星",
            "model": "X18",
            "original_app": "爱记星",
            "connection_hint": "bluetooth",
            "device_key": "x18",
        },
    )
    assert status == 200, case
    assert case["status"] == engine.CREATED
    case_id = case["case_id"]

    status, decision = server.call("POST", f"{CASE_PATH}/{case_id}/scan")
    assert status == 200, decision
    print(
        f"  scan: {decision['match_source']} {decision['candidate_profile']} "
        f"{decision['candidate_transport']}/{decision['candidate_protocol']} "
        f"{decision['status']} next={decision['next_action']}"
    )
    assert decision["match_source"] == "exact"
    assert decision["candidate_profile"] == "x18_aijixing"
    assert decision["capability_decision"]["path"] == capability.PATH_PROFILE_ONLY
    assert decision["requires_firmware_update"] is False
    assert decision["status"] == engine.READY_FOR_TEST
    assert decision["recommended_safe_test"]["safe_test_id"] == "SAFE_TEST_TINY_STATUS_V1"
    assert decision["recommended_safe_test"]["executable"] is True

    status, tested = server.call(
        "POST", f"{CASE_PATH}/{case_id}/test", {"outcome": "pass"}
    )
    assert status == 200, tested
    assert tested["status"] == engine.TEST_PASSED
    assert tested["safe_test_run"]["simulated"] is True
    print(f"  test: {tested['safe_test_run']['safe_test_id']} -> {tested['status']}")

    status, completed = server.call("POST", f"{CASE_PATH}/{case_id}/complete")
    assert status == 200, completed
    assert completed["status"] == engine.COMPLETED
    assert completed["printer_id"].startswith("prn-")
    print(f"  complete: printer_id={completed['printer_id']} profile={completed['printer']['profile_id']}")

    status, fetched = server.call("GET", f"{CASE_PATH}/{case_id}")
    assert status == 200
    assert fetched["status"] == engine.COMPLETED
    assert fetched["printer_id"] == completed["printer_id"]
    # Field preservation: None and booleans must survive serialisation.
    assert "minimum_firmware_version" in fetched and fetched["minimum_firmware_version"] is None
    assert fetched["firmware_available"] is True
    assert fetched["minimum_firmware_simulated"] is False
    assert fetched["recommended_safe_test"]["executable"] is True
    assert fetched["next_action"] in (engine.NEXT_NONE, engine.NEXT_DEPLOY_PROFILE)
    assert fetched["compatibility_status"] in (
        engine.COMPAT_SUPPORTED,
        engine.COMPAT_SUPPORTED_AFTER_PROFILE,
    )
    assert fetched["case_id"] == case_id

    # A completed case refuses further actions with 409, not 500.
    status, body = server.call("POST", f"{CASE_PATH}/{case_id}/scan")
    assert status == 409 and body["error"] == "INVALID_TRANSITION", body
    return fetched


def check_connection_hint(server: Server) -> None:
    section("connection_hint is stored, never turned into a transport")

    status, case = server.call(
        "POST",
        CASE_PATH,
        {
            "gateway_id": GATEWAY_ID,
            "connection_hint": "wifi",
            "raw_fingerprint": {
                "name": "X18-47E3",
                "advertised_services": ["AF30"],
                "gatt_services": ["AE30"],
                "write_characteristics": ["AE01"],
                "notify_characteristics": ["AE02"],
            },
        },
    )
    assert status == 200, case
    assert case["connection_hint"] == "wifi"

    status, decision = server.call("POST", f"{CASE_PATH}/{case['case_id']}/scan")
    assert status == 200, decision
    normalized = decision["normalized_fingerprint"]
    print(
        f"  hint=wifi -> transport={normalized['transport']} "
        f"({normalized['transport_source']}) profile={decision['candidate_profile']}"
    )
    # Discovery data (BLE GATT) decides, not the user's answer.
    assert normalized["transport"] == "ble"
    assert normalized["transport_source"] == "inferred_from_gatt"
    assert decision["candidate_transport"] == "ble"
    assert decision["candidate_profile"] == "x18_aijixing"

    status, fetched = server.call("GET", f"{CASE_PATH}/{case['case_id']}")
    assert status == 200 and fetched["connection_hint"] == "wifi"


def check_g2d(server: Server) -> dict:
    section("G2D: WAITING_FIRMWARE, safe test defined but not executable")

    status, case = server.call(
        "POST", CASE_PATH, {"gateway_id": GATEWAY_ID, "device_key": "g2d"}
    )
    assert status == 200, case
    case_id = case["case_id"]

    status, decision = server.call("POST", f"{CASE_PATH}/{case_id}/scan")
    assert status == 200, decision
    print(
        f"  scan: {decision['candidate_transport']}/{decision['candidate_protocol']} "
        f"status={decision['status']} next={decision['next_action']} "
        f"firmware_available={decision['firmware_available']}"
    )
    assert decision["candidate_profile"] == "memobird_g2d"
    assert decision["candidate_transport"] == "bluetooth_classic"
    assert decision["candidate_protocol"] == "memobird"
    assert decision["status"] == engine.WAITING_FIRMWARE
    assert decision["requires_firmware_update"] is True
    assert decision["firmware_available"] is False
    assert decision["minimum_firmware_version"] is None
    assert decision["next_action"] == engine.NEXT_WAIT_FIRMWARE
    assert decision["recommended_safe_test"]["safe_test_id"] == "SAFE_TEST_MEMOBIRD_INFO_V1"
    assert decision["recommended_safe_test"]["executable"] is False

    status, body = server.call("POST", f"{CASE_PATH}/{case_id}/test", {"outcome": "pass"})
    assert status == 409 and body["error"] == "INVALID_TRANSITION", body
    assert body["case_id"] == case_id
    print("  /test -> 409 INVALID_TRANSITION (not executable while firmware missing)")

    status, body = server.call("POST", f"{CASE_PATH}/{case_id}/complete")
    assert status == 409 and body["error"] == "INVALID_TRANSITION", body
    print("  /complete -> 409 INVALID_TRANSITION")

    # Simulated firmware must be refused on the production-default server.
    status, body = server.call(
        "POST",
        f"{CASE_PATH}/{case_id}/refresh-capability",
        {"include_simulated_firmware": True},
    )
    assert status == 403 and body["error"] == "SIMULATION_DISABLED", body
    print("  refresh with simulated firmware -> 403 SIMULATION_DISABLED")
    return decision


def check_g2d_simulated(waiting_case: dict) -> None:
    section("G2D + simulated firmware (fixture, opt-in server)")

    conn = db.connect()
    try:
        simulated_firmware.install(conn)
    finally:
        conn.close()

    demo = Server(allow_simulated_firmware=True)
    try:
        case_id = waiting_case["case_id"]
        status, refreshed = demo.call(
            "POST",
            f"{CASE_PATH}/{case_id}/refresh-capability",
            {"include_simulated_firmware": True},
        )
        assert status == 200, refreshed
        print(
            f"  refresh: min={refreshed['minimum_firmware_version']}"
            f"/{refreshed['firmware_channel']} simulated={refreshed['minimum_firmware_simulated']} "
            f"next={refreshed['next_action']}"
        )
        assert refreshed["firmware_available"] is True
        assert refreshed["minimum_firmware_version"] == "1.3.0-beta.1"
        assert refreshed["firmware_channel"] == "beta"
        assert refreshed["minimum_firmware_simulated"] is True
        assert refreshed["status"] == engine.WAITING_FIRMWARE
        assert refreshed["next_action"] == engine.NEXT_UPDATE_FIRMWARE
        assert refreshed["safe_test_executable"] is False

        # Default flag on the same opt-in server still ignores the fixture.
        status, default_view = demo.call("POST", f"{CASE_PATH}/{case_id}/refresh-capability")
        assert status == 200
        assert default_view["firmware_available"] is False
        print("  default refresh still ignores the fixture")
    finally:
        demo.close()

    conn = db.connect()
    try:
        simulated_firmware.uninstall(conn)
        assert simulated_firmware.installed(conn) is False
        # Emulate the finished gateway update (capability report, not a firmware claim).
        for capability_type, name in (
            ("transport", "bluetooth_classic"),
            ("protocol", "memobird"),
        ):
            conn.execute(
                "INSERT OR IGNORE INTO gateway_capabilities (gateway_id,"
                " capability_type, capability_name, version) VALUES (?,?,?,?)",
                (GATEWAY_ID, capability_type, name, 1),
            )
        conn.commit()
        runs_before = conn.execute(
            "SELECT COUNT(*) AS n FROM safe_test_runs WHERE case_id = ?",
            (waiting_case["case_id"],),
        ).fetchone()["n"]
    finally:
        conn.close()

    production = Server()
    try:
        status, updated = production.call(
            "POST", f"{CASE_PATH}/{waiting_case['case_id']}/refresh-capability"
        )
        assert status == 200, updated
        assert updated["requires_firmware_update"] is False
        assert updated["status"] == engine.READY_FOR_TEST
        assert updated["next_action"] == engine.NEXT_DEPLOY_PROFILE
        assert updated["safe_test_executable"] is True
        print(
            f"  after gateway update: {updated['status']} next={updated['next_action']} "
            f"executable={updated['safe_test_executable']}"
        )
    finally:
        production.close()

    conn = db.connect()
    try:
        runs_after = conn.execute(
            "SELECT COUNT(*) AS n FROM safe_test_runs WHERE case_id = ?",
            (waiting_case["case_id"],),
        ).fetchone()["n"]
    finally:
        conn.close()
    assert runs_after == runs_before == 0, "no Memobird safe test may actually run"
    print("  CMD 0x01 never executed (safe_test_runs unchanged)")


def check_unknown(server: Server) -> None:
    section("Unknown device: BLOCKED, no safe test, actions refused")

    status, case = server.call(
        "POST", CASE_PATH, {"gateway_id": GATEWAY_ID, "brand": "XP", "model": "XP-460B"}
    )
    assert status == 200, case
    case_id = case["case_id"]

    status, decision = server.call("POST", f"{CASE_PATH}/{case_id}/scan")
    assert status == 200, decision
    print(
        f"  scan: protocol={decision['candidate_protocol']} "
        f"status={decision['status']} next={decision['next_action']}"
    )
    assert decision["status"] == engine.BLOCKED
    assert decision["candidate_protocol"] == "unknown"
    assert decision["recommended_safe_test"] is None
    assert decision["next_action"] == engine.NEXT_NONE

    for action in ("test", "complete"):
        status, body = server.call("POST", f"{CASE_PATH}/{case_id}/{action}")
        assert status == 409 and body["error"] == "INVALID_TRANSITION", (action, body)
    print("  /test and /complete -> 409")


def check_error_mapping(server: Server) -> None:
    section("Error mapping")

    status, body = server.call("GET", f"{CASE_PATH}/case-missing")
    assert status == 404 and body["error"] == "CASE_NOT_FOUND", body

    status, body = server.call("POST", f"{CASE_PATH}/case-missing/scan")
    assert status == 404 and body["error"] == "CASE_NOT_FOUND" and body["case_id"] == "case-missing", body

    status, body = server.call_raw("POST", CASE_PATH, b"{not json")
    assert status == 400 and body["error"] == "INVALID_REQUEST", body

    status, body = server.call(
        "POST", CASE_PATH, {"gateway_id": GATEWAY_ID, "connection_hint": "carrier-pigeon"}
    )
    assert status == 400 and body["error"] == "INVALID_REQUEST", body

    # The old transport-shaped field is gone: it must not creep back in.
    status, body = server.call(
        "POST", CASE_PATH, {"gateway_id": GATEWAY_ID, "expected_connection_type": "wifi"}
    )
    assert status == 400 and body["error"] == "INVALID_REQUEST", body
    print("  user connection input cannot be sent as a transport")

    status, body = server.call("POST", CASE_PATH, {"brand": "x"})
    assert status == 400 and body["error"] == "INVALID_REQUEST", body

    status, case = server.call("POST", CASE_PATH, {"gateway_id": GATEWAY_ID, "device_key": "x18"})
    case_id = case["case_id"]
    # Arbitrary payloads must not be accepted by /test.
    status, body = server.call(
        "POST", f"{CASE_PATH}/{case_id}/test", {"command": "0x01"}
    )
    assert status == 400 and body["error"] == "INVALID_REQUEST", body
    print("  extra/arbitrary fields rejected with 400 INVALID_REQUEST")

    status, body = server.call("GET", f"{CASE_PATH}/{case_id}/scan")
    assert status == 405 and body["error"] == "METHOD_NOT_ALLOWED", body
    print("  wrong method -> 405 METHOD_NOT_ALLOWED")

    status, body = server.call("GET", "/api/nope")
    assert status == 404 and body["error"] == "NOT_FOUND", body
    print("  404 / 400 / 405 all use the uniform error body")


def main() -> int:
    db.initialize()
    server = Server()
    try:
        check_gateway(server)
        check_x18(server)
        check_connection_hint(server)
        waiting = check_g2d(server)
        check_unknown(server)
        check_error_mapping(server)
    finally:
        server.close()

    check_g2d_simulated(waiting)

    conn = db.connect()
    try:
        counts = {
            table: conn.execute(f"SELECT COUNT(*) AS n FROM {table}").fetchone()["n"]
            for table in (
                "compatibility_cases",
                "fingerprints",
                "safe_test_runs",
                "printer_instances",
            )
        }
    finally:
        conn.close()
    print(f"\nrows: {counts}")
    print("\nPASS: API V0.1 (X18, G2D, simulated firmware isolation, unknown, error mapping)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

"""Host tests for the compatibility engine.

No hardware, no network, no printing: the simulator provides the radio data and
the rule-based analyst (no RTB_AI_ENDPOINT) decides Stage C.

Usage: python tests/test_gateway_engine.py
"""

from __future__ import annotations

import json
import os
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
os.environ["RTB_GATEWAY_DB"] = str(Path(tempfile.mkdtemp(prefix="rtb-engine-")) / "gateway.db")
# Force the deterministic analyst: an ambient endpoint would make this flaky.
os.environ.pop("RTB_AI_ENDPOINT", None)
os.environ.pop("RTB_AI_API_KEY", None)

from gateway import (  # noqa: E402  (import after the DB path is pinned)
    ai_analyst,
    canonical,
    capability,
    config,
    db,
    engine,
    fingerprint,
    matcher,
    safe_tests,
    simulated_firmware,
    simulator,
)

PROFILE_DIR = ROOT / "printers" / "profiles"
GATEWAY_PROFILE_DIR = ROOT / "gateway" / "profiles"
GATEWAY_ID = db.GATEWAY["gateway_id"]


def section(title: str) -> None:
    print(f"\n== {title} ==")


def check_canonical_vocabulary(conn) -> None:
    section("canonical vocabulary (no drift between modules)")

    assert canonical.normalize_transport("BLE") == "ble"
    assert canonical.normalize_transport("BT_CLASSIC") == "bluetooth_classic"
    assert canonical.normalize_transport("Bluetooth Classic") == "bluetooth_classic"
    assert canonical.normalize_transport("SPP") == "bluetooth_classic"
    assert canonical.normalize_transport("Wi-Fi") == "tcp"
    assert canonical.normalize_transport("nonsense") == "unknown"
    assert fingerprint.normalize_transport("BT_CLASSIC") == "bluetooth_classic"

    for device in simulator.list_devices():
        transport = device["raw"]["transport"]
        assert canonical.is_canonical_transport(transport), device["key"]

    for cap_type, cap_name, _ in db.GATEWAY_CAPABILITIES:
        if cap_type == "transport":
            assert canonical.is_canonical_transport(cap_name), cap_name
        if cap_type == "protocol":
            assert canonical.is_canonical_protocol(cap_name), cap_name

    for version, _family, _channel, caps, _status, _released in db.FIRMWARE_RELEASES:
        for transport in caps["transports"]:
            assert canonical.is_canonical_transport(transport), (version, transport)
        for protocol in caps["protocols"]:
            assert canonical.is_canonical_protocol(protocol), (version, protocol)

    rows = conn.execute("SELECT transport, protocol FROM printer_profiles").fetchall()
    assert rows
    for row in rows:
        assert canonical.is_canonical_transport(row["transport"]), row["transport"]
        assert canonical.is_canonical_protocol(row["protocol"]), row["protocol"]

    rules = conn.execute("SELECT fingerprint_rules_json FROM printer_profiles").fetchall()
    for row in rules:
        transport = json.loads(row["fingerprint_rules_json"])["transport"]
        assert canonical.is_canonical_transport(transport), transport

    for path in sorted(PROFILE_DIR.glob("*.json")) + sorted(GATEWAY_PROFILE_DIR.glob("*.json")):
        data = json.loads(path.read_text(encoding="utf-8"))
        for transport in data["supported_transports"]:
            assert canonical.is_canonical_transport(transport), (path.name, transport)
        assert canonical.is_canonical_protocol(data["protocol_family"]), path.name

    for spec in safe_tests.REGISTRY.values():
        assert canonical.is_canonical_transport(spec.transport), spec.safe_test_id
        assert canonical.is_canonical_protocol(spec.protocol), spec.safe_test_id

    memobird_test = safe_tests.get("SAFE_TEST_MEMOBIRD_INFO_V1")
    assert memobird_test.transport == "bluetooth_classic"
    assert memobird_test.protocol == "memobird"
    assert memobird_test.command == "CMD 0x01"

    # Stage C evidence: SPP alone is not a protocol family, the name decides.
    analyst = ai_analyst.RuleBasedAnalyst()
    memobird = analyst.analyze(
        ai_analyst.AnalysisContext(
            brand="MEMOBIRD",
            model="G2D",
            fingerprint={"spp_uuids": ["1101"], "name": "G2D", "service_uuids": []},
            candidates=[],
        )
    )
    assert memobird.candidate_protocol == "memobird", memobird.model_dump()
    assert memobird.action == "PROTOCOL_FAMILY"
    generic = analyst.analyze(
        ai_analyst.AnalysisContext(
            fingerprint={"spp_uuids": ["1101"], "name": "POS-58", "service_uuids": []},
            candidates=[],
        )
    )
    assert generic.candidate_protocol == "escpos", generic.model_dump()

    assert set(ai_analyst.PROTOCOLS) == set(canonical.PROTOCOLS)
    for protocol in canonical.PROTOCOLS:
        ai_analyst.AIAnalysis(
            candidate_protocol=protocol,
            confidence=0.5,
            reason_codes=["no_candidate"],
            action="UNSUPPORTED",
        )
    print(f"  {len(canonical.TRANSPORTS)} transports, {len(canonical.PROTOCOLS)} protocols, all modules agree")


def check_x18(conn) -> dict:
    section("A. X18: known answer, must be PROFILE_ONLY (never a firmware update)")

    case = engine.create_case(conn, GATEWAY_ID, device_key="x18")
    assert case["status"] == engine.CREATED
    assert case["scan_source"]["kind"] == "simulator"

    decision = engine.scan_case(conn, case["case_id"])
    print(
        f"  match={decision['match_source']} profile={decision['candidate_profile']} "
        f"transport={decision['candidate_transport']} protocol={decision['candidate_protocol']} "
        f"status={decision['status']} next={decision['next_action']}"
    )

    assert decision["match_source"] == "exact"
    assert decision["candidate_profile"] == "x18_aijixing"
    assert decision["candidate_transport"] == "ble"
    assert decision["candidate_protocol"] == "tiny_iprint"
    assert decision["normalized_fingerprint"]["transport"] == "ble"
    assert decision["requires_firmware_update"] is False
    assert decision["minimum_firmware_version"] is None
    assert decision["capability_decision"]["path"] == capability.PATH_PROFILE_ONLY
    assert decision["compatibility_status"] in (
        engine.COMPAT_SUPPORTED,
        engine.COMPAT_SUPPORTED_AFTER_PROFILE,
    )
    assert decision["recommended_safe_test"]["safe_test_id"] == "SAFE_TEST_TINY_STATUS_V1"
    assert decision["recommended_safe_test"]["executable"] is True
    assert decision["safe_test_executable"] is True
    assert decision["status"] == engine.READY_FOR_TEST
    assert decision["print_implemented"] is True

    tested = engine.run_test(conn, case["case_id"])
    assert tested["status"] == engine.TEST_PASSED
    assert tested["safe_test_run"]["simulated"] is True
    print(f"  safe test {tested['safe_test_run']['safe_test_id']} -> {tested['status']} (simulated)")

    completed = engine.complete_case(conn, case["case_id"])
    assert completed["status"] == engine.COMPLETED
    assert completed["printer_id"].startswith("prn-")

    row = conn.execute(
        "SELECT * FROM printer_instances WHERE printer_id = ?", (completed["printer_id"],)
    ).fetchone()
    assert row is not None and row["profile_id"] == "x18_aijixing" and row["case_id"] == case["case_id"]
    print(f"  printer instance {row['printer_id']} profile={row['profile_id']} address={row['physical_address']}")
    return completed


def check_g2d(conn) -> dict:
    section("B. G2D: Bluetooth Classic SPP carrying the Memobird Classic Protocol")

    case = engine.create_case(conn, GATEWAY_ID, device_key="g2d")
    decision = engine.scan_case(conn, case["case_id"])
    print(
        f"  match={decision['match_source']} profile={decision['candidate_profile']} "
        f"transport={decision['candidate_transport']} protocol={decision['candidate_protocol']} "
        f"status={decision['status']} next={decision['next_action']} "
        f"firmware_available={decision['firmware_available']}"
    )

    assert decision["normalized_fingerprint"]["transport"] == "bluetooth_classic"
    assert decision["candidate_profile"] == "memobird_g2d"
    assert decision["candidate_transport"] == "bluetooth_classic"
    assert decision["candidate_protocol"] == "memobird"
    assert decision["match_source"] in ("exact", "similarity", "ai")

    # SPP is only the transport; the gateway has no Memobird implementation.
    assert decision["requires_firmware_update"] is True
    assert decision["firmware_available"] is False
    assert decision["minimum_firmware_version"] is None
    assert decision["capability_decision"]["path"] == capability.PATH_BLOCKED
    assert "no_firmware_release_provides_capability" in decision["capability_decision"]["reason_codes"]
    assert decision["status"] == engine.WAITING_FIRMWARE
    assert decision["compatibility_status"] == engine.COMPAT_NEEDS_FIRMWARE_UNAVAILABLE
    assert decision["next_action"] == engine.NEXT_WAIT_FIRMWARE

    safe_test = decision["recommended_safe_test"]
    assert safe_test["safe_test_id"] == "SAFE_TEST_MEMOBIRD_INFO_V1"
    assert safe_test["transport"] == "bluetooth_classic"
    assert safe_test["protocol"] == "memobird"
    assert safe_test["command"] == "CMD 0x01"
    assert safe_test["executable"] is False
    assert decision["safe_test_executable"] is False
    assert decision["print_implemented"] is False

    profile = conn.execute(
        "SELECT * FROM printer_profiles WHERE profile_id = 'memobird_g2d'"
    ).fetchone()
    assert profile is not None
    assert profile["status"] == "EXPERIMENTAL", "G2D is static analysis only"
    assert profile["status"] not in ("VERIFIED", "SUPPORTED")
    assert profile["source"].startswith("gateway/profiles/")

    # No shipped firmware may claim Memobird support (simulated rows excluded).
    for row in conn.execute(
        "SELECT firmware_version, capabilities_json FROM firmware_releases WHERE simulated = 0"
    ).fetchall():
        caps = json.loads(row["capabilities_json"])
        assert "memobird" not in caps.get("protocols", []), row["firmware_version"]

    try:
        engine.run_test(conn, case["case_id"])
    except (engine.InvalidTransition, engine.EngineError):
        print("  safe test is defined but not executable: refused")
    else:
        raise AssertionError("an unsimulated Memobird test must not run")

    return decision


def check_g2d_simulated_firmware(conn, waiting_case: dict) -> None:
    section("B2. G2D with a SIMULATED 1.3.0-beta.1 (fixture only, never shipped)")

    simulated_firmware.install(conn)

    # A case scanned while asking for simulated firmware sees the beta release.
    case = engine.create_case(conn, GATEWAY_ID, device_key="g2d")
    decision = engine.scan_case(conn, case["case_id"], include_simulated_firmware=True)
    print(
        f"  match={decision['match_source']} profile={decision['candidate_profile']} "
        f"protocol={decision['candidate_protocol']} min={decision['minimum_firmware_version']}"
        f"/{decision['firmware_channel']} simulated={decision['minimum_firmware_simulated']} "
        f"next={decision['next_action']}"
    )
    assert decision["candidate_profile"] == "memobird_g2d"
    assert decision["candidate_protocol"] == "memobird"
    assert decision["requires_firmware_update"] is True
    assert decision["firmware_available"] is True
    assert decision["minimum_firmware_version"] == "1.3.0-beta.1"
    assert decision["firmware_channel"] == "beta"
    assert decision["minimum_firmware_simulated"] is True
    assert decision["next_action"] == engine.NEXT_UPDATE_FIRMWARE
    assert decision["safe_test_executable"] is False

    # Isolation: the same fixture is invisible without the explicit flag.
    isolated = engine.create_case(conn, GATEWAY_ID, device_key="g2d")
    isolated_decision = engine.scan_case(conn, isolated["case_id"])
    assert isolated_decision["firmware_available"] is False
    assert isolated_decision["minimum_firmware_version"] is None
    assert isolated_decision["status"] == engine.WAITING_FIRMWARE
    print("  default resolve ignores the simulated release")

    # Re-resolving the earlier waiting case now knows a beta release exists.
    refreshed = engine.refresh_capability(
        conn, waiting_case["case_id"], include_simulated_firmware=True
    )
    assert refreshed["firmware_available"] is True
    assert refreshed["minimum_firmware_version"] == "1.3.0-beta.1"
    assert refreshed["minimum_firmware_simulated"] is True
    assert refreshed["status"] == engine.WAITING_FIRMWARE
    assert refreshed["next_action"] == engine.NEXT_UPDATE_FIRMWARE
    print("  waiting case re-resolved: beta firmware exists, update still required")

    simulated_firmware.uninstall(conn)
    assert simulated_firmware.installed(conn) is False

    # Emulate the completed gateway update (not a firmware claim: the gateway
    # simply reports the capabilities the update gave it).
    conn.execute(
        "INSERT OR IGNORE INTO gateway_capabilities (gateway_id, capability_type,"
        " capability_name, version) VALUES (?,?,?,?)",
        (GATEWAY_ID, "transport", "bluetooth_classic", 1),
    )
    conn.execute(
        "INSERT OR IGNORE INTO gateway_capabilities (gateway_id, capability_type,"
        " capability_name, version) VALUES (?,?,?,?)",
        (GATEWAY_ID, "protocol", "memobird", 1),
    )
    conn.commit()

    after_update = engine.refresh_capability(conn, waiting_case["case_id"])
    print(
        f"  after gateway update: status={after_update['status']} "
        f"next={after_update['next_action']} executable={after_update['safe_test_executable']}"
    )
    assert after_update["requires_firmware_update"] is False
    assert after_update["status"] == engine.READY_FOR_TEST
    assert after_update["next_action"] == engine.NEXT_DEPLOY_PROFILE
    assert after_update["safe_test_executable"] is True
    assert conn.execute(
        "SELECT COUNT(*) AS n FROM safe_test_runs WHERE case_id = ?",
        (waiting_case["case_id"],),
    ).fetchone()["n"] == 0


def check_xp460b(conn) -> dict:
    section("C. XP-460B: unsupported device must stay BLOCKED (no invented candidate)")

    case = engine.create_case(conn, GATEWAY_ID, brand="XP", model="XP-460B")
    assert case["scan_source"]["device_key"] == "unknown"
    decision = engine.scan_case(conn, case["case_id"])
    print(
        f"  match={decision['match_source']} protocol={decision['candidate_protocol']} "
        f"profile={decision['candidate_profile']} confidence={decision['confidence']} "
        f"status={decision['status']} next={decision['next_action']}"
    )

    assert decision["normalized_fingerprint"]["transport"] == "ble"
    assert decision["match_source"] == "ai"
    assert decision["candidate_profile"] is None
    assert decision["candidate_protocol"] == "unknown"
    assert decision["status"] == engine.BLOCKED
    assert decision["compatibility_status"] == engine.COMPAT_BLOCKED
    assert decision["recommended_safe_test"] is None

    try:
        engine.run_test(conn, case["case_id"])
    except engine.InvalidTransition:
        pass
    else:
        raise AssertionError("a BLOCKED case must not run a safe test")
    print("  no candidate, no safe test: BLOCKED")
    return decision


def check_unknown(conn) -> dict:
    section("D. Unknown fingerprint supplied as raw data")

    case = engine.create_case(
        conn,
        GATEWAY_ID,
        brand="NoName",
        model="ZZ-9",
        raw={
            "name": "ZZ-9",
            "transport": "ble",
            "rssi": -70,
            "address": "aa:bb:cc:dd:ee:ff",
            "gatt_services": ["1800"],
            "write_characteristics": ["2a00"],
        },
    )
    assert case["scan_source"]["kind"] == "raw"
    decision = engine.scan_case(conn, case["case_id"])
    print(
        f"  match={decision['match_source']} protocol={decision['candidate_protocol']} "
        f"confidence={decision['confidence']} status={decision['status']} "
        f"next={decision['next_action']}"
    )

    assert decision["normalized_fingerprint"]["transport"] == "ble"
    assert decision["candidate_profile"] is None
    assert decision["confidence"] < config.AI_MIN_CONFIDENCE
    assert decision["status"] in (engine.BLOCKED, engine.NEEDS_REVIEW)
    assert decision["next_action"] in (engine.NEXT_NONE, engine.NEXT_MANUAL_REVIEW)
    assert decision["recommended_safe_test"] is None
    return decision


def check_trace(conn, x18_case: dict) -> None:
    section("E. traceability: raw + normalised fingerprint, matcher, AI, capability, safe test, printer")

    case_id = x18_case["case_id"]
    row = conn.execute(
        "SELECT * FROM compatibility_cases WHERE case_id = ?", (case_id,)
    ).fetchone()
    stored = json.loads(row["result_json"])
    assert row["status"] == engine.COMPLETED
    assert row["fingerprint_id"]
    assert stored["capability_decision"]["path"] == capability.PATH_PROFILE_ONLY
    assert stored["recommended_safe_test"]["safe_test_id"] == "SAFE_TEST_TINY_STATUS_V1"
    assert stored["normalized_fingerprint"]["service_uuids"] == ["ae30"]
    assert stored["ai_used"] is False
    assert stored["printer_id"] == x18_case["printer_id"]
    assert stored["candidates"], "matcher evidence must be stored"

    fp = conn.execute(
        "SELECT * FROM fingerprints WHERE fingerprint_id = ?", (row["fingerprint_id"],)
    ).fetchone()
    assert json.loads(fp["raw_data_json"])["name"] == "X18-47E3"
    assert json.loads(fp["normalized_data_json"])["transport"] == "ble"

    run = conn.execute(
        "SELECT * FROM safe_test_runs WHERE case_id = ?", (case_id,)
    ).fetchone()
    assert run is not None and run["status"] == "passed"
    assert json.loads(run["detail_json"])["simulated"] is True

    g2d_case = engine.create_case(conn, GATEWAY_ID, device_key="g2d")
    engine.scan_case(conn, g2d_case["case_id"])
    try:
        engine.scan_case(conn, g2d_case["case_id"])
    except engine.InvalidTransition:
        pass
    else:
        raise AssertionError("a scanned case must not be scanned twice")
    print("  case row, fingerprint row, safe test run and printer row all linked")
    print("  lifecycle guard: rescanning a SCANNED/ANALYZED case is refused")


def main() -> int:
    db.initialize()
    conn = db.connect()

    check_canonical_vocabulary(conn)
    x18 = check_x18(conn)
    g2d = check_g2d(conn)
    check_g2d_simulated_firmware(conn, g2d)
    check_xp460b(conn)
    check_unknown(conn)
    check_trace(conn, x18)

    counts = {
        table: conn.execute(f"SELECT COUNT(*) AS n FROM {table}").fetchone()["n"]
        for table in ("compatibility_cases", "fingerprints", "safe_test_runs", "printer_instances")
    }
    print(f"\nrows: {counts}")
    print("\nPASS: 4/4 acceptance cases + canonical vocabulary + traceability")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

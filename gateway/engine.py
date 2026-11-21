"""Compatibility Engine: the one place where a case is decided.

Pipeline (V0.1, no hardware):

    simulator / raw fingerprint
      -> fingerprint.normalize_fingerprint
      -> matcher.exact_matches (Stage A)
      -> matcher.similarity_ranking (Stage B)
      -> ai_analyst (Stage C, only when A and B cannot decide)
      -> capability.resolve (profile vs firmware)
      -> safe_tests.recommend (whitelisted, read-only)
      -> compatibility case state + trace
      -> final decision

Everything the UI or a future HTTP layer needs is in the returned dict; neither
of them may re-run a matcher, an analyst or a capability decision. The HTTP
layer (not written yet) is transport only.

Every state change goes through ``_set_state`` so the allowed transitions live
here and nowhere else.
"""

from __future__ import annotations

import json
import sqlite3
import uuid
from typing import Any

from . import (
    adapters,
    ai_analyst,
    capability,
    config,
    db,
    fingerprint,
    matcher,
    safe_tests,
    simulator,
)
from .canonical import normalize_protocol, normalize_transport

# Lifecycle states of a compatibility case.
CREATED = "CREATED"
SCANNED = "SCANNED"
MATCHED = "MATCHED"
ANALYZED = "ANALYZED"
WAITING_FIRMWARE = "WAITING_FIRMWARE"
READY_FOR_TEST = "READY_FOR_TEST"
TEST_PASSED = "TEST_PASSED"
TEST_FAILED = "TEST_FAILED"
BLOCKED = "BLOCKED"
NEEDS_REVIEW = "NEEDS_REVIEW"
COMPLETED = "COMPLETED"

STATUSES: tuple[str, ...] = (
    CREATED,
    SCANNED,
    MATCHED,
    ANALYZED,
    WAITING_FIRMWARE,
    READY_FOR_TEST,
    TEST_PASSED,
    TEST_FAILED,
    BLOCKED,
    NEEDS_REVIEW,
    COMPLETED,
)

# WAITING_FIRMWARE is not a dead end: once a release exists (or the gateway is
# actually updated) the case is re-resolved with refresh_capability(). BLOCKED
# stays reserved for "no credible path at all".
_TRANSITIONS: dict[str, frozenset[str]] = {
    CREATED: frozenset({SCANNED, BLOCKED}),
    SCANNED: frozenset({MATCHED, BLOCKED, NEEDS_REVIEW}),
    MATCHED: frozenset({ANALYZED, WAITING_FIRMWARE, READY_FOR_TEST, BLOCKED, NEEDS_REVIEW}),
    ANALYZED: frozenset({WAITING_FIRMWARE, READY_FOR_TEST, BLOCKED, NEEDS_REVIEW}),
    WAITING_FIRMWARE: frozenset({READY_FOR_TEST, BLOCKED}),
    READY_FOR_TEST: frozenset({TEST_PASSED, TEST_FAILED, BLOCKED}),
    TEST_PASSED: frozenset({COMPLETED}),
    TEST_FAILED: frozenset({READY_FOR_TEST, BLOCKED}),
    NEEDS_REVIEW: frozenset({BLOCKED, READY_FOR_TEST}),
    BLOCKED: frozenset(),
    COMPLETED: frozenset(),
}

# compatibility_status: what the product answer actually is.
COMPAT_SUPPORTED = "SUPPORTED"
COMPAT_SUPPORTED_AFTER_PROFILE = "SUPPORTED_AFTER_PROFILE_DEPLOY"
COMPAT_NEEDS_FIRMWARE = "NEEDS_FIRMWARE"
COMPAT_NEEDS_FIRMWARE_UNAVAILABLE = "NEEDS_FIRMWARE_UNAVAILABLE"
COMPAT_NEEDS_REVIEW = "NEEDS_REVIEW"
COMPAT_BLOCKED = "BLOCKED"

# next_action: the single thing to do next.
NEXT_DEPLOY_PROFILE = "DEPLOY_PROFILE"
NEXT_RUN_SAFE_TEST = "RUN_SAFE_TEST"
NEXT_UPDATE_FIRMWARE = "UPDATE_FIRMWARE"
NEXT_WAIT_FIRMWARE = "WAIT_FOR_COMPATIBILITY_UPDATE"
NEXT_MANUAL_REVIEW = "MANUAL_REVIEW"
NEXT_NONE = "NONE"


class EngineError(RuntimeError):
    """Base class for engine contract violations."""


class CaseNotFound(EngineError, KeyError):
    def __init__(self, case_id: str) -> None:
        super().__init__(f"unknown compatibility case: {case_id}")


class InvalidTransition(EngineError):
    def __init__(self, current: str, target: str) -> None:
        super().__init__(f"illegal case transition: {current} -> {target}")


class UnknownGateway(EngineError, KeyError):
    def __init__(self, gateway_id: str) -> None:
        super().__init__(f"unknown gateway: {gateway_id}")


# --------------------------------------------------------------------------- #
# storage helpers
# --------------------------------------------------------------------------- #
def _case_row(conn: sqlite3.Connection, case_id: str) -> sqlite3.Row:
    row = conn.execute(
        "SELECT * FROM compatibility_cases WHERE case_id = ?", (case_id,)
    ).fetchone()
    if row is None:
        raise CaseNotFound(case_id)
    return row


def _gateway_row(conn: sqlite3.Connection, gateway_id: str) -> sqlite3.Row:
    row = conn.execute(
        "SELECT * FROM gateways WHERE gateway_id = ?", (gateway_id,)
    ).fetchone()
    if row is None:
        raise UnknownGateway(gateway_id)
    return row


def _report_of(row: sqlite3.Row) -> dict[str, Any]:
    if not row["result_json"]:
        return {}
    try:
        report = json.loads(row["result_json"])
    except ValueError:
        return {}
    return report if isinstance(report, dict) else {}


def _set_state(
    conn: sqlite3.Connection, case_id: str, new_status: str, report: dict[str, Any]
) -> dict[str, Any]:
    """The only writer of compatibility_cases.status."""
    row = _case_row(conn, case_id)
    current = row["status"]
    if new_status not in _TRANSITIONS.get(current, frozenset()):
        raise InvalidTransition(current, new_status)
    payload = dict(report)
    payload["status"] = new_status
    now = db.now_iso()
    conn.execute(
        "UPDATE compatibility_cases SET status = ?, result_json = ?, updated_at = ?"
        " WHERE case_id = ?",
        (new_status, json.dumps(payload, ensure_ascii=False), now, case_id),
    )
    conn.commit()
    payload["updated_at"] = now
    return payload


def _save_report(
    conn: sqlite3.Connection, case_id: str, report: dict[str, Any]
) -> dict[str, Any]:
    """Persist the trace for a case whose status does not change."""
    row = _case_row(conn, case_id)
    payload = dict(report)
    payload["status"] = row["status"]
    now = db.now_iso()
    conn.execute(
        "UPDATE compatibility_cases SET result_json = ?, updated_at = ? WHERE case_id = ?",
        (json.dumps(payload, ensure_ascii=False), now, case_id),
    )
    conn.commit()
    payload["updated_at"] = now
    return payload


def _save_fingerprint(
    conn: sqlite3.Connection,
    gateway_id: str,
    raw: dict[str, Any],
    normalized: dict[str, Any],
) -> str:
    fingerprint_id = "fp-" + uuid.uuid4().hex[:12]
    conn.execute(
        "INSERT INTO fingerprints (fingerprint_id, gateway_id, transport,"
        " raw_data_json, normalized_data_json, created_at) VALUES (?,?,?,?,?,?)",
        (
            fingerprint_id,
            gateway_id,
            normalized["transport"],
            json.dumps(raw, ensure_ascii=False),
            json.dumps(normalized, ensure_ascii=False),
            db.now_iso(),
        ),
    )
    conn.commit()
    return fingerprint_id


# --------------------------------------------------------------------------- #
# public API
# --------------------------------------------------------------------------- #
CONNECTION_HINTS = ("bluetooth", "wifi", "unknown")


def _connection_hint(value: str | None) -> str:
    """Store the user's own words; never turn them into a transport."""
    text = (value or "").strip().lower()
    return text if text in CONNECTION_HINTS else "unknown"


def create_case(
    conn: sqlite3.Connection,
    gateway_id: str,
    *,
    brand: str = "",
    model: str = "",
    original_app: str = "",
    connection_hint: str = "unknown",
    device_key: str | None = None,
    raw: dict[str, Any] | None = None,
) -> dict[str, Any]:
    """Record the user's inputs. Nothing is scanned or decided yet.

    A real gateway owns the discovery step: no fingerprint is fabricated here and
    ``device_key`` (a simulator selector) is rejected outright, so a real-mode
    acceptance run can never be short-circuited by naming the device.
    """
    gateway = _gateway_row(conn, gateway_id)
    mode = adapters.gateway_mode(gateway)

    if mode == adapters.MODE_REAL and device_key:
        raise EngineError(
            "device_key is a simulator selector; a real gateway scan must identify"
            " the printer"
        )

    if raw is not None:
        source = {"kind": "raw", "device_key": None, "raw": dict(raw)}
    elif mode == adapters.MODE_REAL:
        # Filled by scan_case() from the gateway's own scan result.
        source = {"kind": "gateway", "device_key": None, "raw": {}}
    else:
        key = simulator.resolve_device_key(brand, model, device_key)
        source = {"kind": "simulator", "device_key": key, "raw": simulator.scan(key)}

    case_id = "case-" + uuid.uuid4().hex[:12]
    now = db.now_iso()
    report: dict[str, Any] = {
        "case_id": case_id,
        "gateway_id": gateway_id,
        "user_id": gateway["user_id"],
        "brand": brand,
        "model": model,
        "original_app": original_app,
        "connection_hint": _connection_hint(connection_hint),
        "scan_source": source,
        "raw_fingerprint": source["raw"],
        "fingerprint_id": None,
        "normalized_fingerprint": None,
        "match_source": "none",
        "ai_used": False,
        "candidate_profile": None,
        "candidate_protocol": "unknown",
        "candidate_transport": "unknown",
        "confidence": 0.0,
        "reason_codes": [],
        "candidates": [],
        "capability_decision": None,
        "requires_profile_update": False,
        "requires_firmware_update": False,
        "minimum_firmware_version": None,
        "firmware_channel": None,
        "firmware_available": None,
        "minimum_firmware_simulated": False,
        "recommended_safe_test": None,
        "safe_test_executable": False,
        "safe_test_run": None,
        "compatibility_status": COMPAT_BLOCKED,
        "next_action": NEXT_NONE,
        "printer_id": None,
        "created_at": now,
        "updated_at": now,
    }
    conn.execute(
        "INSERT INTO compatibility_cases (case_id, user_id, gateway_id,"
        " fingerprint_id, brand, model, original_app, connection_hint,"
        " status, result_json, created_at, updated_at)"
        " VALUES (?,?,?,?,?,?,?,?,?,?,?,?)",
        (
            case_id,
            gateway["user_id"],
            gateway_id,
            None,
            brand,
            model,
            original_app,
            report["connection_hint"],
            CREATED,
            json.dumps(report, ensure_ascii=False),
            now,
            now,
        ),
    )
    conn.commit()
    report["status"] = CREATED
    return report


def get_case(conn: sqlite3.Connection, case_id: str) -> dict[str, Any]:
    """Stored case + decision trace, with the row's status authoritative."""
    row = _case_row(conn, case_id)
    report = _report_of(row)
    report.update(
        {
            "case_id": row["case_id"],
            "gateway_id": row["gateway_id"],
            "user_id": row["user_id"],
            "fingerprint_id": row["fingerprint_id"],
            "brand": row["brand"],
            "model": row["model"],
            "original_app": row["original_app"],
            "connection_hint": row["connection_hint"],
            "status": row["status"],
            "created_at": row["created_at"],
            "updated_at": row["updated_at"],
        }
    )
    return report


def list_gateways(conn: sqlite3.Connection) -> dict[str, Any]:
    """Every configured gateway, real ones first (the UI prefers them)."""
    rows = conn.execute(
        "SELECT gateway_id, name, mode, hardware_family, firmware_version, status"
        " FROM gateways ORDER BY CASE mode WHEN 'real' THEN 0 ELSE 1 END, gateway_id"
    ).fetchall()
    return {
        "gateways": [
            {
                "gateway_id": row["gateway_id"],
                "name": row["name"],
                "mode": adapters.gateway_mode(row),
                "hardware_family": row["hardware_family"],
                "firmware_version": row["firmware_version"],
                "status": row["status"],
            }
            for row in rows
        ]
    }


def gateway_snapshot(
    conn: sqlite3.Connection, gateway_id: str, *, adapter: Any = None
) -> dict[str, Any]:
    """Read-only view of a gateway for the UI. No decisions are taken here.

    For a real gateway the live ``/api/status`` answer overrides identity data.
    Being unreachable is reported *here* (``status=offline, reachable=false``)
    and never translated into "printer unsupported": those are different facts.
    """
    row = _gateway_row(conn, gateway_id)
    caps = conn.execute(
        "SELECT capability_type, capability_name, version FROM gateway_capabilities"
        " WHERE gateway_id = ? ORDER BY capability_type, capability_name",
        (gateway_id,),
    ).fetchall()

    transports: set[str] = set()
    protocols: set[str] = set()
    profiles: list[str] = []
    for cap in caps:
        if cap["capability_type"] == "transport":
            transports.add(normalize_transport(cap["capability_name"]))
        elif cap["capability_type"] == "protocol":
            protocols.add(normalize_protocol(cap["capability_name"]))
        elif cap["capability_type"] == "profile":
            profiles.append(cap["capability_name"])

    mode = adapters.gateway_mode(row)
    snapshot: dict[str, Any] = {
        "gateway_id": row["gateway_id"],
        "user_id": row["user_id"],
        "name": row["name"],
        "mode": mode,
        "status": row["status"],
        "reachable": True,
        "hardware_family": row["hardware_family"],
        "firmware_version": row["firmware_version"],
        "supported_transports": sorted(transports),
        "supported_protocols": sorted(protocols),
        "deployed_profiles": sorted(profiles),
        "last_seen": row["last_seen"],
        "status_source": "record",
    }

    if mode != adapters.MODE_REAL:
        return snapshot

    probe = adapter if adapter is not None else adapters.for_gateway(row)
    try:
        live = probe.status()
    except adapters.GatewayError as error:
        snapshot.update(
            {
                "status": "offline",
                "reachable": False,
                "error": _gateway_error_code(error),
                "message": str(error),
                "status_source": "probe",
            }
        )
        return snapshot

    snapshot.update(
        {
            "status": "online" if live.online else "offline",
            "hardware_family": live.hardware_family,
            "firmware_version": live.firmware_version,
            "supported_transports": sorted(
                transports.union(normalize_transport(name) for name in live.supported_transports)
            ),
            "supported_protocols": sorted(
                protocols.union(normalize_protocol(name) for name in live.supported_protocols)
            ),
            "status_source": "probe",
        }
    )
    return snapshot


def _gateway_error_code(error: adapters.GatewayError) -> str:
    if isinstance(error, adapters.GatewayOffline):
        return "GATEWAY_OFFLINE"
    if isinstance(error, adapters.GatewayTimeout):
        return "GATEWAY_TIMEOUT"
    if isinstance(error, adapters.PrinterNotFound):
        return "PRINTER_NOT_FOUND"
    return "GATEWAY_INVALID_RESPONSE"


def scan_case(
    conn: sqlite3.Connection,
    case_id: str,
    *,
    include_simulated_firmware: bool = False,
    adapter: Any = None,
) -> dict[str, Any]:
    """Run the full pipeline once for a CREATED case and store the decision.

    Discovery is delegated to the gateway adapter: a real ESP32 performs the BLE
    scan (and its GATT evidence) here, at scan time, not at case creation. If the
    gateway is offline / times out / finds nothing, the adapter raises and the
    case stays CREATED, so the user can simply retry.

    ``include_simulated_firmware`` is for the demo/test fixture only; the
    default path never sees a simulated release.
    """
    row = _case_row(conn, case_id)
    if row["status"] != CREATED:
        raise InvalidTransition(row["status"], SCANNED)

    report = _report_of(row)
    source = dict(report.get("scan_source") or {})
    gateway = _gateway_row(conn, row["gateway_id"])
    mode = adapters.gateway_mode(gateway)

    if adapter is None and mode == adapters.MODE_REAL:
        adapter = adapters.for_gateway(gateway, device_key=source.get("device_key"))

    raw = report.get("raw_fingerprint") or {}
    if adapter is not None:
        # Raises GatewayOffline / GatewayTimeout / PrinterNotFound: the case is
        # left untouched so a retry starts from a clean CREATED state.
        scan = adapter.scan()
        raw = dict(scan.raw)
        report["scan"] = {
            "gateway_id": scan.gateway_id,
            "mode": mode,
            "elapsed_ms": scan.elapsed_ms,
            "scanned": scan.scanned,
            "found": True,
        }
        source["kind"] = "gateway" if mode == adapters.MODE_REAL else "adapter"
        source["raw"] = raw
        report["scan_source"] = source
        report["raw_fingerprint"] = raw

    # Discovery data only: the user's connection_hint stays on the case.
    normalized = fingerprint.normalize_fingerprint(raw)
    fingerprint_id = _save_fingerprint(conn, row["gateway_id"], raw, normalized)
    conn.execute(
        "UPDATE compatibility_cases SET fingerprint_id = ? WHERE case_id = ?",
        (fingerprint_id, case_id),
    )
    conn.commit()

    report["fingerprint_id"] = fingerprint_id
    report["normalized_fingerprint"] = normalized
    report = _set_state(conn, case_id, SCANNED, report)

    outcome = _decide(conn, normalized, row)
    report.update(outcome)
    report = _set_state(conn, case_id, MATCHED, report)

    if outcome["ai_used"]:
        report = _set_state(conn, case_id, ANALYZED, report)

    gate = _ai_gate(outcome)
    if gate is not None:
        status, compatibility_status, next_action = gate
        report["compatibility_status"] = compatibility_status
        report["next_action"] = next_action
        return _set_state(conn, case_id, status, report)

    return _finish_capability(
        conn, row, report, outcome, include_simulated=include_simulated_firmware
    )


def refresh_capability(
    conn: sqlite3.Connection,
    case_id: str,
    *,
    include_simulated_firmware: bool = False,
) -> dict[str, Any]:
    """Re-resolve an already matched case after firmware availability changed.

    A case waiting for firmware must be able to become testable without being
    re-created; that is the whole reason WAITING_FIRMWARE is not terminal.
    """
    row = _case_row(conn, case_id)
    if row["status"] not in (MATCHED, ANALYZED, WAITING_FIRMWARE):
        raise InvalidTransition(row["status"], READY_FOR_TEST)

    report = _report_of(row)
    outcome = {
        "candidate_profile": report.get("candidate_profile"),
        "candidate_protocol": normalize_protocol(report.get("candidate_protocol")),
        "candidate_transport": normalize_transport(report.get("candidate_transport")),
    }
    if outcome["candidate_protocol"] == "unknown":
        raise EngineError("case has no candidate protocol to re-resolve")
    return _finish_capability(
        conn, row, report, outcome, include_simulated=include_simulated_firmware
    )


def _apply_status(
    conn: sqlite3.Connection, case_id: str, report: dict[str, Any], status: str
) -> dict[str, Any]:
    """Move to ``status``, or just persist the trace when already there."""
    current = _case_row(conn, case_id)["status"]
    if status == current:
        return _save_report(conn, case_id, report)
    return _set_state(conn, case_id, status, report)


def _finish_capability(
    conn: sqlite3.Connection,
    row: sqlite3.Row,
    report: dict[str, Any],
    outcome: dict[str, Any],
    *,
    include_simulated: bool,
) -> dict[str, Any]:
    """Capability resolve -> case state. Shared by scan and re-resolve."""
    case_id = row["case_id"]
    decision = capability.resolve(
        conn,
        row["gateway_id"],
        outcome["candidate_transport"],
        outcome["candidate_protocol"],
        outcome["candidate_profile"],
        include_simulated=include_simulated,
    )
    report["capability_decision"] = decision.as_dict()
    report["requires_profile_update"] = decision.requires_profile_update
    report["requires_firmware_update"] = decision.requires_firmware_update
    report["minimum_firmware_version"] = decision.minimum_firmware_version
    report["firmware_channel"] = decision.minimum_firmware_channel
    report["firmware_available"] = decision.firmware_available
    report["minimum_firmware_simulated"] = decision.minimum_firmware_simulated
    report["print_implemented"] = _print_implemented(conn, outcome["candidate_profile"])

    spec = safe_tests.recommend(outcome["candidate_protocol"], outcome["candidate_transport"])
    executable = (
        spec is not None
        and decision.path != capability.PATH_BLOCKED
        and not decision.requires_firmware_update
    )
    report["recommended_safe_test"] = _safe_test_dict(spec, executable=executable)
    report["safe_test_executable"] = executable

    if decision.path == capability.PATH_BLOCKED:
        if (
            outcome["candidate_protocol"] != "unknown"
            and "no_firmware_release_provides_capability" in decision.reason_codes
        ):
            # The printer is understood, but no shipped firmware can talk to it
            # yet. That is a wait, not a dead end.
            report["compatibility_status"] = COMPAT_NEEDS_FIRMWARE_UNAVAILABLE
            report["next_action"] = NEXT_WAIT_FIRMWARE
            return _apply_status(conn, case_id, report, WAITING_FIRMWARE)
        report["compatibility_status"] = COMPAT_BLOCKED
        report["next_action"] = NEXT_NONE
        return _apply_status(conn, case_id, report, BLOCKED)

    if decision.requires_firmware_update:
        # Nothing can be tested on the current firmware; the safe test stays
        # listed as the step that follows the update, but is not runnable.
        report["compatibility_status"] = COMPAT_NEEDS_FIRMWARE
        report["next_action"] = NEXT_UPDATE_FIRMWARE
        return _save_report(conn, case_id, report)

    report["compatibility_status"] = (
        COMPAT_SUPPORTED_AFTER_PROFILE
        if decision.requires_profile_update
        else COMPAT_SUPPORTED
    )
    report["next_action"] = (
        NEXT_DEPLOY_PROFILE if decision.requires_profile_update else NEXT_RUN_SAFE_TEST
    )
    return _apply_status(conn, case_id, report, READY_FOR_TEST)


def run_test(
    conn: sqlite3.Connection,
    case_id: str,
    safe_test_id: str | None = None,
    *,
    outcome: str | None = None,
    adapter: Any = None,
) -> dict[str, Any]:
    """Execute the whitelisted safe test for a READY_FOR_TEST case.

    A real gateway EXECUTES the test and reports what the printer said; the
    browser cannot choose pass/fail. Only the simulator accepts a requested
    outcome (it has no printer), which is why ``outcome`` is rejected for real
    gateways instead of being silently ignored.
    """
    row = _case_row(conn, case_id)
    if row["status"] == TEST_FAILED:
        # A failed safe test is retryable: re-arm through the declared transition
        # (TEST_FAILED -> READY_FOR_TEST) instead of forcing a brand-new case and
        # a second scan. The printer/profile evidence is still valid.
        _set_state(conn, case_id, READY_FOR_TEST, _report_of(row))
        row = _case_row(conn, case_id)
    if row["status"] != READY_FOR_TEST:
        raise InvalidTransition(row["status"], TEST_PASSED)

    report = _report_of(row)
    if report.get("safe_test_executable") is False:
        raise EngineError("safe test is not executable for this case")
    recommended = report.get("recommended_safe_test") or {}
    chosen_id = safe_test_id or recommended.get("safe_test_id")
    if not chosen_id:
        raise EngineError("case has no recommended safe test")

    spec = safe_tests.get(chosen_id)  # whitelist: unknown ids raise
    if (
        spec.protocol != normalize_protocol(report.get("candidate_protocol"))
        or spec.transport != normalize_transport(report.get("candidate_transport"))
    ):
        raise EngineError(
            f"safe test {chosen_id} does not match "
            f"{report.get('candidate_protocol')}/{report.get('candidate_transport')}"
        )

    gateway = _gateway_row(conn, row["gateway_id"])
    mode = adapters.gateway_mode(gateway)
    if mode == adapters.MODE_REAL and outcome is not None:
        raise EngineError(
            "a real gateway executes the safe test; the client cannot choose the outcome"
        )
    if adapter is None:
        adapter = adapters.for_gateway(
            gateway,
            device_key=(report.get("scan_source") or {}).get("device_key"),
            simulate_outcome=outcome,
        )

    execution = adapter.run_safe_test(spec, report.get("normalized_fingerprint") or {})
    result = execution.status
    if result not in {"passed", "failed"}:
        raise EngineError(f"invalid safe test outcome: {result}")

    run_id = "run-" + uuid.uuid4().hex[:12]
    detail = dict(execution.detail)
    detail.setdefault("elapsed_ms", execution.elapsed_ms)
    conn.execute(
        "INSERT INTO safe_test_runs (run_id, case_id, safe_test_id, gateway_id,"
        " status, detail_json, created_at) VALUES (?,?,?,?,?,?,?)",
        (
            run_id,
            case_id,
            spec.safe_test_id,
            row["gateway_id"],
            result,
            json.dumps(detail, ensure_ascii=False),
            db.now_iso(),
        ),
    )
    conn.commit()

    report["safe_test_run"] = {
        "run_id": run_id,
        "safe_test_id": spec.safe_test_id,
        "status": result,
        **detail,
    }
    report["next_action"] = NEXT_NONE if result == "passed" else NEXT_MANUAL_REVIEW
    return _set_state(conn, case_id, TEST_PASSED if result == "passed" else TEST_FAILED, report)


def complete_case(conn: sqlite3.Connection, case_id: str) -> dict[str, Any]:
    """Turn a tested case into a printer instance (the only write of that row)."""
    row = _case_row(conn, case_id)
    if row["status"] != TEST_PASSED:
        raise InvalidTransition(row["status"], COMPLETED)

    report = _report_of(row)
    profile_id = report.get("candidate_profile")
    if not profile_id:
        raise EngineError("cannot complete a case without a candidate profile")

    profile = conn.execute(
        "SELECT * FROM printer_profiles WHERE profile_id = ?", (profile_id,)
    ).fetchone()
    if profile is None:
        raise EngineError(f"profile disappeared: {profile_id}")

    normalized = report.get("normalized_fingerprint") or {}
    printer_id = "prn-" + uuid.uuid4().hex[:8]
    display_name = report.get("brand") or profile["display_name"] or profile_id
    model = report.get("model") or profile["model"] or ""
    if model and model.lower() not in str(display_name).lower():
        display_name = f"{display_name} {model}"

    conn.execute(
        "INSERT INTO printer_instances (printer_id, gateway_id, profile_id,"
        " display_name, physical_address, status, case_id, created_at)"
        " VALUES (?,?,?,?,?,?,?,?)",
        (
            printer_id,
            row["gateway_id"],
            profile_id,
            str(display_name).strip(),
            normalized.get("address") or "",
            "active",
            case_id,
            db.now_iso(),
        ),
    )
    conn.commit()

    report["printer_id"] = printer_id
    report["printer"] = {
        "printer_id": printer_id,
        "profile_id": profile_id,
        "display_name": str(display_name).strip(),
        "physical_address": normalized.get("address") or "",
        "status": "active",
    }
    report["next_action"] = NEXT_NONE
    return _set_state(conn, case_id, COMPLETED, report)


# --------------------------------------------------------------------------- #
# decision helpers
# --------------------------------------------------------------------------- #
def _decide(
    conn: sqlite3.Connection, normalized: dict[str, Any], row: sqlite3.Row
) -> dict[str, Any]:
    """Stage A/B/C. Returns the evidence, never a final verdict."""
    profiles = matcher.load_profiles(conn)
    exact = matcher.exact_matches(profiles, normalized)
    ranked = matcher.similarity_ranking(profiles, normalized)
    similarity = [score.as_dict() for score in ranked]

    if exact:
        best = exact[0]
        seen = {score.profile_id for score in exact}
        evidence = [score.as_dict() for score in exact]
        evidence += [item for item in similarity if item["profile_id"] not in seen]
        return {
            "match_source": "exact",
            "ai_used": False,
            "candidate_profile": best.profile_id,
            "candidate_protocol": normalize_protocol(best.profile.get("protocol")),
            "candidate_transport": normalize_transport(best.profile.get("transport")),
            "confidence": 1.0,
            "reason_codes": best.reason_codes(),
            "candidates": evidence,
        }

    top = ranked[0] if ranked else None
    if top is not None and top.score >= config.SIMILARITY_AUTO_ACCEPT:
        return {
            "match_source": "similarity",
            "ai_used": False,
            "candidate_profile": top.profile_id,
            "candidate_protocol": normalize_protocol(top.profile.get("protocol")),
            "candidate_transport": normalize_transport(top.profile.get("transport")),
            "confidence": round(top.score, 4),
            "reason_codes": top.reason_codes(),
            "candidates": similarity,
        }

    context = ai_analyst.AnalysisContext(
        brand=row["brand"] or "",
        model=row["model"] or "",
        original_app=row["original_app"] or "",
        connection_hint=row["connection_hint"] or "unknown",
        fingerprint=normalized,
        candidates=similarity,
    )
    base: dict[str, Any] = {
        "match_source": "ai",
        "ai_used": True,
        "candidate_profile": None,
        "candidate_protocol": "unknown",
        "candidate_transport": normalized["transport"],
        "confidence": 0.0,
        "reason_codes": [],
        "candidates": similarity,
    }
    try:
        analysis = ai_analyst.get_analyst().analyze(context)
    except ai_analyst.AIAnalysisError as error:
        base["ai_error"] = str(error)
        base["reason_codes"] = ["no_candidate"]
        return base

    base.update(
        {
            "ai": analysis.model_dump(),
            "ai_action": analysis.action,
            "candidate_profile": analysis.candidate_profile,
            "candidate_protocol": normalize_protocol(analysis.candidate_protocol),
            "confidence": analysis.confidence,
            "reason_codes": list(analysis.reason_codes),
        }
    )
    if analysis.candidate_profile:
        profile = next(
            (p for p in profiles if p["profile_id"] == analysis.candidate_profile), None
        )
        if profile is not None:
            base["candidate_transport"] = normalize_transport(profile.get("transport"))
    return base


def _ai_gate(outcome: dict[str, Any]) -> tuple[str, str, str] | None:
    """Decide whether an AI answer may advance. ``None`` means it may."""
    if outcome["match_source"] != "ai":
        return None
    action = outcome.get("ai_action")
    if action == "UNSUPPORTED" or outcome["candidate_protocol"] == "unknown":
        return BLOCKED, COMPAT_BLOCKED, NEXT_NONE
    if action == "ESCALATE" or outcome["confidence"] < config.AI_MIN_CONFIDENCE:
        return NEEDS_REVIEW, COMPAT_NEEDS_REVIEW, NEXT_MANUAL_REVIEW
    return None


def _safe_test_dict(
    spec: safe_tests.SafeTestSpec | None, *, executable: bool = False
) -> dict[str, Any] | None:
    if spec is None:
        return None
    return {
        "safe_test_id": spec.safe_test_id,
        "protocol": spec.protocol,
        "transport": spec.transport,
        "description": spec.description,
        "steps": list(spec.steps),
        "command": spec.command,
        "executable": executable,
    }


def _print_implemented(conn: sqlite3.Connection, profile_id: str | None) -> bool:
    if not profile_id:
        return False
    row = conn.execute(
        "SELECT parameters_json FROM printer_profiles WHERE profile_id = ?", (profile_id,)
    ).fetchone()
    if row is None:
        return False
    try:
        return bool(json.loads(row["parameters_json"]).get("implemented"))
    except ValueError:
        return False

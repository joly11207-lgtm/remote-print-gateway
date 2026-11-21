"""Stage A (exact) and Stage B (similarity) matchers.

Both stages work on the normalised fingerprint only. Neither stage is allowed
to *decide* compatibility - they produce candidates and evidence, and the
capability resolver + safe test registry decide what may actually run.
"""

from __future__ import annotations

import fnmatch
import json
import sqlite3
from dataclasses import dataclass, field
from typing import Any

from .canonical import normalize_protocol, normalize_transport, normalize_uuid

FEATURE_WEIGHTS = {
    "transport": 0.15,
    "service_uuid": 0.25,
    "advertised_service_uuid": 0.20,
    "write_uuid": 0.20,
    "notify_uuid": 0.10,
    "spp_uuid": 0.20,
    "name": 0.10,
}

FEATURE_LABELS = {
    "transport": "transport",
    "service_uuid": "gatt_service",
    "advertised_service_uuid": "advertised_service",
    "write_uuid": "write_characteristic",
    "notify_uuid": "notify_characteristic",
    "spp_uuid": "spp_service",
    "name": "name_pattern",
}


@dataclass
class ProfileScore:
    profile: dict[str, Any]
    score: float = 0.0
    matched: list[str] = field(default_factory=list)
    missing: list[str] = field(default_factory=list)
    conflicting: list[str] = field(default_factory=list)
    exact: bool = False

    @property
    def profile_id(self) -> str:
        return self.profile["profile_id"]

    def reason_codes(self) -> list[str]:
        codes: list[str] = []
        for feature in self.matched:
            codes.append("matching_" + FEATURE_LABELS[feature])
        for feature in self.conflicting:
            codes.append("conflicting_" + FEATURE_LABELS[feature])
        for feature in self.missing:
            codes.append("missing_" + FEATURE_LABELS[feature])
        if not codes:
            codes.append("no_matching_evidence")
        return codes

    def as_dict(self) -> dict[str, Any]:
        return {
            "profile_id": self.profile_id,
            "display_name": self.profile.get("display_name"),
            "brand": self.profile.get("brand"),
            "model": self.profile.get("model"),
            "protocol": normalize_protocol(self.profile.get("protocol")),
            "transport": normalize_transport(self.profile.get("transport")),
            "profile_status": self.profile.get("status"),
            "score": round(self.score, 4),
            "exact": self.exact,
            "matched_features": list(self.matched),
            "missing_features": list(self.missing),
            "conflicting_features": list(self.conflicting),
            "reason_codes": self.reason_codes(),
        }


def load_profiles(conn: sqlite3.Connection) -> list[dict[str, Any]]:
    rows = conn.execute(
        "SELECT * FROM printer_profiles WHERE status != 'UNSUPPORTED'"
    ).fetchall()
    profiles = []
    for row in rows:
        item = dict(row)
        item["parameters"] = json.loads(item["parameters_json"])
        item["rules"] = json.loads(item["fingerprint_rules_json"])
        profiles.append(item)
    return profiles


def _uuid_evidence(feature: str, rule_value: str, fingerprint_values: list[str]) -> str:
    """Return 'match' | 'missing' | 'conflict' for a UUID feature."""
    if not rule_value:
        return "absent"
    if not fingerprint_values:
        return "missing"
    # Profile rules are written by humans ("AE30", the full SPP UUID, ...) and
    # the fingerprint is already normalised, so the rule must be normalised too.
    normalized_rule = normalize_uuid(rule_value)
    if normalized_rule and normalized_rule in fingerprint_values:
        return "match"
    return "conflict"


def _name_evidence(patterns: list[str], name: str) -> str:
    if not patterns:
        return "absent"
    if not name:
        return "missing"
    upper = name.upper()
    for pattern in patterns:
        if fnmatch.fnmatchcase(upper, str(pattern).upper()):
            return "match"
    return "mismatch"


def score_profile(profile: dict[str, Any], normalized: dict[str, Any]) -> ProfileScore:
    rules = profile["rules"]
    result = ProfileScore(profile=profile)
    # ``declared`` is the denominator: every rule the profile actually states,
    # including transport. Leaving transport out made scores exceed 1.0 and made
    # ``exact`` impossible for every profile.
    declared: list[str] = []

    transport_rule = normalize_transport(rules.get("transport"))
    if transport_rule != "unknown":
        declared.append("transport")
        if normalized["transport"] == "unknown":
            result.missing.append("transport")
        elif normalized["transport"] == transport_rule:
            result.matched.append("transport")
        else:
            # A transport conflict is disqualifying: a BLE profile can never
            # describe an SPP printer.
            result.conflicting.append("transport")
            result.score = 0.0
            return result

    for feature in (
        "service_uuid",
        "advertised_service_uuid",
        "write_uuid",
        "notify_uuid",
        "spp_uuid",
    ):
        evidence = _uuid_evidence(
            feature,
            str(rules.get(feature) or ""),
            normalized[_fingerprint_key(feature)],
        )
        if evidence == "absent":
            continue
        declared.append(feature)
        if evidence == "match":
            result.matched.append(feature)
        elif evidence == "missing":
            result.missing.append(feature)
        else:
            result.conflicting.append(feature)

    name_evidence = _name_evidence(rules.get("bluetooth_name_patterns") or [], normalized.get("name", ""))
    if name_evidence != "absent":
        declared.append("name")
        if name_evidence == "match":
            result.matched.append("name")
        elif name_evidence == "missing":
            result.missing.append("name")

    total = sum(FEATURE_WEIGHTS[f] for f in declared)
    if total <= 0:
        result.score = 0.0
        return result
    gained = sum(FEATURE_WEIGHTS[f] for f in result.matched)
    result.score = gained / total

    result.exact = (
        not result.conflicting
        and not result.missing
        and set(declared) == set(result.matched)
        and "transport" in result.matched
    )
    if result.exact:
        result.score = 1.0
    return result


def _fingerprint_key(feature: str) -> str:
    return {
        "service_uuid": "service_uuids",
        "advertised_service_uuid": "advertised_service_uuids",
        "write_uuid": "write_uuids",
        "notify_uuid": "notify_uuids",
        "spp_uuid": "spp_uuids",
    }[feature]


def exact_matches(profiles: list[dict[str, Any]], normalized: dict[str, Any]) -> list[ProfileScore]:
    """Stage A: every profile whose declared fingerprint is fully satisfied.

    Several profiles can be exact at once when they share a radio fingerprint
    (all AE30 printers look alike). The most specific one - the profile that
    matched the most declared features, e.g. also matched the name pattern -
    comes first, so callers never have to know about profile ids.
    """
    scored = [score_profile(p, normalized) for p in profiles]
    exact = [s for s in scored if s.exact]
    exact.sort(key=lambda s: (-len(s.matched), s.profile_id))
    return exact


def similarity_ranking(
    profiles: list[dict[str, Any]], normalized: dict[str, Any], top_n: int = 5
) -> list[ProfileScore]:
    """Stage B: best-effort ranking when nothing matches exactly."""
    scored = [score_profile(p, normalized) for p in profiles]
    scored = [s for s in scored if s.score > 0 and not s.conflicting]
    scored.sort(key=lambda s: (-s.score, -len(s.matched), s.profile_id))
    return scored[:top_n]

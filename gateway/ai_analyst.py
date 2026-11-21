"""Stage C: AI compatibility analyst.

Hard boundaries (enforced by types, not by prompt text):

* The analyst may only return the JSON contract below. Free text is rejected by
  ``AIAnalysis.model_validate`` and the case falls back to a low-confidence
  result.
* The analyst never emits hardware commands. Its output is a candidate + an
  action verb; the capability resolver and the safe-test whitelist decide what
  is actually allowed to run.

Two implementations ship in V0.1:

* ``RuleBasedAnalyst``  - deterministic, no network, is the default.
* ``HttpLLMAnalyst``    - used only when RTB_AI_ENDPOINT is configured.
"""

from __future__ import annotations

import json
import os
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from typing import Any, Literal, Protocol

from pydantic import BaseModel, Field, field_validator

from .canonical import PROTOCOLS as CANONICAL_PROTOCOLS
from .canonical import normalize_protocol

# Same values as AIAnalysis.candidate_protocol; kept as one tuple so the
# prompt, the contract and the canonical vocabulary cannot drift apart.
PROTOCOLS = CANONICAL_PROTOCOLS

ALLOWED_REASON_CODES = {
    "matching_gatt_service",
    "matching_advertised_service",
    "matching_write_characteristic",
    "matching_notify_characteristic",
    "matching_name_pattern",
    "matching_transport",
    "matching_brand_keyword",
    "matching_model_keyword",
    "matching_app_keyword",
    "protocol_family_heuristic",
    "missing_gatt_service",
    "conflicting_service_uuid",
    "low_similarity",
    "no_candidate",
    "unknown_transport",
}


class AIAnalysis(BaseModel):
    candidate_protocol: Literal["tiny_iprint", "escpos", "ipp", "memobird", "unknown"]
    candidate_profile: str | None = None
    confidence: float = Field(ge=0.0, le=1.0)
    reason_codes: list[str] = Field(min_length=1, max_length=12)
    action: Literal["PROFILE_VARIANT", "NEW_PROFILE", "PROTOCOL_FAMILY", "UNSUPPORTED", "ESCALATE"]

    @field_validator("reason_codes")
    @classmethod
    def _known_codes(cls, value: list[str]) -> list[str]:
        unknown = [code for code in value if code not in ALLOWED_REASON_CODES]
        if unknown:
            raise ValueError(f"unknown reason codes: {unknown}")
        return value


class AIAnalysisError(RuntimeError):
    """Raised when the analyst is unavailable or returned an invalid contract."""


@dataclass
class AnalysisContext:
    brand: str = ""
    model: str = ""
    original_app: str = ""
    # The user's own words (bluetooth | wifi | unknown), never a transport.
    connection_hint: str = "unknown"
    fingerprint: dict[str, Any] = field(default_factory=dict)
    candidates: list[dict[str, Any]] = field(default_factory=list)
    known_protocols: list[str] = field(default_factory=lambda: ["tiny_iprint", "escpos"])

    def as_prompt_payload(self) -> dict[str, Any]:
        return {
            "brand": self.brand,
            "model": self.model,
            "original_app": self.original_app,
            "connection_hint": self.connection_hint,
            "fingerprint": self.fingerprint,
            "candidates": self.candidates,
            "known_protocols": self.known_protocols,
        }


class Analyst(Protocol):
    name: str

    def analyze(self, context: AnalysisContext) -> AIAnalysis: ...


PROTOCOL_HINTS = {
    "ae30": "tiny_iprint",
    "ae01": "tiny_iprint",
    "ae02": "tiny_iprint",
}

# SPP is only the transport. The payload family has to come from the name:
# G2D carries the Memobird Classic Protocol recovered from the official app,
# generic SPP printers are assumed to be ESC/POS. Neither is promoted to a
# profile without matching fingerprint evidence.
MEMOBIRD_NAME_HINTS = ("g2d", "memobird", "咕咕机")


class RuleBasedAnalyst:
    """Deterministic stand-in for a real model.

    It is deliberately conservative: it only upgrades a similarity candidate
    when independent evidence (brand/model text or protocol-family UUIDs)
    agrees with it.
    """

    name = "rule-based-v1"

    def analyze(self, context: AnalysisContext) -> AIAnalysis:
        fp = context.fingerprint or {}
        reasons: list[str] = []

        # 1. protocol family from UUID evidence
        uuids = set(
            (fp.get("service_uuids") or [])
            + (fp.get("advertised_service_uuids") or [])
            + (fp.get("write_uuids") or [])
            + (fp.get("notify_uuids") or [])
        )
        heuristic_protocol = "unknown"
        for uuid, protocol in PROTOCOL_HINTS.items():
            if uuid in uuids:
                heuristic_protocol = protocol
                reasons.append("protocol_family_heuristic")
                if uuid in (fp.get("service_uuids") or []):
                    reasons.append("matching_gatt_service")
                if uuid in (fp.get("write_uuids") or []):
                    reasons.append("matching_write_characteristic")
                if uuid in (fp.get("notify_uuids") or []):
                    reasons.append("matching_notify_characteristic")
                break
        if fp.get("spp_uuids"):
            name = str(fp.get("name") or "").lower()
            if any(hint in name for hint in MEMOBIRD_NAME_HINTS):
                heuristic_protocol = "memobird"
                reasons.append("matching_name_pattern")
            else:
                heuristic_protocol = "escpos"
            reasons.append("protocol_family_heuristic")

        # 2. textual evidence against the candidate list
        text = " ".join([context.brand, context.model, context.original_app]).lower()
        best = None
        best_score = 0.0
        for candidate in context.candidates:
            score = float(candidate.get("score") or 0.0)
            for needle in (candidate.get("brand"), candidate.get("model"), candidate.get("display_name")):
                if needle and str(needle).lower() and str(needle).lower() in text:
                    score += 0.08
                    reasons.append("matching_brand_keyword")
            if score > best_score:
                best_score = score
                best = candidate

        if best is not None and best_score >= 0.55:
            protocol = normalize_protocol(best.get("protocol") or heuristic_protocol)
            confidence = min(0.93, 0.55 + best_score * 0.35)
            return AIAnalysis(
                candidate_protocol=protocol,
                candidate_profile=best.get("profile_id"),
                confidence=round(confidence, 3),
                reason_codes=_dedupe(reasons) or ["matching_gatt_service"],
                action="PROFILE_VARIANT",
            )

        if heuristic_protocol != "unknown":
            return AIAnalysis(
                candidate_protocol=heuristic_protocol,
                candidate_profile=None,
                confidence=0.62,
                reason_codes=_dedupe(reasons) or ["protocol_family_heuristic"],
                action="PROTOCOL_FAMILY",
            )

        return AIAnalysis(
            candidate_protocol="unknown",
            candidate_profile=None,
            confidence=0.2,
            reason_codes=["no_candidate", "low_similarity"],
            action="UNSUPPORTED",
        )


def _dedupe(items: list[str]) -> list[str]:
    seen: list[str] = []
    for item in items:
        if item not in seen:
            seen.append(item)
    return seen


SYSTEM_PROMPT = (
    "You are a printer compatibility analyst. Reply with ONE JSON object and "
    "nothing else. Schema: {candidate_protocol: one of tiny_iprint|escpos|ipp|"
    "memobird|unknown, candidate_profile: string|null, confidence: number 0..1, "
    "reason_codes: string[], action: one of PROFILE_VARIANT|NEW_PROFILE|"
    "PROTOCOL_FAMILY|UNSUPPORTED|ESCALATE}. Never output commands, hex payloads "
    "or prose."
)


class HttpLLMAnalyst:
    """OpenAI-compatible JSON-mode client. Optional; no key = never constructed."""

    name = "http-llm"

    def __init__(self, endpoint: str, api_key: str, model: str, timeout: float = 20.0) -> None:
        self.endpoint = endpoint
        self.api_key = api_key
        self.model = model
        self.timeout = timeout

    def analyze(self, context: AnalysisContext) -> AIAnalysis:
        payload = {
            "model": self.model,
            "response_format": {"type": "json_object"},
            "messages": [
                {"role": "system", "content": SYSTEM_PROMPT},
                {"role": "user", "content": json.dumps(context.as_prompt_payload(), ensure_ascii=False)},
            ],
        }
        request = urllib.request.Request(
            self.endpoint,
            data=json.dumps(payload).encode("utf-8"),
            headers={
                "Content-Type": "application/json",
                "Authorization": f"Bearer {self.api_key}",
            },
            method="POST",
        )
        try:
            with urllib.request.urlopen(request, timeout=self.timeout) as response:
                body = json.loads(response.read().decode("utf-8"))
        except (urllib.error.URLError, TimeoutError, ValueError) as error:
            raise AIAnalysisError(f"analyst request failed: {error}") from error

        try:
            content = body["choices"][0]["message"]["content"]
        except (KeyError, IndexError, TypeError) as error:
            raise AIAnalysisError("analyst response missing choices[0].message.content") from error

        try:
            parsed = json.loads(content)
        except ValueError as error:
            raise AIAnalysisError("analyst returned non-JSON content") from error

        # Free text or an off-contract object is a hard failure, never a guess.
        return AIAnalysis.model_validate(parsed)


def get_analyst() -> Analyst:
    endpoint = os.environ.get("RTB_AI_ENDPOINT", "").strip()
    api_key = os.environ.get("RTB_AI_API_KEY", "").strip()
    model = os.environ.get("RTB_AI_MODEL", "gpt-4o-mini").strip()
    if endpoint and api_key:
        return HttpLLMAnalyst(endpoint=endpoint, api_key=api_key, model=model)
    return RuleBasedAnalyst()

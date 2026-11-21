"""Minimal HTTP API + same-origin web UI (V0.1).

The HTTP layer is transport only:

    parse -> validate -> call gateway.engine -> map errors -> return the
    engine report unchanged

No matcher, analyst, capability, lifecycle or safe-test policy logic lives
here. If a rule is needed, it belongs in ``engine.py``.

Static files are served from ``web/`` on the same origin (``/``, ``/app.js``,
``/styles.css``) so the UI needs no build step, no Node and no CORS.

Run: ``python -m gateway.api --host 127.0.0.1 --port 8080``

Response bodies are the engine's own dicts, serialised verbatim, so no field can
be lost in a response schema. pydantic is used only to validate requests.
"""

from __future__ import annotations

import argparse
import json
import re
import sqlite3
import sys
from contextlib import contextmanager
from dataclasses import dataclass
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any, Callable, Iterator, Literal
from urllib.parse import unquote, urlparse

from pydantic import BaseModel, ConfigDict, Field, ValidationError

from . import adapters, ai_analyst, config, db, engine, safe_tests, simulated_firmware

MAX_BODY_BYTES = 64 * 1024

OUTCOMES = {
    "pass": "passed",
    "passed": "passed",
    "fail": "failed",
    "failed": "failed",
}


@dataclass(frozen=True)
class StaticResponse:
    content_type: str
    body: bytes


# --------------------------------------------------------------------------- #
# request schemas (pydantic, extra fields rejected)
# --------------------------------------------------------------------------- #
class CreateCaseRequest(BaseModel):
    model_config = ConfigDict(extra="forbid")

    gateway_id: str = Field(min_length=1)
    brand: str = ""
    model: str = ""
    original_app: str = ""
    # The user's own words. Deliberately not a transport: "Wi-Fi" can end up as
    # IPP, raw TCP or something else, so only the gateway fingerprint decides.
    connection_hint: Literal["bluetooth", "wifi", "unknown"] = "unknown"
    device_key: str | None = None
    raw_fingerprint: dict[str, Any] | None = None


class FirmwareOptionRequest(BaseModel):
    """Body for /scan and /refresh-capability. Both fields are optional."""

    model_config = ConfigDict(extra="forbid")

    include_simulated_firmware: bool = False


class TestRequest(BaseModel):
    model_config = ConfigDict(extra="forbid")

    safe_test_id: str | None = None
    outcome: str = "pass"


class DevFirmwareRequest(BaseModel):
    """Body for /api/dev/simulated-firmware. 404 unless dev mode is enabled."""

    model_config = ConfigDict(extra="forbid")

    installed: bool = True


# --------------------------------------------------------------------------- #
# errors
# --------------------------------------------------------------------------- #
class ApiError(Exception):
    def __init__(
        self,
        status: int,
        code: str,
        message: str,
        case_id: str | None = None,
    ) -> None:
        super().__init__(message)
        self.status = status
        self.code = code
        self.message = message
        self.case_id = case_id

    def payload(self) -> dict[str, Any]:
        body: dict[str, Any] = {"error": self.code, "message": self.message}
        if self.case_id:
            body["case_id"] = self.case_id
        return body


# Ordered: subclasses before their bases (CaseNotFound is an EngineError+KeyError).
# Gateway transport failures keep their own codes: "the gateway is offline" must
# never be reported as "this printer is unsupported".
_ERROR_MAP: tuple[tuple[type[BaseException], int, str], ...] = (
    (engine.CaseNotFound, 404, "CASE_NOT_FOUND"),
    (engine.UnknownGateway, 404, "GATEWAY_NOT_FOUND"),
    (adapters.PrinterNotFound, 404, "PRINTER_NOT_FOUND"),
    (engine.InvalidTransition, 409, "INVALID_TRANSITION"),
    (safe_tests.UnknownSafeTest, 422, "SAFE_TEST_NOT_ALLOWED"),
    (engine.EngineError, 422, "ACTION_NOT_ALLOWED"),
    (adapters.GatewayTimeout, 504, "GATEWAY_TIMEOUT"),
    (adapters.GatewayOffline, 503, "GATEWAY_OFFLINE"),
    (adapters.GatewayInvalidResponse, 502, "GATEWAY_INVALID_RESPONSE"),
    (ai_analyst.AIAnalysisError, 503, "DEPENDENCY_UNAVAILABLE"),
)


def _validation_message(error: ValidationError) -> str:
    parts = []
    for item in error.errors():
        location = ".".join(str(part) for part in item.get("loc", ())) or "body"
        parts.append(f"{location}: {item.get('msg', 'invalid')}")
    return "; ".join(parts)


def _to_api_error(error: BaseException, case_id: str | None = None) -> ApiError:
    """Single place that turns an exception into an HTTP status + code."""
    if isinstance(error, ApiError):
        if error.case_id is None:
            error.case_id = case_id
        return error
    if isinstance(error, ValidationError):
        return ApiError(400, "INVALID_REQUEST", _validation_message(error), case_id)
    for kind, status, code in _ERROR_MAP:
        if isinstance(error, kind):
            return ApiError(status, code, str(error), case_id)
    if isinstance(error, KeyError):
        return ApiError(404, "NOT_FOUND", str(error), case_id)
    if isinstance(error, sqlite3.Error):
        return ApiError(503, "STORAGE_UNAVAILABLE", "database unavailable", case_id)
    return ApiError(500, "INTERNAL_ERROR", "unexpected server error", case_id)


def _decode_body(body: bytes) -> dict[str, Any]:
    if not body:
        return {}
    if len(body) > MAX_BODY_BYTES:
        raise ApiError(400, "INVALID_REQUEST", "request body too large")
    try:
        payload = json.loads(body.decode("utf-8"))
    except (UnicodeDecodeError, ValueError) as error:
        raise ApiError(400, "INVALID_REQUEST", f"body is not valid JSON: {error}") from error
    if not isinstance(payload, dict):
        raise ApiError(400, "INVALID_REQUEST", "body must be a JSON object")
    return payload


# --------------------------------------------------------------------------- #
# routes
# --------------------------------------------------------------------------- #
RouteHandler = Callable[[dict[str, str], dict[str, Any]], Any]

ROUTES: tuple[tuple[str, re.Pattern[str], str], ...] = (
    # same-origin static UI
    ("GET", re.compile(r"^/$"), "index_html"),
    ("GET", re.compile(r"^/index\.html$"), "index_html"),
    ("GET", re.compile(r"^/app\.js$"), "app_js"),
    ("GET", re.compile(r"^/i18n\.js$"), "i18n_js"),
    ("GET", re.compile(r"^/serial\.js$"), "serial_js"),
    ("GET", re.compile(r"^/styles\.css$"), "styles_css"),
    # V1 pages are real HTML paths (folder/index.html), so a deep link and a
    # browser refresh both hit the same file, locally and on static hosting.
    ("GET", re.compile(r"^/setup/?$"), "setup_html"),
    ("GET", re.compile(r"^/lab/?$"), "lab_html"),
    ("GET", re.compile(r"^/printers/?$"), "printers_html"),
    ("GET", re.compile(r"^/printers/add/?$"), "printers_add_html"),
    ("GET", re.compile(r"^/print/?$"), "print_html"),
    ("GET", re.compile(r"^/compatibility/?$"), "compatibility_html"),
    # compatibility API
    ("GET", re.compile(r"^/api/health$"), "health"),
    ("GET", re.compile(r"^/api/gateways$"), "list_gateways"),
    ("GET", re.compile(r"^/api/gateways/(?P<gateway_id>[^/]+)$"), "get_gateway"),
    ("POST", re.compile(r"^/api/compatibility/cases$"), "create_case"),
    ("GET", re.compile(r"^/api/compatibility/cases/(?P<case_id>[^/]+)$"), "get_case"),
    ("POST", re.compile(r"^/api/compatibility/cases/(?P<case_id>[^/]+)/scan$"), "scan_case"),
    ("POST", re.compile(r"^/api/compatibility/cases/(?P<case_id>[^/]+)/test$"), "run_test"),
    ("POST", re.compile(r"^/api/compatibility/cases/(?P<case_id>[^/]+)/complete$"), "complete_case"),
    (
        "POST",
        re.compile(r"^/api/compatibility/cases/(?P<case_id>[^/]+)/refresh-capability$"),
        "refresh_capability",
    ),
    # dev-only fixtures; 403 unless the server was started with --allow-simulated-firmware
    ("POST", re.compile(r"^/api/dev/simulated-firmware$"), "dev_simulated_firmware"),
    ("POST", re.compile(r"^/api/dev/gateway-upgrade$"), "dev_gateway_upgrade"),
)


class Api:
    """Framework-free dispatcher; the HTTP handler is a thin shell around it."""

    def __init__(
        self,
        connect: Callable[[], sqlite3.Connection] = db.connect,
        allow_simulated_firmware: bool | None = None,
    ) -> None:
        self._connect = connect
        self.allow_simulated_firmware = (
            config.ALLOW_SIMULATED_FIRMWARE
            if allow_simulated_firmware is None
            else allow_simulated_firmware
        )

    # ---------------------------------------------------------------- plumbing
    @contextmanager
    def session(self) -> Iterator[sqlite3.Connection]:
        conn = self._connect()
        try:
            yield conn
        finally:
            conn.close()

    def dispatch(
        self, method: str, path: str, body: bytes = b""
    ) -> tuple[int, dict[str, Any] | StaticResponse]:
        case_id: str | None = None
        try:
            handler, params = self._match(method, path)
            case_id = params.get("case_id")
            payload = _decode_body(body)
            return 200, handler(params, payload)
        except Exception as error:  # noqa: BLE001 - mapped below, never leaked
            api_error = _to_api_error(error, case_id)
            if api_error.code == "INTERNAL_ERROR":
                sys.stderr.write(
                    f"[api] unexpected error on {method} {path}: {error!r}\n"
                )
            return api_error.status, api_error.payload()

    def _match(
        self, method: str, path: str
    ) -> tuple[RouteHandler, dict[str, str]]:
        allowed: set[str] = set()
        for route_method, pattern, name in ROUTES:
            found = pattern.match(path)
            if found is None:
                continue
            if route_method != method:
                allowed.add(route_method)
                continue
            params = {key: unquote(value) for key, value in found.groupdict().items()}
            return getattr(self, name), params
        if allowed:
            raise ApiError(
                405, "METHOD_NOT_ALLOWED", f"{method} not allowed; use {sorted(allowed)}"
            )
        raise ApiError(404, "NOT_FOUND", f"no route for {path}")

    def _simulation(self, requested: bool) -> bool:
        if not requested:
            return False
        if not self.allow_simulated_firmware:
            raise ApiError(
                403,
                "SIMULATION_DISABLED",
                "simulated firmware is disabled on this server",
            )
        return True

    # ------------------------------------------------------------------ routes
    def _static(self, name: str, content_type: str) -> StaticResponse:
        path = config.WEB_DIR / name
        if not path.is_file():
            raise ApiError(404, "NOT_FOUND", f"missing static asset: {name}")
        return StaticResponse(content_type=content_type, body=path.read_bytes())

    def index_html(self, params: dict[str, str], payload: dict[str, Any]) -> StaticResponse:
        return self._static("index.html", "text/html; charset=utf-8")

    def app_js(self, params: dict[str, str], payload: dict[str, Any]) -> StaticResponse:
        return self._static("app.js", "application/javascript; charset=utf-8")

    def i18n_js(self, params: dict[str, str], payload: dict[str, Any]) -> StaticResponse:
        return self._static("i18n.js", "application/javascript; charset=utf-8")

    def serial_js(self, params: dict[str, str], payload: dict[str, Any]) -> StaticResponse:
        # V1.1: Web Serial transport. Served from this origin like every other
        # asset; the page never loads a third-party script.
        return self._static("serial.js", "application/javascript; charset=utf-8")

    def styles_css(self, params: dict[str, str], payload: dict[str, Any]) -> StaticResponse:
        return self._static("styles.css", "text/css; charset=utf-8")

    def _page(self, folder: str) -> StaticResponse:
        return self._static(f"{folder}/index.html", "text/html; charset=utf-8")

    def setup_html(self, params: dict[str, str], payload: dict[str, Any]) -> StaticResponse:
        return self._page("setup")

    def lab_html(self, params: dict[str, str], payload: dict[str, Any]) -> StaticResponse:
        return self._page("lab")

    def printers_html(self, params: dict[str, str], payload: dict[str, Any]) -> StaticResponse:
        return self._page("printers")

    def printers_add_html(
        self, params: dict[str, str], payload: dict[str, Any]
    ) -> StaticResponse:
        return self._page("printers/add")

    def print_html(self, params: dict[str, str], payload: dict[str, Any]) -> StaticResponse:
        return self._page("print")

    def compatibility_html(
        self, params: dict[str, str], payload: dict[str, Any]
    ) -> StaticResponse:
        return self._page("compatibility")

    def health(self, params: dict[str, str], payload: dict[str, Any]) -> dict[str, Any]:
        return {"app": config.APP_NAME, "version": config.APP_VERSION, "status": "ok"}

    def list_gateways(self, params: dict[str, str], payload: dict[str, Any]) -> dict[str, Any]:
        with self.session() as conn:
            return engine.list_gateways(conn)

    def get_gateway(self, params: dict[str, str], payload: dict[str, Any]) -> dict[str, Any]:
        with self.session() as conn:
            return engine.gateway_snapshot(conn, params["gateway_id"])

    def create_case(self, params: dict[str, str], payload: dict[str, Any]) -> dict[str, Any]:
        request = CreateCaseRequest.model_validate(payload)
        if request.raw_fingerprint is not None and request.device_key:
            raise ApiError(
                400,
                "INVALID_REQUEST",
                "provide either device_key or raw_fingerprint, not both",
            )
        with self.session() as conn:
            return engine.create_case(
                conn,
                request.gateway_id,
                brand=request.brand,
                model=request.model,
                original_app=request.original_app,
                connection_hint=request.connection_hint,
                device_key=request.device_key,
                raw=request.raw_fingerprint,
            )

    def get_case(self, params: dict[str, str], payload: dict[str, Any]) -> dict[str, Any]:
        with self.session() as conn:
            return engine.get_case(conn, params["case_id"])

    def scan_case(self, params: dict[str, str], payload: dict[str, Any]) -> dict[str, Any]:
        request = FirmwareOptionRequest.model_validate(payload)
        include = self._simulation(request.include_simulated_firmware)
        with self.session() as conn:
            return engine.scan_case(
                conn, params["case_id"], include_simulated_firmware=include
            )

    def refresh_capability(
        self, params: dict[str, str], payload: dict[str, Any]
    ) -> dict[str, Any]:
        request = FirmwareOptionRequest.model_validate(payload)
        include = self._simulation(request.include_simulated_firmware)
        with self.session() as conn:
            return engine.refresh_capability(
                conn, params["case_id"], include_simulated_firmware=include
            )

    def run_test(self, params: dict[str, str], payload: dict[str, Any]) -> dict[str, Any]:
        request = TestRequest.model_validate(payload)
        # Only an EXPLICIT outcome is forwarded (and only the simulator honours
        # it). A real gateway executes the test; sending {"outcome": "..."} to a
        # real case is rejected by the engine instead of being ignored.
        outcome: str | None = None
        if "outcome" in request.model_fields_set:
            outcome = OUTCOMES.get((request.outcome or "").strip().lower())
            if outcome is None:
                raise ApiError(
                    400, "INVALID_REQUEST", f"unknown outcome: {request.outcome}"
                )
        with self.session() as conn:
            return engine.run_test(
                conn, params["case_id"], request.safe_test_id, outcome=outcome
            )

    def complete_case(self, params: dict[str, str], payload: dict[str, Any]) -> dict[str, Any]:
        with self.session() as conn:
            return engine.complete_case(conn, params["case_id"])

    # -------------------------------------------------------------- dev only
    def _require_dev(self) -> None:
        if not self.allow_simulated_firmware:
            raise ApiError(
                403,
                "SIMULATION_DISABLED",
                "development fixtures are disabled on this server",
            )

    def dev_simulated_firmware(
        self, params: dict[str, str], payload: dict[str, Any]
    ) -> dict[str, Any]:
        """Install/remove the simulated firmware row. Fixture tooling, not a product API."""
        self._require_dev()
        request = DevFirmwareRequest.model_validate(payload)
        with self.session() as conn:
            if request.installed:
                simulated_firmware.install(conn)
            else:
                simulated_firmware.uninstall(conn)
            return {
                "installed": simulated_firmware.installed(conn),
                "simulated": True,
                "firmware_version": simulated_firmware.SIMULATED_RELEASE["firmware_version"],
                "channel": simulated_firmware.SIMULATED_RELEASE["channel"],
            }

    def dev_gateway_upgrade(
        self, params: dict[str, str], payload: dict[str, Any]
    ) -> dict[str, Any]:
        """Pretend the gateway finished updating. Fixture tooling only."""
        self._require_dev()
        with self.session() as conn:
            return simulated_firmware.emulate_gateway_upgrade(
                conn, gateway_id=config.DEFAULT_GATEWAY_ID
            )


# --------------------------------------------------------------------------- #
# HTTP shell
# --------------------------------------------------------------------------- #
def make_handler(api: Api) -> type[BaseHTTPRequestHandler]:
    class Handler(BaseHTTPRequestHandler):
        server_version = "RTB-Gateway/0.1"
        protocol_version = "HTTP/1.1"

        def do_GET(self) -> None:  # noqa: N802 - http.server naming
            self._dispatch("GET")

        def do_POST(self) -> None:  # noqa: N802 - http.server naming
            self._dispatch("POST")

        def _dispatch(self, method: str) -> None:
            parsed = urlparse(self.path)
            try:
                length = int(self.headers.get("Content-Length") or 0)
            except ValueError:
                length = 0
            body = self.rfile.read(length) if length > 0 else b""
            status, payload = api.dispatch(method, parsed.path, body)
            if isinstance(payload, StaticResponse):
                data = payload.body
                content_type = payload.content_type
            else:
                data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
                content_type = "application/json; charset=utf-8"
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def log_message(self, fmt: str, *args: Any) -> None:
            sys.stderr.write("[api] " + (fmt % args) + "\n")

    return Handler


def make_server(
    host: str = config.HTTP_HOST,
    port: int = config.HTTP_PORT,
    *,
    allow_simulated_firmware: bool | None = None,
) -> ThreadingHTTPServer:
    """Bind without serving; tests use port 0 and read the assigned port."""
    api = Api(allow_simulated_firmware=allow_simulated_firmware)
    return ThreadingHTTPServer((host, port), make_handler(api))


def serve(
    host: str = config.HTTP_HOST,
    port: int = config.HTTP_PORT,
    *,
    allow_simulated_firmware: bool | None = None,
) -> int:
    db.initialize()
    server = make_server(host, port, allow_simulated_firmware=allow_simulated_firmware)
    bound_host, bound_port = server.server_address[:2]
    print(
        f"{config.APP_NAME} {config.APP_VERSION} listening on http://{bound_host}:{bound_port}",
        flush=True,
    )
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("stopping", flush=True)
    finally:
        server.server_close()
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Remote Print Gateway minimal HTTP API (V0.1)"
    )
    parser.add_argument("--host", default=config.HTTP_HOST)
    parser.add_argument("--port", type=int, default=config.HTTP_PORT)
    parser.add_argument(
        "--allow-simulated-firmware",
        action="store_true",
        help="demo/test only: expose the simulated firmware fixture",
    )
    args = parser.parse_args(argv)
    allow = True if args.allow_simulated_firmware else None
    return serve(args.host, args.port, allow_simulated_firmware=allow)


if __name__ == "__main__":
    raise SystemExit(main())

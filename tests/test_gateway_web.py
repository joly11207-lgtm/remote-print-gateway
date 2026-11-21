"""Web UI smoke tests for Website V1 (multi-page static site + the calls it makes).

No browser automation dependency: this checks that the server serves every page,
that each page declares the same page id app.js dispatches on, that a deep link
and a refresh hit the same file, that every API path referenced by app.js really
exists, and that the exact call sequence the page performs produces the expected
engine states.

Usage: python tests/test_gateway_web.py
"""

from __future__ import annotations

import json
import os
import re
import sys
import tempfile
import threading
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
os.environ["RTB_GATEWAY_DB"] = str(Path(tempfile.mkdtemp(prefix="rtb-web-")) / "gateway.db")
os.environ.pop("RTB_AI_ENDPOINT", None)
os.environ.pop("RTB_ALLOW_SIMULATED_FIRMWARE", None)

from gateway import api, db, engine, simulated_firmware  # noqa: E402

WEB_DIR = ROOT / "web"
GATEWAY_ID = db.GATEWAY["gateway_id"]
API_REF = re.compile(r"[`'\"](/api/[^`'\"]+)[`'\"]")
TEMPLATE = re.compile(r"\$\{[^}]*\}")

# route -> (file, data-page). Real HTML paths, so a refresh is just a GET.
PAGES: dict[str, tuple[str, str]] = {
    "/": ("index.html", "home"),
    "/setup": ("setup/index.html", "setup"),
    "/lab": ("lab/index.html", "lab"),
    "/printers": ("printers/index.html", "printers"),
    "/printers/add": ("printers/add/index.html", "add"),
    "/print": ("print/index.html", "print"),
    "/compatibility": ("compatibility/index.html", "compatibility"),
}
DEEP_LINKS = [route for route in PAGES if route != "/"]


def section(title: str) -> None:
    print(f"\n== {title} ==")


class Server:
    def __init__(self, allow_simulated_firmware: bool = False) -> None:
        self._server = api.make_server(
            "127.0.0.1", 0, allow_simulated_firmware=allow_simulated_firmware
        )
        self._server.daemon_threads = True
        threading.Thread(target=self._server.serve_forever, daemon=True).start()
        self.base = f"http://127.0.0.1:{self._server.server_address[1]}"

    def get(self, path: str) -> tuple[int, str, bytes]:
        request = urllib.request.Request(self.base + path, method="GET")
        try:
            with urllib.request.urlopen(request, timeout=10) as response:
                return response.status, response.headers.get("Content-Type", ""), response.read()
        except urllib.error.HTTPError as error:
            return error.code, error.headers.get("Content-Type", ""), error.read()

    def post(self, path: str, body: dict | None = None) -> tuple[int, dict]:
        data = None if body is None else json.dumps(body).encode("utf-8")
        request = urllib.request.Request(self.base + path, data=data, method="POST")
        if data is not None:
            request.add_header("Content-Type", "application/json")
        try:
            with urllib.request.urlopen(request, timeout=10) as response:
                return response.status, json.loads(response.read().decode("utf-8"))
        except urllib.error.HTTPError as error:
            return error.code, json.loads(error.read().decode("utf-8"))

    def close(self) -> None:
        self._server.shutdown()
        self._server.server_close()


def page_files() -> list[tuple[str, str, str]]:
    return [(route, name, page) for route, (name, page) in PAGES.items()]


def check_static(server: Server) -> None:
    section("every V1 page is served, including a direct refresh of a deep link")

    for route, (name, page) in PAGES.items():
        path = WEB_DIR / name
        assert path.is_file(), f"missing page file for {route}: {path}"

        for request_path in {route, route + "/"}:
            status, content_type, body = server.get(request_path)
            assert status == 200, (request_path, status)
            assert content_type.startswith("text/html"), (request_path, content_type)
            html = body.decode("utf-8")
            assert f'data-page="{page}"' in html, (request_path, page)
            assert 'data-demo="1"' in html, (request_path, "explicit demo build marker")
            assert "Remote Print Gateway" in html, request_path
            assert 'src="/app.js"' in html and 'href="/styles.css"' in html, request_path
            assert "http://" not in html and "https://" not in html, request_path
            assert "react" not in html.lower() and "vue" not in html.lower(), request_path
        print(f"  GET {route:<18} 200 text/html  data-page={page}")

    for path, expected_type, expected_text in (
        ("/app.js", "application/javascript", "compatibility/cases"),
        ("/styles.css", "text/css", ":root"),
    ):
        status, content_type, body = server.get(path)
        assert status == 200, (path, status)
        assert content_type.startswith(expected_type), (path, content_type)
        assert expected_text in body.decode("utf-8"), path
        print(f"  GET {path:<18} 200 {content_type}")

    status, content_type, body = server.get("/index.html")
    assert status == 200 and "Add Printer" in body.decode("utf-8"), status
    print("  GET /index.html      200 (legacy path still works)")

    status, content_type, _ = server.get("/gateway/api.py")
    assert status == 404 and content_type.startswith("application/json"), status
    print("  unknown path         404 (no file disclosure)")


def check_navigation() -> None:
    section("shared navigation points at pages that exist")

    for route, (name, _page) in PAGES.items():
        html = (WEB_DIR / name).read_text(encoding="utf-8")
        for target in PAGES:
            assert f'href="{target}"' in html, (route, target)
        active = 'class="is-active"'
        assert html.count(active) == 1, f"{route}: exactly one active nav item"
    print(f"  {len(PAGES)} pages, {len(PAGES)} nav targets each, one active item per page")


def check_page_dispatch() -> None:
    section("data-page values match the initialisers in app.js")

    source = (WEB_DIR / "app.js").read_text(encoding="utf-8")
    block = re.search(r"const PAGE_INIT = \{(.*?)\n\};", source, re.S)
    assert block, "app.js has no PAGE_INIT map"
    keys = set(re.findall(r"^\s*(\w+):", block.group(1), re.M))
    expected = {page for _name, page in PAGES.values()}
    assert keys == expected, f"PAGE_INIT {keys} != data-page {expected}"

    assert "const PAGE = document.body.dataset.page" in source
    assert "const init = PAGE_INIT[PAGE];" in source
    print(f"  PAGE_INIT keys: {', '.join(sorted(keys))}")


def check_referenced_paths(server: Server) -> None:
    section("every API path used by app.js exists on the server")

    source = (WEB_DIR / "app.js").read_text(encoding="utf-8")
    referenced = []
    for match in API_REF.finditer(source):
        path = TEMPLATE.sub("x", match.group(1))
        if path.endswith("/"):
            continue
        if path not in referenced:
            referenced.append(path)
    assert referenced, "app.js references no API paths?"
    assert "/api/gateways/x" in referenced
    assert "/api/compatibility/cases" in referenced

    for path in sorted(referenced):
        matched = any(pattern.match(path) for _method, pattern, _name in api.ROUTES)
        assert matched, f"app.js calls a path the server does not serve: {path}"
    print(f"  {len(referenced)} referenced paths, all routed:")
    for path in sorted(referenced):
        print(f"    {path}")


def check_ui_copy() -> None:
    section("consumer copy + no escape hatches in the UI source")

    source = (WEB_DIR / "app.js").read_text(encoding="utf-8")
    html = "\n".join(
        (WEB_DIR / name).read_text(encoding="utf-8") for name, _page in PAGES.values()
    )

    required = [
        "Good news — your printer looks compatible.",
        "No Gateway software update is required.",
        "Test compatibility",
        "Compatibility test passed.",
        "We recognize this printer.",
        "Your Gateway does not yet have the software needed",
        "We couldn't safely identify this printer yet.",
        "Your printer information has been recorded",
        "Advanced details",
        "Demo / Simulator",
        "Coming soon",
    ]
    missing = [text for text in required if text not in source + html]
    assert not missing, f"missing UI copy: {missing}"

    forbidden = ["force test", "Force test", "Try anyway", "Send command", "send command"]
    found = [text for text in forbidden if text in source]
    assert not found, f"UI must not offer these: {found}"

    # The test button may only be produced when the engine says it is executable.
    assert "safeTest.executable" in source
    assert "executable ? testButton(report) : null" in source

    # The user's connection answer is a hint, not a transport.
    assert "connection_hint" in source
    assert "expected_connection_type" not in source

    # V0.2: the browser never chooses the safe-test result and never holds the
    # gateway address. It only asks this origin which gateway to use.
    assert "outcome:" not in source, "the UI must not send a test outcome"
    assert "mode === 'real'" in source, "the UI must prefer the real gateway"
    assert "Gateway is offline" in source
    assert "couldn't find your printer" in source

    # No framework, no CDN, no build step: everything comes from this origin.
    for document in (source, html):
        assert "http://" not in document and "https://" not in document
        assert "react" not in document.lower() and "vue" not in document.lower()
    print(f"  {len(required)} required copy strings, no force-test paths, no external assets")


def check_deploy_copy() -> None:
    section("production copy: profile facts, brand-neutral setup, demo fallback")

    pages = {route: (WEB_DIR / name).read_text(encoding="utf-8") for route, (name, _p) in PAGES.items()}
    source = (WEB_DIR / "app.js").read_text(encoding="utf-8")

    # G2D is a Bluetooth Classic / SPP printer in gateway/profiles/memobird_g2d.json.
    compatibility = pages["/compatibility"]
    assert "Bluetooth Classic · Memobird" in compatibility
    assert "Bluetooth LE · Memobird" not in compatibility

    # Setup must not pin the product to one specific MCU.
    assert "Plug your Gateway board into USB" in pages["/setup"]
    for html in pages.values():
        assert "ESP32" not in html, "the website must not name the ESP32-S3"

    # A static deployment has no engine: that state is demo copy, not an error.
    assert "Gateway simulator ready" in source
    assert "This deployment is not connected to a real Gateway." in source
    assert "const DEMO_BUILD = document.body.dataset.demo === '1';" in source
    assert "DEMO_BUILD && noBackend(error)" in source
    # Genuine failures keep the error path.
    assert "showError($('gateway-error'), error)" in source
    assert "error.code === 'BACKEND_UNAVAILABLE' ? 'Gateway service offline'" in source
    print("  G2D = Bluetooth Classic, setup is brand-neutral, demo fallback is gated")


def check_dom_wiring() -> None:
    section("app.js only touches elements that exist in the markup")

    ids: set[str] = set()
    buttons: set[str] = set()
    for name, _page in PAGES.values():
        html = (WEB_DIR / name).read_text(encoding="utf-8")
        ids |= set(re.findall(r'id="([^"]+)"', html))
        buttons |= set(re.findall(r'<button[^>]*\bid="([^"]+)"', html))

    source = (WEB_DIR / "app.js").read_text(encoding="utf-8")
    used = set(re.findall(r"\$\('([^']+)'\)", source))

    missing = sorted(used - ids)
    assert not missing, f"app.js references missing ids: {missing}"

    # A button with an id is a button the script is expected to wire up.
    unhandled = sorted(buttons - used)
    assert not unhandled, f"buttons with an id but no handler in app.js: {unhandled}"

    # device fixture buttons are bound through the data attribute, not an id
    assert 'name="connection"' in "\n".join(
        (WEB_DIR / name).read_text(encoding="utf-8") for name, _page in PAGES.values()
    )
    assert "data-device" in "\n".join(
        (WEB_DIR / name).read_text(encoding="utf-8") for name, _page in PAGES.values()
    )
    print(f"  {len(used)} element ids wired, all present; {len(buttons)} buttons handled")


def check_vercel_config() -> None:
    section("Vercel static config covers every deep link")

    path = ROOT / "vercel.json"
    assert path.is_file(), "vercel.json is missing"
    config = json.loads(path.read_text(encoding="utf-8"))

    assert config.get("outputDirectory") == "web", config.get("outputDirectory")
    assert config.get("buildCommand") in ("", None), config.get("buildCommand")
    assert config.get("framework", "unset") is None, config.get("framework")
    assert (ROOT / config["outputDirectory"]).is_dir()

    rewrites = {item["source"]: item["destination"] for item in config.get("rewrites", [])}
    for route in DEEP_LINKS:
        assert route in rewrites, f"no rewrite for {route}"
        destination = WEB_DIR / rewrites[route].lstrip("/")
        assert destination.is_file(), f"rewrite for {route} points at {destination}"
        assert rewrites[route] == f"{route}/index.html", rewrites[route]
    print(f"  outputDirectory=web, buildCommand=none, {len(rewrites)} deep-link rewrites")


def check_ui_flow_x18(server: Server) -> None:
    section("X18 flow (same calls the page makes)")

    body = {
        "gateway_id": GATEWAY_ID,
        "brand": "爱记星",
        "model": "X18",
        "original_app": "爱记星",
        "connection_hint": "bluetooth",
        "device_key": "x18",
    }
    status, created = server.post("/api/compatibility/cases", body)
    assert status == 200, created
    case_id = created["case_id"]

    status, report = server.post(f"/api/compatibility/cases/{case_id}/scan")
    assert status == 200, report
    assert report["status"] == engine.READY_FOR_TEST
    assert report["recommended_safe_test"]["executable"] is True
    print(f"  scan     -> {report['status']} ({report['candidate_profile']})")

    status, tested = server.post(f"/api/compatibility/cases/{case_id}/test", {"outcome": "pass"})
    assert status == 200 and tested["status"] == engine.TEST_PASSED, tested
    print(f"  test     -> {tested['status']}")

    status, completed = server.post(f"/api/compatibility/cases/{case_id}/complete")
    assert status == 200 and completed["status"] == engine.COMPLETED, completed
    print(f"  complete -> {completed['status']} printer={completed['printer_id']}")


def check_ui_flow_g2d(server: Server) -> str:
    section("G2D flow stops at WAITING_FIRMWARE (no test button)")

    status, created = server.post(
        "/api/compatibility/cases", {"gateway_id": GATEWAY_ID, "device_key": "g2d"}
    )
    assert status == 200, created
    case_id = created["case_id"]

    status, report = server.post(f"/api/compatibility/cases/{case_id}/scan")
    assert status == 200, report
    assert report["status"] == engine.WAITING_FIRMWARE
    assert report["candidate_protocol"] == "memobird"
    assert report["recommended_safe_test"]["executable"] is False
    assert report["firmware_available"] is False
    print(f"  scan     -> {report['status']} executable=false")

    status, body = server.post(f"/api/compatibility/cases/{case_id}/test", {"outcome": "pass"})
    assert status == 409 and body["error"] == "INVALID_TRANSITION", body
    print("  test     -> 409 (UI would never show the button)")
    return case_id


def check_ui_flow_unknown(server: Server) -> None:
    section("Unknown flow stops at BLOCKED (no test / no add)")

    status, created = server.post(
        "/api/compatibility/cases",
        {"gateway_id": GATEWAY_ID, "brand": "XP", "model": "XP-460B", "connection_hint": "unknown"},
    )
    assert status == 200, created
    case_id = created["case_id"]

    status, report = server.post(f"/api/compatibility/cases/{case_id}/scan")
    assert status == 200, report
    assert report["status"] == engine.BLOCKED
    assert report["recommended_safe_test"] is None
    print(f"  scan     -> {report['status']} safe_test=null")

    for action in ("test", "complete"):
        status, body = server.post(f"/api/compatibility/cases/{case_id}/{action}")
        assert status == 409, (action, body)
    print("  test/complete -> 409")


def check_dev_gating(case_id: str) -> None:
    section("dev fixtures are gated")

    production = Server()
    try:
        status, body = production.post("/api/dev/simulated-firmware", {"installed": True})
        assert status == 403 and body["error"] == "SIMULATION_DISABLED", body
        status, body = production.post("/api/dev/gateway-upgrade", {})
        assert status == 403, body
        print("  production server -> 403 SIMULATION_DISABLED")
    finally:
        production.close()

    demo = Server(allow_simulated_firmware=True)
    try:
        status, body = demo.post("/api/dev/simulated-firmware", {"installed": True})
        assert status == 200 and body["installed"] is True and body["simulated"] is True, body
        print(f"  dev server -> fixture installed ({body['firmware_version']}/{body['channel']})")

        status, report = demo.post(
            f"/api/compatibility/cases/{case_id}/refresh-capability",
            {"include_simulated_firmware": True},
        )
        assert status == 200, report
        assert report["minimum_firmware_version"] == "1.3.0-beta.1"
        assert report["minimum_firmware_simulated"] is True
        assert report["next_action"] == engine.NEXT_UPDATE_FIRMWARE
        print(f"  refresh  -> {report['status']} next={report['next_action']}")

        status, body = demo.post("/api/dev/gateway-upgrade", {})
        assert status == 200 and body["simulated"] is True, body
        status, upgraded = demo.post(f"/api/compatibility/cases/{case_id}/refresh-capability", {})
        assert status == 200 and upgraded["status"] == engine.READY_FOR_TEST, upgraded
        assert upgraded["safe_test_executable"] is True
        print(f"  upgrade  -> {upgraded['status']} next={upgraded['next_action']}")
    finally:
        demo.close()

    conn = db.connect()
    try:
        runs = conn.execute(
            "SELECT COUNT(*) AS n FROM safe_test_runs WHERE case_id = ?", (case_id,)
        ).fetchone()["n"]
        assert runs == 0, "CMD 0x01 must never be executed"
        simulated_firmware.uninstall(conn)
    finally:
        conn.close()
    print("  CMD 0x01 never executed")


def main() -> int:
    db.initialize()
    server = Server()
    try:
        check_static(server)
        check_navigation()
        check_page_dispatch()
        check_referenced_paths(server)
        check_dom_wiring()
        check_ui_copy()
        check_deploy_copy()
        check_vercel_config()
        check_ui_flow_x18(server)
        g2d_case = check_ui_flow_g2d(server)
        check_ui_flow_unknown(server)
    finally:
        server.close()

    check_dev_gating(g2d_case)

    pages = ", ".join(PAGES)
    print(f"\nPASS: Web V1 ({len(PAGES)} pages: {pages}; X18/G2D/unknown flows; Vercel deep links)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

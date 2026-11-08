"""REAL-HARDWARE ONLY probe for the BLE scan lifecycle regression.

This is deliberately NOT a host Mock test: the ESP32 Arduino/Bluedroid BLE
stack is compiled out of host builds (RTB_ENABLE_BLE == 0), so a Mock test
cannot reproduce or verify scanner state. Running this script requires:
  - an ESP32-S3 flashed with the firmware under test, in AP mode
  - the PC joined to RTB-V0.1-AP (gateway 192.168.4.1)
  - at least one BLE advertiser nearby (a powered-on printer)

Usage:
  python tests/hardware/scan_lifecycle_probe.py [base_url]

Regression covered (no printing, no paper used):
  1. scan 1 discovers advertisements
  2. connect -> disconnect
  3. scan 2 STILL discovers advertisements
  4. repeated scans stay healthy
  5. connect-first ordering: after a COLD BOOT, if /api/connect is the first
     BLE call, a later /api/scan must still discover advertisements.

Exit code 0 = all checks passed; 1 = regression present.
"""
from __future__ import annotations

import json
import sys
import time
import urllib.request

BASE = sys.argv[1] if len(sys.argv) > 1 else "http://192.168.4.1"
PRINTER_ADDRESS = "e3:47:88:2e:e1:2c"
MODEL_ID = "x18_aijixing"


def call(method: str, path: str, body=None, timeout: float = 60.0):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(
        BASE + path, data=data,
        headers={"Content-Type": "application/json"}, method=method)
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.status, r.read().decode("utf-8", "replace")


def scan(tag: str, count: int = 3) -> bool:
    ok = True
    for i in range(count):
        try:
            _, raw = call("GET", "/api/scan")
            eps = json.loads(raw).get("endpoints", [])
            good = len(eps) > 0
            ok = ok and good
            print("%s #%d: endpoints=%d %s" % (
                tag, i + 1, len(eps), "OK" if good else "FAIL"))
        except Exception as exc:  # noqa: BLE001 - hardware probe reports errors
            ok = False
            print("%s #%d: ERROR %r" % (tag, i + 1, exc))
        time.sleep(0.5)
    return ok


def main() -> int:
    print("BLE scan lifecycle probe (REAL HARDWARE); base=%s" % BASE)
    try:
        _, status = call("GET", "/api/status")
        print("status: %s" % status)
    except Exception as exc:  # noqa: BLE001
        print("board unreachable: %r" % exc)
        return 1

    results = {}
    results["scan_before"] = scan("scan-before", 3)

    try:
        print("connect: %s" % call("POST", "/api/connect", {
            "address": PRINTER_ADDRESS, "model_id": MODEL_ID})[1])
        print("disconnect: %s" % call("POST", "/api/disconnect", {})[1])
    except Exception as exc:  # noqa: BLE001
        print("connect/disconnect ERROR %r" % exc)
        return 1

    results["scan_after"] = scan("scan-after", 3)
    results["scan_repeated"] = scan("scan-repeated", 4)

    print("\nNOTE: the 'connect is the FIRST BLE operation after a cold boot'")
    print("case needs a power cycle and is recorded separately in")
    print("docs/HARDWARE_TEST_PLAN.md; it cannot be triggered over HTTP alone.")

    passed = all(results.values())
    print("\nRESULT: %s %s" % ("PASS" if passed else "FAIL", results))
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())

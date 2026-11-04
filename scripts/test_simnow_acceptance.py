#!/usr/bin/env python3
"""Hermetic smoke test for scripts/simnow_acceptance.py.

Run:
    python3 scripts/test_simnow_acceptance.py
"""

from __future__ import annotations

import http.server
import json
import subprocess
import sys
import tempfile
import threading
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "scripts" / "simnow_acceptance.py"


class FakeState:
    def __init__(self) -> None:
        self.orders: dict[str, dict] = {}
        self.next_id = 1
        self.quote_requests = 0


def fake_quote() -> dict:
    quote = {
        "instrument_id": "SHFE.rb2701",
        "last_price": 3000.0,
        "price_tick": 1.0,
        "lower_limit": 2800.0,
        "upper_limit": 3200.0,
    }
    for level in range(1, 6):
        quote[f"bid_price{level}"] = 3001.0 - level
        quote[f"ask_price{level}"] = 3000.0 + level
        quote[f"bid_volume{level}"] = 10
        quote[f"ask_volume{level}"] = 11
    return quote


def make_handler(state: FakeState):
    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *_args):
            pass

        def send_json(self, status: int, value) -> None:
            payload = json.dumps(value).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

        def do_GET(self):
            if self.path == "/healthz":
                return self.send_json(200, {"ok": True})
            if self.path == "/readyz":
                return self.send_json(200, {"ready": True, "md": True, "td": True})
            if self.path == "/account":
                return self.send_json(200, {"balance": 1_000_000.0, "available": 900_000.0})
            if self.path == "/positions":
                return self.send_json(200, {"positions": {}})
            if self.path == "/orders":
                return self.send_json(200, {"orders": state.orders})
            if self.path == "/trades":
                return self.send_json(200, {"trades": {}})
            if self.path.startswith("/quote/"):
                state.quote_requests += 1
                # First two requests model node triggering a new subscription.
                if state.quote_requests <= 2:
                    return self.send_json(200, {"instrument_id": "SHFE.rb2701"})
                return self.send_json(200, fake_quote())
            if self.path == "/metrics":
                return self.send_json(200, {"ready": True})
            return self.send_json(404, {"error": "not found"})

        def read_body(self):
            length = int(self.headers.get("Content-Length", "0"))
            return json.loads(self.rfile.read(length) or b"{}")

        def do_POST(self):
            if self.path != "/order":
                return self.send_json(404, {"error": "not found"})
            body = self.read_body()
            if int(body.get("volume", 0)) <= 0:
                return self.send_json(200, {"success": False, "order_id": "", "error": "volume must be positive"})
            order_id = f"fake-{state.next_id}"
            state.next_id += 1
            state.orders[order_id] = {
                "order_id": order_id,
                "instrument_id": body.get("instrument_id"),
                "direction": body.get("direction"),
                "offset": body.get("offset"),
                "volume_orign": body.get("volume", 0),
                "volume_left": body.get("volume", 0),
                "status": "ALIVE",
                "is_dead": False,
                "is_error": False,
                "last_msg": "",
                "limit_price": body.get("limit_price"),
            }
            return self.send_json(200, {"success": True, "order_id": order_id, "error": ""})

        def do_DELETE(self):
            prefix = "/order/"
            if not self.path.startswith(prefix):
                return self.send_json(404, {"error": "not found"})
            order_id = self.path[len(prefix):]
            order = state.orders.get(order_id)
            if order is None:
                return self.send_json(404, {"success": False, "order_id": order_id, "error": "missing"})
            order["status"] = "FINISHED"
            order["is_dead"] = True
            order["last_msg"] = "cancelled by acceptance test"
            return self.send_json(200, {"success": True, "order_id": order_id, "error": ""})

    return Handler


class AcceptanceScriptTest(unittest.TestCase):
    def test_read_and_trade_flow(self):
        state = FakeState()
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), make_handler(state))
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            with tempfile.TemporaryDirectory() as directory:
                json_out = Path(directory) / "report.json"
                md_out = Path(directory) / "report.md"
                result = subprocess.run(
                    [
                        sys.executable,
                        str(SCRIPT),
                        "--node-url",
                        f"http://127.0.0.1:{server.server_port}",
                        "--symbol",
                        "SHFE.rb2701",
                        "--orders",
                        "--yes",
                        "--include-market",
                        "--include-counterparty",
                        "--no-ws",
                        "--timeout",
                        "1",
                        "--ready-timeout",
                        "2",
                        "--order-timeout",
                        "2",
                        "--json-out",
                        str(json_out),
                        "--md-out",
                        str(md_out),
                    ],
                    cwd=ROOT,
                    text=True,
                    capture_output=True,
                )
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                report = json.loads(json_out.read_text())
                self.assertEqual(report["summary"]["FAIL"], 0)
                names = {item["name"] for item in report["checks"]}
                self.assertIn("orders.limit.cancel", names)
                self.assertIn("orders.reject", names)
                self.assertIn("orders.market", names)
                self.assertIn("orders.counterparty", names)
                self.assertTrue(md_out.exists())
        finally:
            server.shutdown()
            server.server_close()


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""Two-process, two-account acceptance test using two Sim gateways.

This is a real process-level check (not just the in-process HTTP unit test):
  gw A (Sim) <--IPC--> node -- HTTP/WS --> client
  gw B (Sim) <--IPC--> node (extra account)

It verifies /accounts, order routing, position isolation, per-account
task/risk and `/stream?account=`.
"""

from __future__ import annotations

import argparse
import asyncio
import http.client
import json
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SYMBOL = "SHFE.rb2701"


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def http_json(port: int, method: str, path: str, body: dict | None = None) -> tuple[int, dict]:
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
    headers = {"Accept": "application/json"}
    payload = None
    if body is not None:
        headers["Content-Type"] = "application/json"
        payload = json.dumps(body)
    try:
        conn.request(method, path, body=payload, headers=headers)
        response = conn.getresponse()
        raw = response.read()
        return response.status, json.loads(raw) if raw else {}
    finally:
        conn.close()


def wait_http(port: int, timeout: float = 20.0) -> None:
    deadline = time.monotonic() + timeout
    last: Exception | None = None
    while time.monotonic() < deadline:
        try:
            status, _ = http_json(port, "GET", "/healthz")
            if status == 200:
                return
        except Exception as exc:  # noqa: BLE001 - retry transient startup errors
            last = exc
        time.sleep(0.2)
    raise RuntimeError(f"node HTTP not ready on {port}: {last}")


def wait_accounts(port: int, timeout: float = 20.0) -> dict:
    deadline = time.monotonic() + timeout
    last: dict = {}
    while time.monotonic() < deadline:
        try:
            status, body = http_json(port, "GET", "/accounts")
            last = body
            ids = {item["account_id"] for item in body.get("accounts", [])}
            if status == 200 and ids == {"A", "B"}:
                return body
        except Exception:  # noqa: BLE001 - retry while extras connect
            pass
        time.sleep(0.2)
    raise RuntimeError(f"accounts not available on {port}: {last}")


def wait_port(port: int, timeout: float = 10.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return
        except OSError:
            time.sleep(0.1)
    raise RuntimeError(f"port {port} did not open")


def gw_config(port: int, balance: float) -> str:
    return f"""
[gateway]
ipc = "gw"

[ipc.gw]
name = "gw-{port}"
transport = "tcp"
tcp_host = "127.0.0.1"
tcp_port = {port}
encoding = "json"
handshake_timeout_ms = 5000
heartbeat_interval_ms = 1000
heartbeat_timeout_ms = 5000

[md]
kind = "none"

[td]
kind = "sim"
[td.sim]
initial_balance = {balance}
[td.sim.specs."{SYMBOL}"]
volume_multiple = 10.0
price_tick = 1.0
margin_rate = 0.1
commission_per_lot = 2.0
[td.sim.last_prices]
"{SYMBOL}" = 3800.0

[runtime]
threads = 1
""".strip()


def node_config(serve_port: int, md_port: int, extra_port: int) -> str:
    return f"""
primary_account = "A"

[[accounts]]
id = "A"
ipc = "gw-a"

[[accounts]]
id = "B"
ipc = "gw-b"

[ipc.gw-a]
name = "node-multi-account-a"
transport = "tcp"
tcp_host = "127.0.0.1"
tcp_port = {md_port}
encoding = "json"
handshake_timeout_ms = 5000
heartbeat_interval_ms = 1000
heartbeat_timeout_ms = 5000

[ipc.gw-b]
name = "node-multi-account-b"
transport = "tcp"
tcp_host = "127.0.0.1"
tcp_port = {extra_port}
encoding = "json"
handshake_timeout_ms = 5000
heartbeat_interval_ms = 1000
heartbeat_timeout_ms = 5000

[serve]
bind = "127.0.0.1"
port = {serve_port}
symbols = []

[serve.ws]
path = "/stream"
heartbeat_interval_ms = 30000
max_clients = 32
max_queue = 256

[cache.instruments]
enabled = false

[cache.kline]
enabled = false

[cache.tick]
enabled = false

[runtime]
threads = 1
""".strip()


async def ws_snapshot(port: int, account: str) -> dict:
    import websockets

    url = f"ws://127.0.0.1:{port}/stream?account={account}"
    async with websockets.connect(url, open_timeout=5) as socket:
        await socket.recv()  # welcome
        await socket.send(
            json.dumps(
                {
                    "op": "subscribe",
                    "symbols": [],
                    "klines": [],
                    "topics": ["account", "positions", "orders", "trades"],
                }
            )
        )
        for _ in range(12):
            message = json.loads(await asyncio.wait_for(socket.recv(), timeout=5))
            if message.get("type") == "snapshot":
                return message.get("data", {})
    raise RuntimeError(f"no snapshot for account {account}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin-dir", default=str(ROOT / "build-ui"))
    parser.add_argument("--keep", action="store_true")
    args = parser.parse_args()

    bin_dir = Path(args.bin_dir)
    gw_bin = bin_dir / "tqsdk-gw"
    node_bin = bin_dir / "tqsdk-node"
    for binary in (gw_bin, node_bin):
        if not binary.exists():
            raise SystemExit(f"missing binary: {binary}")

    work = Path(tempfile.mkdtemp(prefix="tqsdk-multi-account-"))
    gw_a_port = free_port()
    gw_b_port = free_port()
    node_port = free_port()
    (work / "gw-a.toml").write_text(gw_config(gw_a_port, 1_000_000.0))
    (work / "gw-b.toml").write_text(gw_config(gw_b_port, 2_000_000.0))
    (work / "node.toml").write_text(node_config(node_port, gw_a_port, gw_b_port))

    logs = {
        "gw-a": (work / "gw-a.log").open("w", encoding="utf-8"),
        "gw-b": (work / "gw-b.log").open("w", encoding="utf-8"),
        "node": (work / "node.log").open("w", encoding="utf-8"),
    }
    procs: list[subprocess.Popen] = []
    try:
        procs.append(
            subprocess.Popen([str(gw_bin), "-c", str(work / "gw-a.toml")], stdout=logs["gw-a"], stderr=subprocess.STDOUT, cwd=ROOT)
        )
        procs.append(
            subprocess.Popen([str(gw_bin), "-c", str(work / "gw-b.toml")], stdout=logs["gw-b"], stderr=subprocess.STDOUT, cwd=ROOT)
        )
        wait_port(gw_a_port)
        wait_port(gw_b_port)
        procs.append(
            subprocess.Popen([str(node_bin), "-c", str(work / "node.toml")], stdout=logs["node"], stderr=subprocess.STDOUT, cwd=ROOT)
        )
        wait_http(node_port)

        accounts = wait_accounts(node_port)
        ids = sorted(item["account_id"] for item in accounts["accounts"])
        assert ids == ["A", "B"], ids
        assert accounts["default"] == "A", accounts

        order = {
            "instrument_id": SYMBOL,
            "direction": "BUY",
            "offset": "OPEN",
            "price_type": "LIMIT",
            "limit_price": 3800.0,
            "volume": 1,
        }
        for account in ("A", "B"):
            status, body = http_json(node_port, "POST", f"/order?account={account}", order)
            assert status == 200 and body.get("success"), (account, status, body)

        status, positions_a = http_json(node_port, "GET", "/positions?account=A")
        status_b, positions_b = http_json(node_port, "GET", "/positions?account=B")
        assert status == 200 and status_b == 200
        assert positions_a["positions"][SYMBOL]["volume_long"] == 1, positions_a
        assert positions_b["positions"][SYMBOL]["volume_long"] == 1, positions_b

        status, created = http_json(
            node_port, "POST", "/task/target?account=B",
            {"symbol": SYMBOL, "target_volume": 0, "mode": "ACTIVE"},
        )
        assert status == 200 and created.get("success"), created
        _, tasks_b = http_json(node_port, "GET", "/tasks?account=B")
        _, tasks_a = http_json(node_port, "GET", "/tasks?account=A")
        assert any(task["task_id"] == created["task_id"] for task in tasks_b["targets"]), tasks_b
        assert not any(task["task_id"] == created["task_id"] for task in tasks_a["targets"]), tasks_a

        status, risk = http_json(
            node_port, "POST", "/risk/rules?account=B",
            {"type": "ORDER_RATE_LIMIT", "limit": 2, "window_ms": 1000},
        )
        assert status == 200 and risk["rules"] >= 1, risk
        _, risk_a = http_json(node_port, "GET", "/risk/status?account=A")
        assert risk_a["rules"] == 0, risk_a

        snapshot_a = asyncio.run(ws_snapshot(node_port, "A"))
        snapshot_b = asyncio.run(ws_snapshot(node_port, "B"))
        assert snapshot_a["positions"][SYMBOL]["volume_long"] == 1, snapshot_a
        assert snapshot_b["positions"][SYMBOL]["volume_long"] == 1, snapshot_b

        print(json.dumps({"ok": True, "accounts": ids, "default": accounts["default"], "work": str(work)}, indent=2))
        return 0
    finally:
        for proc in reversed(procs):
            proc.terminate()
        for proc in reversed(procs):
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
        for log in logs.values():
            log.close()
        if args.keep:
            print(f"kept work dir: {work}", file=sys.stderr)


if __name__ == "__main__":
    raise SystemExit(main())

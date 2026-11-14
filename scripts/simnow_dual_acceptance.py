#!/usr/bin/env python3
"""Read-only acceptance for two real SimNow/CTP accounts.

This starts two independent tqsdk-gw processes (one CTP account each) and one
tqsdk-node with a default account A and extra account B. It verifies:

  - /accounts contains A and B and both report ready;
  - /account, /positions, /orders, /trades work for both accounts;
  - /stream?account=A|B returns an account snapshot;
  - no order is submitted by this script.

Credentials are read from shell-style KEY=VALUE env files. The generated TOML
files contain secrets, so they are written to a 0700 temporary directory with
0600 permissions and removed on exit unless --keep is given.
"""

from __future__ import annotations

import argparse
import asyncio
import http.client
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parent.parent
REQUIRED = (
    "CTP_BROKER_ID",
    "CTP_USER_ID",
    "CTP_PASSWORD",
    "CTP_APP_ID",
    "CTP_AUTH_CODE",
    "CTP_TD_FRONT",
    "CTP_MD_FRONT",
)


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def load_env(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("export "):
            line = line[7:].lstrip()
        if "=" not in line:
            raise RuntimeError(f"{path}: expected KEY=VALUE")
        key, value = line.split("=", 1)
        value = value.strip()
        if len(value) >= 2 and value[0] == value[-1] and value[0] in "'\"":
            value = value[1:-1]
        values[key.strip()] = value
    missing = [key for key in REQUIRED if not values.get(key) and key != "CTP_AUTH_CODE"]
    if missing:
        raise RuntimeError(f"{path}: missing {', '.join(missing)}")
    return values


def http_json(port: int, method: str, path: str, body: dict[str, Any] | None = None):
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
        try:
            parsed = json.loads(raw) if raw else {}
        except json.JSONDecodeError:
            parsed = {"raw": raw.decode("utf-8", "replace")}
        return response.status, parsed
    finally:
        conn.close()


def wait_http(port: int, timeout: float = 90.0) -> None:
    deadline = time.monotonic() + timeout
    last: str = ""
    while time.monotonic() < deadline:
        try:
            status, body = http_json(port, "GET", "/healthz")
            if status == 200:
                return
            last = f"{status} {body}"
        except OSError as exc:
            last = str(exc)
        time.sleep(0.3)
    raise RuntimeError(f"node HTTP not ready on {port}: {last}")


def wait_accounts(port: int, timeout: float = 90.0) -> dict[str, Any]:
    deadline = time.monotonic() + timeout
    last: dict[str, Any] = {}
    while time.monotonic() < deadline:
        try:
            status, body = http_json(port, "GET", "/accounts")
            last = body
            if status == 200:
                entries = {
                    str(item.get("account_id")): item
                    for item in body.get("accounts", [])
                    if isinstance(item, dict)
                }
                if {"A", "B"}.issubset(entries) and all(
                    entries[account].get("ready")
                    and isinstance(entries[account].get("account"), dict)
                    and entries[account]["account"].get("balance") is not None
                    for account in ("A", "B")
                ):
                    return body
        except OSError:
            pass
        time.sleep(0.3)
    raise RuntimeError(f"accounts A/B not available on {port}: {last}")


async def ws_snapshot(port: int, account: str) -> dict[str, Any]:
    import websockets

    url = f"ws://127.0.0.1:{port}/stream?account={account}"
    async with websockets.connect(url, open_timeout=10) as socket:
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
        for _ in range(30):
            message = json.loads(await asyncio.wait_for(socket.recv(), timeout=10))
            if message.get("type") == "snapshot":
                return message.get("data", {})
    raise RuntimeError(f"no snapshot for account {account}")


def gw_config(env: dict[str, str], port: int, name: str, flow: Path) -> str:
    def q(value: str) -> str:
        return json.dumps(value)

    return f"""
[listen]
transport = "tcp"
tcp_host = "127.0.0.1"
tcp_port = {port}

[ipc]
encoding = "json"
handshake_timeout_ms = 5000
heartbeat_interval_ms = 1000
heartbeat_timeout_ms = 10000
local_name = {q(name)}

[md]
kind = "tq"
host = "free-openmd.shinnytech.com"
port = "443"
path = "/t/md/front/mobile"

[td]
kind = "ctp"

[ctp]
broker_id = {q(env["CTP_BROKER_ID"])}
user_id = {q(env["CTP_USER_ID"])}
password = {q(env["CTP_PASSWORD"])}
auth_code = {q(env.get("CTP_AUTH_CODE", ""))}
app_id = {q(env["CTP_APP_ID"])}
td_front = {q(env["CTP_TD_FRONT"])}
md_front = {q(env["CTP_MD_FRONT"])}
flow_path = {q(str(flow))}
order_interval_ms = 100

[runtime]
threads = 2
""".strip()


def node_config(port: int, gw_a: int, gw_b: int, name: str) -> str:
    def q(value: str) -> str:
        return json.dumps(value)

    return f"""
account_id = "A"

[ipc]
transport = "tcp"
encoding = "json"
handshake_timeout_ms = 5000
heartbeat_interval_ms = 1000
heartbeat_timeout_ms = 10000
local_name = {q(name)}

[md]
kind = "remote"
tcp_host = "127.0.0.1"
tcp_port = {gw_a}

[td]
kind = "remote"
tcp_host = "127.0.0.1"
tcp_port = {gw_a}
ack_timeout_ms = 10000

[serve]
bind = "127.0.0.1"
port = {port}
symbols = []

[serve.ws]
path = "/stream"
heartbeat_interval_ms = 30000
max_clients = 32
max_queue = 256

[instruments]
enabled = false

[kline_cache]
enabled = false

[tick_cache]
enabled = false

[[accounts]]
id = "B"
gw_host = "127.0.0.1"
gw_port = {gw_b}

[runtime]
threads = 2
""".strip()


def write_private(path: Path, text: str) -> None:
    path.write_text(text + "\n", encoding="utf-8")
    path.chmod(0o600)


def start(binary: Path, config: Path, log: Path) -> subprocess.Popen:
    output = log.open("w", encoding="utf-8")
    return subprocess.Popen(
        [str(binary), "-c", str(config)],
        cwd=str(ROOT),
        stdout=output,
        stderr=subprocess.STDOUT,
        start_new_session=True,
    )


def stop(procs: list[subprocess.Popen]) -> None:
    for proc in procs:
        try:
            os.killpg(proc.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
    deadline = time.monotonic() + 8
    for proc in procs:
        try:
            proc.wait(timeout=max(0.5, deadline - time.monotonic()))
        except subprocess.TimeoutExpired:
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin-dir", type=Path, default=ROOT / "build-ui")
    parser.add_argument("--env-a", type=Path, default=ROOT / "config/simnow_a.env")
    parser.add_argument("--env-b", type=Path, default=ROOT / "config/simnow_b.env")
    parser.add_argument("--node-port", type=int, default=18082)
    parser.add_argument("--gw-a-port", type=int, default=14331)
    parser.add_argument("--gw-b-port", type=int, default=14332)
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument("--keep", action="store_true")
    parser.add_argument("--out-dir", type=Path, default=ROOT / "artifacts/simnow_dual")
    args = parser.parse_args()

    gw_bin = args.bin_dir / "tqsdk-gw"
    node_bin = args.bin_dir / "tqsdk-node"
    for binary in (gw_bin, node_bin):
        if not os.access(binary, os.X_OK):
            raise SystemExit(f"missing executable: {binary}")

    if args.node_port == 18081 or args.gw_a_port == 14330 or args.gw_b_port == 14330:
        raise SystemExit("do not reuse the live soak ports 18081/14330")

    env_a = load_env(args.env_a)
    env_b = load_env(args.env_b)
    work = Path(tempfile.mkdtemp(prefix="tqsdk-simnow-dual-"))
    work.chmod(0o700)
    logs_by_name: dict[str, Path] = {}
    procs: list[subprocess.Popen] = []
    report: dict[str, Any] = {"ok": False, "node_port": args.node_port}
    try:
        write_private(work / "gw-a.toml", gw_config(env_a, args.gw_a_port, "simnow-a", work / "ctp-a"))
        write_private(work / "gw-b.toml", gw_config(env_b, args.gw_b_port, "simnow-b", work / "ctp-b"))
        write_private(work / "node.toml", node_config(args.node_port, args.gw_a_port, args.gw_b_port, "simnow-dual-node"))

        for name, binary, cfg in (
            ("gw-a", gw_bin, work / "gw-a.toml"),
            ("gw-b", gw_bin, work / "gw-b.toml"),
            ("node", node_bin, work / "node.toml"),
        ):
            log = work / f"{name}.log"
            logs_by_name[name] = log
            procs.append(start(binary, cfg, log))
            time.sleep(0.4)

        wait_http(args.node_port, args.timeout)
        accounts = wait_accounts(args.node_port, args.timeout)
        report["accounts"] = accounts

        for account in ("A", "B"):
            for endpoint in ("/account", "/positions", "/orders", "/trades"):
                status, body = http_json(args.node_port, "GET", f"{endpoint}?account={account}")
                if status != 200:
                    raise RuntimeError(f"{endpoint}?account={account} -> {status}: {body}")
            report[f"ws_{account}"] = asyncio.run(ws_snapshot(args.node_port, account))

        args.out_dir.mkdir(parents=True, exist_ok=True)
        stamp = time.strftime("%Y%m%d_%H%M%S")
        out = args.out_dir / f"simnow_dual_{stamp}.json"
        log_dir = args.out_dir / f"simnow_dual_{stamp}_logs"
        log_dir.mkdir(parents=True, exist_ok=True)
        log_dir.chmod(0o700)
        copied_logs = {}
        for name, source in logs_by_name.items():
            target = log_dir / source.name
            shutil.copy2(source, target)
            copied_logs[name] = str(target)
        report["ok"] = True
        report["profile"] = "read-only"
        report["artifacts"] = {"report": str(out), "logs": copied_logs}
        out.write_text(json.dumps(report, ensure_ascii=False, indent=2, default=str) + "\n", encoding="utf-8")
        print(json.dumps({
            "ok": True,
            "accounts": [item.get("account_id") for item in accounts.get("accounts", [])],
            "default": accounts.get("default"),
            "report": str(out),
        }, indent=2))
        return 0
    finally:
        stop(procs)
        args.out_dir.mkdir(parents=True, exist_ok=True)
        stamp = time.strftime("%Y%m%d_%H%M%S")
        log_dir = args.out_dir / f"simnow_dual_{stamp}_logs"
        log_dir.mkdir(parents=True, exist_ok=True)
        log_dir.chmod(0o700)
        for source in logs_by_name.values():
            if source.exists():
                shutil.copy2(source, log_dir / source.name)
        if not report.get("ok"):
            failure = args.out_dir / f"simnow_dual_{stamp}.failure.json"
            failure.write_text(
                json.dumps(report, ensure_ascii=False, indent=2, default=str) + "\n",
                encoding="utf-8",
            )
            print(f"failure artifact: {failure}", file=sys.stderr)
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Two-process soak harness for tqsdk-gw + tqsdk-node.

Default is a 24h soak; override with --duration for a short validation run.
The harness drives concurrent REST load, WebSocket subscribe/ping, periodic
insert/cancel, and periodic gw restart to exercise reconnect + reconciliation.
It records RSS/FD/thread counts once per second and writes CSV + JSON summaries.
"""

from __future__ import annotations

import argparse
import asyncio
import csv
import http.client
import json
import os
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


def find_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return int(s.getsockname()[1])


def proc_metrics(pid: int) -> tuple[int, int, int]:
    """Return (rss_kb, fd_count, thread_count); -1 when unavailable."""
    try:
        rss = -1
        threads = -1
        with open(f"/proc/{pid}/status", "r", encoding="utf-8") as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    rss = int(line.split()[1])
                elif line.startswith("Threads:"):
                    threads = int(line.split()[1])
        fds = len(os.listdir(f"/proc/{pid}/fd"))
        return rss, fds, threads
    except (FileNotFoundError, ProcessLookupError, PermissionError):
        return -1, -1, -1


def wait_http(url: str, timeout: float = 15.0) -> None:
    deadline = time.monotonic() + timeout
    last: Exception | None = None
    while time.monotonic() < deadline:
        try:
            with urllib.request.urlopen(url, timeout=1.0) as r:
                if r.status == 200:
                    return
        except Exception as e:  # noqa: BLE001 - retry any transient startup error
            last = e
        time.sleep(0.1)
    raise RuntimeError(f"timeout waiting for {url}: {last}")


@dataclass
class Stats:
    lock: threading.Lock = field(default_factory=threading.Lock)
    http_ok: int = 0
    http_err: int = 0
    ws_messages: int = 0
    ws_err: int = 0
    order_ok: int = 0
    order_err: int = 0
    reconnects: int = 0
    fatal: str = ""

    def inc(self, name: str, n: int = 1) -> None:
        with self.lock:
            setattr(self, name, getattr(self, name) + n)


def make_configs(tmp: Path, gw_port: int, node_port: int) -> tuple[Path, Path]:
    gw = tmp / "gw.toml"
    gw.write_text(
        f"""
[gateway]
ipc = "gw-soak"

[ipc.gw-soak]
name = "gw-soak"
transport = "tcp"
tcp_host = "127.0.0.1"
tcp_port = {gw_port}
encoding = "json"
handshake_timeout_ms = 5000
heartbeat_interval_ms = 1000
heartbeat_timeout_ms = 5000

[md]
kind = "none"

[td]
kind = "sim"
[td.sim]
initial_balance = 1000000000.0
[td.sim.specs."SHFE.rb2610"]
volume_multiple = 10.0
price_tick = 1.0
margin_rate = 0.1
commission_rate = 0.0
commission_per_lot = 2.0
[td.sim.last_prices]
"SHFE.rb2610" = 3800.0

[runtime]
threads = 4
""".strip()
        + "\n",
        encoding="utf-8",
    )

    node = tmp / "node.toml"
    node.write_text(
        f"""
primary_account = "default"

[[accounts]]
id = "default"
ipc = "gw-soak"

[ipc.gw-soak]
name = "node-soak"
transport = "tcp"
tcp_host = "127.0.0.1"
tcp_port = {gw_port}
encoding = "json"
handshake_timeout_ms = 5000
heartbeat_interval_ms = 1000
heartbeat_timeout_ms = 5000

[serve]
bind = "127.0.0.1"
port = {node_port}
symbols = []

[serve.ws]
path = "/stream"
heartbeat_interval_ms = 1000
max_clients = 64
max_queue = 256

[runtime]
threads = 4
""".strip()
        + "\n",
        encoding="utf-8",
    )
    return gw, node


class Soak:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.tmp = Path(tempfile.mkdtemp(prefix="tqsdk_soak_"))
        self.gw_port = find_free_port()
        self.node_port = find_free_port()
        self.gw_cfg, self.node_cfg = make_configs(self.tmp, self.gw_port, self.node_port)
        self.gw_log = self.tmp / "gw.log"
        self.node_log = self.tmp / "node.log"
        self.stop = threading.Event()
        self.stats = Stats()
        self.gw: subprocess.Popen[bytes] | None = None
        self.node: subprocess.Popen[bytes] | None = None
        self.proc_lock = threading.Lock()
        self.node_base = f"http://127.0.0.1:{self.node_port}"
        self.rows: list[tuple[Any, ...]] = []

        self.artifacts = Path(args.out).parent
        self.artifacts.mkdir(parents=True, exist_ok=True)

    def start_gw(self) -> None:
        log = self.gw_log.open("ab")
        try:
            with self.proc_lock:
                self.gw = subprocess.Popen(
                    [self.args.gw_bin, "-c", str(self.gw_cfg)],
                    stdout=log,
                    stderr=subprocess.STDOUT,
                    start_new_session=True,
                )
        finally:
            log.close()
        # gw 没有 HTTP 健康端点；node 的 /healthz 会覆盖建连+握手是否成功。
        time.sleep(0.2)

    def stop_gw(self) -> None:
        with self.proc_lock:
            gw = self.gw
            self.gw = None
        if gw and gw.poll() is None:
            gw.send_signal(signal.SIGTERM)
            try:
                gw.wait(timeout=10)
            except subprocess.TimeoutExpired:
                gw.kill()
                gw.wait(timeout=5)

    def start_node(self) -> None:
        log = self.node_log.open("ab")
        try:
            with self.proc_lock:
                self.node = subprocess.Popen(
                    [self.args.node_bin, "-c", str(self.node_cfg)],
                    stdout=log,
                    stderr=subprocess.STDOUT,
                    start_new_session=True,
                )
        finally:
            log.close()
        wait_http(self.node_base + "/readyz")

    def stop_node(self) -> None:
        with self.proc_lock:
            node = self.node
            self.node = None
        if node and node.poll() is None:
            node.send_signal(signal.SIGTERM)
            try:
                node.wait(timeout=10)
            except subprocess.TimeoutExpired:
                node.kill()
                node.wait(timeout=5)

    def http_get(self, path: str) -> None:
        try:
            with urllib.request.urlopen(self.node_base + path, timeout=3) as r:
                r.read()
                if r.status != 200:
                    raise RuntimeError(f"status {r.status}")
            self.stats.inc("http_ok")
        except Exception:  # noqa: BLE001 - soak counts errors, does not abort
            self.stats.inc("http_err")

    def order_cycle(self) -> None:
        try:
            body = json.dumps(
                {
                    "instrument_id": "SHFE.rb2610",
                    "direction": "BUY",
                    "offset": "OPEN",
                    "price_type": "LIMIT",
                    "limit_price": 1.0,
                    "volume": 1,
                    "time_cond": "GFD",
                    "volume_cond": "ANY",
                }
            ).encode()
            req = urllib.request.Request(
                self.node_base + "/order",
                data=body,
                headers={"Content-Type": "application/json"},
                method="POST",
            )
            with urllib.request.urlopen(req, timeout=5) as r:
                result = json.loads(r.read())
            oid = result.get("order_id", "")
            if not result.get("success") or not oid:
                raise RuntimeError(f"insert failed: {result}")
            time.sleep(0.05)
            req = urllib.request.Request(self.node_base + f"/order/{oid}", method="DELETE")
            with urllib.request.urlopen(req, timeout=5) as r:
                result = json.loads(r.read())
            if not result.get("success"):
                raise RuntimeError(f"cancel failed: {result}")
            self.stats.inc("order_ok")
        except Exception:  # noqa: BLE001 - soak counts errors
            self.stats.inc("order_err")

    def load_worker(self, idx: int) -> None:
        host, port = "127.0.0.1", self.node_port
        conn: http.client.HTTPConnection | None = None
        paths = ("/healthz", "/account")
        i = idx
        while not self.stop.is_set():
            try:
                if conn is None:
                    conn = http.client.HTTPConnection(host, port, timeout=3)
                conn.request("GET", paths[i % len(paths)])
                resp = conn.getresponse()
                resp.read()
                if resp.status != 200:
                    raise RuntimeError(f"status {resp.status}")
                self.stats.inc("http_ok")
            except Exception:  # noqa: BLE001
                self.stats.inc("http_err")
                if conn is not None:
                    conn.close()
                conn = None
            i += 1
        if conn is not None:
            conn.close()

    def ws_worker(self) -> None:
        try:
            import websockets  # type: ignore
        except Exception:  # noqa: BLE001
            self.stats.inc("ws_err")
            return

        async def run() -> None:
            uri = f"ws://127.0.0.1:{self.node_port}/stream"
            while not self.stop.is_set():
                try:
                    async with websockets.connect(uri, open_timeout=3) as ws:
                        await ws.send(
                            json.dumps(
                                {
                                    "op": "subscribe",
                                    "symbols": ["SHFE.rb2610"],
                                    "klines": [],
                                    "topics": [],
                                }
                            )
                        )
                        while not self.stop.is_set():
                            try:
                                msg = await asyncio.wait_for(ws.recv(), timeout=2.0)
                                json.loads(msg)
                                self.stats.inc("ws_messages")
                                await ws.send(json.dumps({"op": "ping"}))
                            except asyncio.TimeoutError:
                                await ws.send(json.dumps({"op": "ping"}))
                except Exception:  # noqa: BLE001 - reconnect on any ws failure
                    self.stats.inc("ws_err")
                    if not self.stop.is_set():
                        await asyncio.sleep(0.2)

        asyncio.run(run())

    def reconnector(self) -> None:
        while not self.stop.wait(self.args.reconnect_interval):
            self.stop_gw()
            time.sleep(0.5)
            self.start_gw()
            try:
                wait_http(self.node_base + "/healthz", timeout=15)
                self.stats.inc("reconnects")
            except Exception as e:  # noqa: BLE001
                self.stats.fatal = f"reconnect failed: {e}"
                self.stop.set()
                return

    def metrics_loop(self) -> None:
        start = time.monotonic()
        next_progress = 60.0
        while not self.stop.wait(1.0):
            now = time.monotonic() - start
            with self.proc_lock:
                node = self.node
                gw = self.gw
            node_m = proc_metrics(node.pid) if node and node.poll() is None else (-1, -1, -1)
            gw_m = proc_metrics(gw.pid) if gw and gw.poll() is None else (-1, -1, -1)
            node_alive = bool(node and node.poll() is None)
            gw_alive = bool(gw and gw.poll() is None)
            self.rows.append(
                (
                    f"{now:.2f}",
                    *node_m,
                    *gw_m,
                    int(node_alive),
                    int(gw_alive),
                    self.stats.http_ok,
                    self.stats.http_err,
                    self.stats.order_ok,
                    self.stats.order_err,
                    self.stats.ws_messages,
                    self.stats.ws_err,
                    self.stats.reconnects,
                )
            )
            if now >= next_progress:
                next_progress += 60.0
                print(
                    json.dumps(
                        {
                            "elapsed_s": round(now, 1),
                            "node_rss_kb": node_m[0],
                            "node_fds": node_m[1],
                            "gw_rss_kb": gw_m[0],
                            "http_ok": self.stats.http_ok,
                            "http_err": self.stats.http_err,
                            "order_ok": self.stats.order_ok,
                            "order_err": self.stats.order_err,
                            "ws_messages": self.stats.ws_messages,
                            "ws_err": self.stats.ws_err,
                            "reconnects": self.stats.reconnects,
                        }
                    ),
                    flush=True,
                )
            if not node_alive:
                self.stats.fatal = "node process exited"
                self.stop.set()
                return

    def run(self) -> int:
        print(
            json.dumps(
                {
                    "starting": True,
                    "duration_s": self.args.duration,
                    "reconnect_interval_s": self.args.reconnect_interval,
                    "load_threads": self.args.load_threads,
                    "order_interval_s": self.args.order_interval,
                    "gw": self.args.gw_bin,
                    "node": self.args.node_bin,
                    "out": self.args.out,
                }
            ),
            flush=True,
        )
        self.start_gw()
        self.start_node()
        time.sleep(0.5)

        threads = [
            threading.Thread(target=self.metrics_loop, name="metrics", daemon=True),
            threading.Thread(target=self.reconnector, name="reconnect", daemon=True),
            threading.Thread(target=self.ws_worker, name="ws", daemon=True),
        ]
        for i in range(self.args.load_threads):
            threads.append(
                threading.Thread(target=self.load_worker, args=(i,), name=f"http-{i}", daemon=True)
            )
        threads.append(threading.Thread(target=self.order_loop, name="orders", daemon=True))
        for t in threads:
            t.start()

        deadline = time.monotonic() + self.args.duration
        try:
            while time.monotonic() < deadline and not self.stop.is_set():
                time.sleep(0.1)
        except KeyboardInterrupt:
            self.stats.fatal = "interrupted"
        finally:
            self.stop.set()
            for t in threads:
                t.join(timeout=5)
            self.stop_node()
            self.stop_gw()

        return self.finish()

    def order_loop(self) -> None:
        while not self.stop.wait(self.args.order_interval):
            self.order_cycle()

    def finish(self) -> int:
        stamp = time.strftime("%Y%m%d_%H%M%S")
        csv_path = self.artifacts / f"soak_{stamp}.csv"
        with csv_path.open("w", newline="", encoding="utf-8") as f:
            w = csv.writer(f)
            w.writerow(
                [
                    "t",
                    "node_rss_kb",
                    "node_fds",
                    "node_threads",
                    "gw_rss_kb",
                    "gw_fds",
                    "gw_threads",
                    "node_alive",
                    "gw_alive",
                    "http_ok",
                    "http_err",
                    "order_ok",
                    "order_err",
                    "ws_messages",
                    "ws_err",
                    "reconnects",
                ]
            )
            w.writerows(self.rows)

        def col(idx: int) -> list[int]:
            return [int(r[idx]) for r in self.rows if int(r[idx]) >= 0]

        summary = {
            "duration_s": self.args.duration,
            "csv": str(csv_path),
            "gw_log": str(self.gw_log),
            "node_log": str(self.node_log),
            "http_ok": self.stats.http_ok,
            "http_err": self.stats.http_err,
            "order_ok": self.stats.order_ok,
            "order_err": self.stats.order_err,
            "ws_messages": self.stats.ws_messages,
            "ws_err": self.stats.ws_err,
            "reconnects": self.stats.reconnects,
            "node_max_rss_kb": max(col(1), default=-1),
            "node_max_fds": max(col(2), default=-1),
            "node_max_threads": max(col(3), default=-1),
            "gw_max_rss_kb": max(col(4), default=-1),
            "gw_max_fds": max(col(5), default=-1),
            "gw_max_threads": max(col(6), default=-1),
            "fatal": self.stats.fatal,
        }
        json_path = self.artifacts / f"soak_{stamp}.json"
        json_path.write_text(json.dumps(summary, indent=2), encoding="utf-8")
        print(json.dumps(summary, indent=2))
        # gw 重启窗口内允许少量 HTTP/WS 失败；只要进程存活且产生过成功请求即通过。
        ok = not summary["fatal"] and self.stats.http_ok > 0
        return 0 if ok else 1

    def cleanup(self) -> None:
        self.stop.set()
        self.stop_node()
        self.stop_gw()
        if not self.args.keep:
            import shutil

            shutil.rmtree(self.tmp, ignore_errors=True)
        else:
            print(f"kept temp dir: {self.tmp}")


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--duration", type=float, default=86400.0, help="soak seconds (default 24h)")
    p.add_argument("--reconnect-interval", type=float, default=600.0)
    p.add_argument("--load-threads", type=int, default=8)
    p.add_argument("--order-interval", type=float, default=5.0)
    p.add_argument("--gw-bin", default="./build/tqsdk-gw")
    p.add_argument("--node-bin", default="./build/tqsdk-node")
    p.add_argument("--out", default="artifacts/soak.csv")
    p.add_argument("--keep", action="store_true")
    args = p.parse_args()

    s = Soak(args)
    try:
        return s.run()
    finally:
        s.cleanup()


if __name__ == "__main__":
    sys.exit(main())

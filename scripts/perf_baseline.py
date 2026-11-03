#!/usr/bin/env python3
"""Repeatable HTTP/WebSocket baseline for a running tqsdk-node.

HTTP: N keep-alive connections issue sequential GETs for a fixed duration.
WebSocket: M clients measure app-level {"op":"ping"} -> {"type":"pong"} RTT.
Results are client-side and intended for relative regression tracking, not as
an absolute network-capacity claim.
"""

from __future__ import annotations

import argparse
import asyncio
import http.client
import json
import math
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path


def percentile(values: list[float], p: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    idx = min(len(ordered) - 1, max(0, int(round((p / 100.0) * (len(ordered) - 1)))))
    return ordered[idx]


@dataclass
class HttpResult:
    latencies_us: list[float] = field(default_factory=list)
    errors: int = 0
    status_errors: int = 0


def http_worker(
    host: str,
    port: int,
    path: str,
    duration: float,
    barrier: threading.Barrier,
    result: HttpResult,
) -> None:
    conn: http.client.HTTPConnection | None = None
    barrier.wait()
    deadline = time.perf_counter() + duration
    while time.perf_counter() < deadline:
        try:
            if conn is None:
                conn = http.client.HTTPConnection(host, port, timeout=3)
            t0 = time.perf_counter()
            conn.request("GET", path)
            resp = conn.getresponse()
            resp.read()
            dt = time.perf_counter() - t0
            if resp.status != 200:
                result.status_errors += 1
            else:
                result.latencies_us.append(dt * 1_000_000.0)
        except Exception:  # noqa: BLE001
            result.errors += 1
            if conn is not None:
                conn.close()
            conn = None
    if conn is not None:
        conn.close()


@dataclass
class WsResult:
    latencies_us: list[float] = field(default_factory=list)
    errors: int = 0


def ws_worker(host: str, port: int, path: str, duration: float, result: WsResult) -> None:
    try:
        import websockets  # type: ignore
    except Exception:  # noqa: BLE001
        result.errors += 1
        return

    async def run() -> None:
        uri = f"ws://{host}:{port}{path}"
        deadline = time.perf_counter() + duration
        while time.perf_counter() < deadline:
            try:
                async with websockets.connect(uri, open_timeout=3) as ws:
                    await ws.recv()  # welcome
                    while time.perf_counter() < deadline:
                        t0 = time.perf_counter()
                        await ws.send('{"op":"ping"}')
                        msg = json.loads(await asyncio.wait_for(ws.recv(), timeout=3))
                        dt = time.perf_counter() - t0
                        if msg.get("type") == "pong":
                            result.latencies_us.append(dt * 1_000_000.0)
                        else:
                            result.errors += 1
            except Exception:  # noqa: BLE001
                result.errors += 1
                await asyncio.sleep(0.1)

    asyncio.run(run())


def stats(latencies: list[float]) -> dict[str, float]:
    return {
        "count": float(len(latencies)),
        "p50_us": percentile(latencies, 50),
        "p90_us": percentile(latencies, 90),
        "p99_us": percentile(latencies, 99),
        "max_us": max(latencies, default=0.0),
        "mean_us": (sum(latencies) / len(latencies)) if latencies else 0.0,
    }


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=8080)
    p.add_argument("--path", default="/healthz")
    p.add_argument("--clients", type=int, default=16)
    p.add_argument("--duration", type=float, default=5.0)
    p.add_argument("--ws-path", default="/stream")
    p.add_argument("--ws-clients", type=int, default=4)
    p.add_argument("--out", default="artifacts/perf")
    args = p.parse_args()

    barrier = threading.Barrier(args.clients + 1)
    http_results = [HttpResult() for _ in range(args.clients)]
    threads = [
        threading.Thread(
            target=http_worker,
            args=(args.host, args.port, args.path, args.duration, barrier, r),
            daemon=True,
        )
        for r in http_results
    ]
    for t in threads:
        t.start()
    barrier.wait()
    start = time.perf_counter()
    for t in threads:
        t.join()
    elapsed = time.perf_counter() - start

    lat = [x for r in http_results for x in r.latencies_us]
    hs = stats(lat)
    hs["qps"] = len(lat) / elapsed if elapsed > 0 else 0.0
    hs["errors"] = sum(r.errors for r in http_results)
    hs["status_errors"] = sum(r.status_errors for r in http_results)

    ws_results = [WsResult() for _ in range(args.ws_clients)]
    ws_threads = [
        threading.Thread(
            target=ws_worker,
            args=(args.host, args.port, args.ws_path, args.duration, r),
            daemon=True,
        )
        for r in ws_results
    ]
    for t in ws_threads:
        t.start()
    for t in ws_threads:
        t.join()
    ws_lat = [x for r in ws_results for x in r.latencies_us]
    ws = stats(ws_lat)
    ws["errors"] = sum(r.errors for r in ws_results)
    ws["qps"] = len(ws_lat) / args.duration if args.duration > 0 else 0.0

    summary = {
        "host": args.host,
        "port": args.port,
        "duration_s": args.duration,
        "http": hs,
        "websocket": ws,
    }

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime("%Y%m%d_%H%M%S")
    json_path = out.parent / f"{out.name}_{stamp}.json"
    md_path = out.parent / f"{out.name}_{stamp}.md"
    json_path.write_text(json.dumps(summary, indent=2), encoding="utf-8")

    def ms(us: float) -> str:
        return f"{us / 1000.0:.3f}"

    md = [
        "# tqsdk-node perf baseline",
        "",
        f"- target: `{args.host}:{args.port}{args.path}` / `{args.ws_path}`",
        f"- duration: {args.duration}s",
        "",
        "| workload | qps | p50 ms | p90 ms | p99 ms | max ms | errors |",
        "|---|---:|---:|---:|---:|---:|---:|",
        f"| HTTP x{args.clients} | {hs['qps']:.1f} | {ms(hs['p50_us'])} | {ms(hs['p90_us'])} | "
        f"{ms(hs['p99_us'])} | {ms(hs['max_us'])} | {int(hs['errors'] + hs['status_errors'])} |",
        f"| WS ping x{args.ws_clients} | {ws['qps']:.1f} | {ms(ws['p50_us'])} | {ms(ws['p90_us'])} | "
        f"{ms(ws['p99_us'])} | {ms(ws['max_us'])} | {int(ws['errors'])} |",
        "",
    ]
    md_path.write_text("\n".join(md), encoding="utf-8")
    print("\n".join(md))
    print(f"json: {json_path}")
    return 0 if hs["errors"] == 0 and ws["errors"] == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())

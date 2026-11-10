#!/usr/bin/env python3
"""Small HTTP/WebSocket latency and throughput probe for a running node.

Example:
  python3 scripts/bench_http_ws.py --base http://127.0.0.1:18081 \
      --symbol SHFE.rb2701 --requests 200 --ws-seconds 5
"""

from __future__ import annotations

import argparse
import asyncio
import http.client
import json
import statistics
import time
import urllib.parse


def percentile(values: list[float], p: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, round((len(ordered) - 1) * p)))
    return ordered[index]


def http_probe(base: str, symbol: str, requests: int) -> dict:
    parsed = urllib.parse.urlparse(base)
    host = parsed.hostname or "127.0.0.1"
    port = parsed.port or 80
    paths = [
        "/healthz",
        f"/quote/{urllib.parse.quote(symbol)}",
        f"/kline/{urllib.parse.quote(symbol)}/60000000000",
        "/trading-snapshot",
    ]
    results: dict[str, dict] = {}
    for path in paths:
        samples: list[float] = []
        errors = 0
        conn = http.client.HTTPConnection(host, port, timeout=5)
        for _ in range(requests):
            started = time.perf_counter()
            try:
                conn.request("GET", path)
                response = conn.getresponse()
                response.read()
                if response.status >= 400:
                    errors += 1
            except Exception:
                errors += 1
                conn.close()
                conn = http.client.HTTPConnection(host, port, timeout=5)
            finally:
                samples.append((time.perf_counter() - started) * 1000)
        conn.close()
        results[path] = {
            "count": len(samples),
            "errors": errors,
            "p50_ms": round(statistics.median(samples), 3) if samples else 0,
            "p95_ms": round(percentile(samples, 0.95), 3),
            "max_ms": round(max(samples, default=0), 3),
        }
    return results


async def ws_probe(base: str, symbol: str, seconds: float, account: str | None) -> dict:
    try:
        import websockets
    except ImportError:
        return {"skipped": "python websockets package is not installed"}

    ws_url = base.replace("http://", "ws://").replace("https://", "wss://").rstrip("/") + "/stream"
    if account:
        ws_url += "?account=" + urllib.parse.quote(account)
    first_snapshot_ms = None
    messages = 0
    started = time.perf_counter()
    try:
        async with websockets.connect(ws_url, open_timeout=5) as socket:
            await socket.send(
                json.dumps(
                    {
                        "op": "subscribe",
                        "symbols": [symbol],
                        "klines": [{"symbol": symbol, "duration_ns": 60_000_000_000, "view_width": 200}],
                        "topics": ["account", "orders", "trades", "positions"],
                    }
                )
            )
            deadline = started + seconds
            while time.perf_counter() < deadline:
                timeout = max(0.01, deadline - time.perf_counter())
                try:
                    raw = await asyncio.wait_for(socket.recv(), timeout=min(timeout, 1.0))
                except asyncio.TimeoutError:
                    continue
                messages += 1
                message = json.loads(raw)
                if message.get("type") == "snapshot" and first_snapshot_ms is None:
                    first_snapshot_ms = round((time.perf_counter() - started) * 1000, 3)
    except Exception as exc:  # noqa: BLE001 - this is a diagnostic probe
        return {"error": str(exc), "messages": messages}
    elapsed = max(0.001, time.perf_counter() - started)
    return {
        "messages": messages,
        "messages_per_second": round(messages / elapsed, 2),
        "first_snapshot_ms": first_snapshot_ms,
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base", default="http://127.0.0.1:18081")
    parser.add_argument("--symbol", default="SHFE.rb2701")
    parser.add_argument("--requests", type=int, default=100)
    parser.add_argument("--ws-seconds", type=float, default=5.0)
    parser.add_argument("--account", default="")
    args = parser.parse_args()

    report = {
        "base": args.base,
        "symbol": args.symbol,
        "http": http_probe(args.base, args.symbol, args.requests),
        "websocket": asyncio.run(
            ws_probe(args.base, args.symbol, args.ws_seconds, args.account or None)
        ),
    }
    print(json.dumps(report, indent=2, ensure_ascii=False))


if __name__ == "__main__":
    main()

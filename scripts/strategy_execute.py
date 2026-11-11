#!/usr/bin/env python3
"""Evaluate the EMA60/Donchian22 strategy and optionally submit one target.

The scanner uses KQ.i@ continuous indices. Execution must use a real contract
(e.g. SHFE.rb2701), so this bridge evaluates the real contract and submits a
single TargetPosTask target only when --execute is passed explicitly.
"""

from __future__ import annotations

import argparse
import json
import os
import urllib.parse
import urllib.request

from strategy_scan import Params, donchian, ema, fetch_bars


def request_json(url: str, method: str = "GET", body: dict | None = None) -> dict:
    headers = {"Accept": "application/json"}
    token = os.environ.get("TQSDK_SERVE_TOKEN", "")
    if token:
        headers["Authorization"] = f"Bearer {token}"
    payload = None
    if body is not None:
        headers["Content-Type"] = "application/json"
        payload = json.dumps(body).encode()
    request = urllib.request.Request(url, data=payload, headers=headers, method=method)
    with urllib.request.urlopen(request, timeout=20) as response:
        return json.load(response)


def latest_target(symbol: str, bars: list[dict], params: Params) -> tuple[int, str]:
    closes = [float(bar["close"]) for bar in bars]
    if len(bars) < max(params.ema_period, params.donchian_period, params.atr_period) + 2:
        return 0, "insufficient bars"
    ema_values = ema(closes, params.ema_period)
    upper, lower = donchian(bars, params.donchian_period)
    last = len(bars) - 1
    close = closes[last]
    if close > ema_values[last] and close > upper[last]:
        return 1, "long"
    if close < ema_values[last] and close < lower[last]:
        return -1, "short"
    return 0, "flat"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--node-url", default="http://127.0.0.1:18081")
    parser.add_argument("--account", default="default")
    parser.add_argument("--symbol", default="SHFE.rb2701")
    parser.add_argument("--execute", action="store_true")
    parser.add_argument("--volume", type=int, default=1)
    args = parser.parse_args()

    base = args.node_url.rstrip("/")
    bars, _ = fetch_bars(base, args.symbol, 200)
    target, signal = latest_target(args.symbol, bars, Params())
    print(json.dumps({
        "symbol": args.symbol,
        "bars": len(bars),
        "signal": signal,
        "target_volume": target * max(1, args.volume),
        "execute": args.execute,
    }, ensure_ascii=False))

    if not args.execute:
        return 0
    if target == 0:
        print("no entry signal; no target submitted")
        return 0
    account = urllib.parse.quote(args.account)
    response = request_json(
        f"{base}/task/target?account={account}",
        method="POST",
        body={
            "symbol": args.symbol,
            "target_volume": target * max(1, args.volume),
            "mode": "ACTIVE",
        },
    )
    print(json.dumps(response, ensure_ascii=False))
    return 0 if response.get("success") else 1


if __name__ == "__main__":
    raise SystemExit(main())

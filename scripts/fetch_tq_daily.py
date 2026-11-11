#!/usr/bin/env python3
"""Download ten-year daily KQ.i@ index bars from the Tq backtest MD server.

The public real-time endpoint only returns a small window and is occasionally
unavailable. TqSdk's backtest MD endpoint is designed for historical research
and returns the full daily history (about 2,600 bars per product).

Credentials are read from an env file or environment variables and are never
printed. Output is one JSON file per product under the requested output dir:

    KQ.i@DCE.jm.json

Usage:
    python3 scripts/fetch_tq_daily.py \
      --env-file config/live.local.env \
      --output-dir artifacts/tq_daily

Then run:
    python3 scripts/backtest_daily.py --data-dir artifacts/tq_daily
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import time
import urllib.request
import uuid
from pathlib import Path
from typing import Any

try:
    import requests
    import websockets
except ImportError as exc:  # pragma: no cover
    raise SystemExit("需要安装 requests 和 websockets: pip install requests websockets") from exc

AUTH_URL = "https://auth.shinnytech.com/auth/realms/shinnytech/protocol/openid-connect/token"
CLIENT_ID = "shinny_tq"
CLIENT_SECRET = "be30b9f4-6862-488a-99ad-21bde0400081"
BACKTEST_MD_URL = "wss://backtest.shinnytech.com/t/nfmd/front/mobile"
CONT_TABLE_URL = "https://files.shinnytech.com/continuous_table.json"
DAY_NS = 86_400_000_000_000


def read_env(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    if not path.exists():
        return values
    for line in path.read_text().splitlines():
        if "=" not in line or line.lstrip().startswith("#"):
            continue
        key, value = line.split("=", 1)
        values[key.strip()] = value.strip().strip('"').strip("'")
    return values


def credentials(env_file: Path) -> tuple[str, str]:
    file_values = read_env(env_file)
    user = (
        os.environ.get("TQSDK_USERNAME")
        or os.environ.get("TQSDK_USER")
        or os.environ.get("TQ_AUTH_USER")
        or file_values.get("TQSDK_USERNAME")
        or file_values.get("TQ_AUTH_USER")
        or ""
    )
    password = (
        os.environ.get("TQSDK_PASSWORD")
        or os.environ.get("TQSDK_PASS")
        or os.environ.get("TQ_AUTH_PASSWORD")
        or file_values.get("TQSDK_PASSWORD")
        or file_values.get("TQ_AUTH_PASSWORD")
        or ""
    )
    if not user or not password:
        raise SystemExit(f"未找到 Tq 账号：{env_file} 或 TQSDK_USERNAME/TQSDK_PASSWORD")
    return user, password


def load_products(path: Path | None) -> list[str]:
    if path is not None:
        return [line.strip() for line in path.read_text().splitlines() if line.strip()]
    local = Path("continuous_table.json")
    text: str
    if local.exists():
        text = local.read_text()
    else:
        with urllib.request.urlopen(CONT_TABLE_URL, timeout=30) as response:
            text = response.read().decode()
    table = json.loads(text)
    return sorted(key for key, rows in table.items() if rows and "." in key)


def auth_token(user: str, password: str) -> str:
    response = requests.post(
        AUTH_URL,
        data={
            "grant_type": "password",
            "username": user,
            "password": password,
            "client_id": CLIENT_ID,
            "client_secret": CLIENT_SECRET,
        },
        headers={"User-Agent": "tqsdk-python 3.10.4", "Accept": "application/json"},
        timeout=30,
    )
    if response.status_code != 200:
        raise SystemExit(f"TqAuth 登录失败: HTTP {response.status_code}")
    return response.json()["access_token"]


def parse_klines(patch: dict[str, Any], symbol: str) -> tuple[dict[str, Any], dict[str, Any]] | None:
    by_symbol = patch.get("klines", {})
    if not isinstance(by_symbol, dict):
        return None
    by_duration = by_symbol.get(symbol)
    if not isinstance(by_duration, dict):
        return None
    obj = by_duration.get(str(DAY_NS))
    if not isinstance(obj, dict) or "data" not in obj:
        return None
    return obj, by_duration


async def fetch_product(
    ws,
    symbol: str,
    timeout: float,
) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    chart_id = "bt_" + uuid.uuid4().hex
    await ws.send(json.dumps({"aid": "subscribe_quote", "ins_list": symbol}))
    await ws.send(
        json.dumps(
            {
                "aid": "set_chart",
                "chart_id": chart_id,
                "ins_list": symbol,
                "duration": DAY_NS,
                "view_width": 10000,
            }
        )
    )
    await ws.send(json.dumps({"aid": "peek_message"}))
    deadline = time.monotonic() + timeout
    chart: dict[str, Any] = {}
    data: dict[str, Any] | None = None

    while time.monotonic() < deadline:
        try:
            raw = await asyncio.wait_for(ws.recv(), timeout=max(0.1, deadline - time.monotonic()))
        except asyncio.TimeoutError:
            break
        frame = json.loads(raw)
        if frame.get("aid") != "rtn_data":
            continue
        for patch in frame.get("data", []):
            charts = patch.get("charts", {})
            if isinstance(charts, dict) and chart_id in charts:
                chart.update(charts[chart_id])
            parsed = parse_klines(patch, symbol)
            if parsed is not None:
                data, _ = parsed
        await ws.send(json.dumps({"aid": "peek_message"}))
        if data is not None and data.get("data"):
            right_id = chart.get("right_id")
            if right_id is None or len(data["data"]) >= int(right_id) + 1:
                break

    if not data or not data.get("data"):
        return [], chart
    bars = sorted(data["data"].values(), key=lambda item: int(item["datetime"]))
    return bars, chart


async def run(args: argparse.Namespace) -> int:
    user, password = credentials(Path(args.env_file))
    token = auth_token(user, password)
    products = load_products(Path(args.products_file) if args.products_file else None)
    if args.limit:
        products = products[: args.limit]
    output = Path(args.output_dir)
    output.mkdir(parents=True, exist_ok=True)
    failures: dict[str, str] = {}
    headers = {"Authorization": "Bearer " + token, "User-Agent": "tqsdk-python 3.10.4"}

    async with websockets.connect(
        args.md_url, additional_headers=headers, max_size=None
    ) as ws:
        for index, product in enumerate(products, 1):
            symbol = f"KQ.i@{product}"
            target = output / f"{symbol}.json"
            if args.resume and target.exists():
                print(f"[{index}/{len(products)}] skip {symbol}", flush=True)
                continue
            started = time.monotonic()
            try:
                bars, chart = await asyncio.wait_for(
                    fetch_product(ws, symbol, args.timeout), timeout=args.timeout + 5
                )
                if not bars:
                    failures[symbol] = "empty bars"
                    print(f"[{index}/{len(products)}] EMPTY {symbol}", flush=True)
                    continue
                payload = {
                    "symbol": symbol,
                    "product": product,
                    "duration_ns": DAY_NS,
                    "source": "tqsdk-backtest-md",
                    "fetched_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
                    "bars": bars,
                    "right_id": chart.get("right_id"),
                    "left_id": chart.get("left_id"),
                }
                target.write_text(json.dumps(payload, ensure_ascii=False))
                print(
                    f"[{index}/{len(products)}] {symbol} bars={len(bars)} "
                    f"first={bars[0]['datetime']} last={bars[-1]['datetime']} "
                    f"{time.monotonic() - started:.2f}s",
                    flush=True,
                )
            except Exception as exc:  # noqa: BLE001 - one bad product must not stop the universe
                failures[symbol] = str(exc)
                print(f"[{index}/{len(products)}] FAIL {symbol}: {exc}", flush=True)

    (output / "_failures.json").write_text(json.dumps(failures, ensure_ascii=False, indent=2))
    print(f"done: {len(products) - len(failures)}/{len(products)} products, failures={len(failures)}")
    return 0 if len(failures) < len(products) else 1


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--env-file", default="config/live.local.env")
    parser.add_argument("--output-dir", default="artifacts/tq_daily")
    parser.add_argument("--products-file", default=None)
    parser.add_argument("--md-url", default=BACKTEST_MD_URL)
    parser.add_argument("--timeout", type=float, default=20.0)
    parser.add_argument("--limit", type=int, default=0)
    parser.add_argument("--resume", action="store_true")
    args = parser.parse_args()
    return asyncio.run(run(args))


if __name__ == "__main__":
    raise SystemExit(main())

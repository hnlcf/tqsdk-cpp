#!/usr/bin/env python3
"""EMA60 + Donchian(22) + ATR stop strategy scanner.

Signals use 4h bars:
  long : close > EMA60 and close > prior Donchian high(22)
  short: close < EMA60 and close < prior Donchian low(22)
  exit : close crosses EMA60/channel, or 2*ATR intrabar stop
  size : 30% of current equity

The default data source is a local tqsdk-node REST API. Continuous *index*
symbols (KQ.i@PRODUCT) are used for scanning so historical roll gaps do not
create fake signals; this script never places an order unless --execute is
explicitly supplied.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import math
import time
import urllib.parse
import urllib.request
from dataclasses import dataclass, asdict
from pathlib import Path

DEFAULT_PRODUCTS = [
    "SHFE.rb", "SHFE.ru", "SHFE.au", "SHFE.ag", "SHFE.cu", "SHFE.al",
    "SHFE.zn", "SHFE.ni", "SHFE.sn", "SHFE.pb", "SHFE.ss", "SHFE.hc",
    "SHFE.sp", "SHFE.bu", "SHFE.fu", "SHFE.ao",
    "DCE.m", "DCE.y", "DCE.p", "DCE.c", "DCE.i", "DCE.j", "DCE.jm",
    "DCE.j", "DCE.v", "DCE.pp", "DCE.l", "DCE.eg", "DCE.eb", "DCE.pg",
    "DCE.rr", "DCE.lh", "DCE.jd", "DCE.a", "DCE.b", "DCE.cs",
    "CZCE.SA", "CZCE.MA", "CZCE.TA", "CZCE.SR", "CZCE.CF", "CZCE.CY",
    "CZCE.FG", "CZCE.AP", "CZCE.CJ", "CZCE.PK", "CZCE.UR", "CZCE.OI",
    "CZCE.RM",
    "INE.sc", "INE.lu", "INE.nr",
    "GFEX.si", "GFEX.lc",
]

FOUR_HOURS_NS = 14_400_000_000_000


@dataclass
class Params:
    ema_period: int = 60
    donchian_period: int = 22
    atr_period: int = 14
    stop_atr: float = 2.0
    allocation: float = 0.30
    initial_equity: float = 1_000_000.0
    commission_rate: float = 0.0001


@dataclass
class Trade:
    side: str
    entry_time: int
    entry_price: float
    exit_time: int
    exit_price: float
    reason: str
    pnl: float
    return_pct: float


@dataclass
class Result:
    symbol: str
    bars: int
    trades: int
    wins: int
    win_rate: float
    final_equity: float
    total_return_pct: float
    max_drawdown_pct: float
    profit_factor: float
    last_signal: str
    stop_reason: str = ""
    bars_from: int = 0
    bars_to: int = 0


def request_json(url: str, timeout: float = 20.0) -> dict:
    with urllib.request.urlopen(url, timeout=timeout) as response:
        return json.load(response)


def fetch_bars(base: str, symbol: str, width: int) -> tuple[list[dict], str]:
    query = urllib.parse.urlencode({"width": width})
    url = f"{base}/kline/{urllib.parse.quote(symbol, safe='')}/{FOUR_HOURS_NS}?{query}"
    data = request_json(url, timeout=45.0)
    return data.get("bars", []), data.get("symbol", symbol)


def ema(values: list[float], period: int) -> list[float]:
    out = []
    alpha = 2.0 / (period + 1.0)
    current = values[0] if values else math.nan
    for index, value in enumerate(values):
        current = value if index == 0 else current + alpha * (value - current)
        out.append(current)
    return out


def atr(bars: list[dict], period: int) -> list[float]:
    out = [math.nan] * len(bars)
    trs = []
    for index, bar in enumerate(bars):
        if index == 0:
            tr = bar["high"] - bar["low"]
        else:
            prev = bars[index - 1]["close"]
            tr = max(bar["high"] - bar["low"], abs(bar["high"] - prev), abs(bar["low"] - prev))
        trs.append(tr)
        if index >= period - 1:
            out[index] = sum(trs[index + 1 - period : index + 1]) / period
    return out


def donchian(bars: list[dict], period: int) -> tuple[list[float], list[float]]:
    upper = [math.nan] * len(bars)
    lower = [math.nan] * len(bars)
    for index in range(period, len(bars)):
        window = bars[index - period : index]  # exclude current bar
        upper[index] = max(item["high"] for item in window)
        lower[index] = min(item["low"] for item in window)
    return upper, lower


def backtest(symbol: str, bars: list[dict], params: Params) -> Result:
    bars = [b for b in bars if all(math.isfinite(float(b.get(k, math.nan))) for k in ("open", "high", "low", "close"))]
    if len(bars) <= max(params.ema_period, params.donchian_period, params.atr_period) + 2:
        return Result(symbol, len(bars), 0, 0, 0.0, params.initial_equity, 0.0, 0.0, 0.0, "none")

    closes = [float(b["close"]) for b in bars]
    ema_values = ema(closes, params.ema_period)
    upper, lower = donchian(bars, params.donchian_period)
    atr_values = atr(bars, params.atr_period)

    equity = params.initial_equity
    peak = equity
    max_dd = 0.0
    position = 0
    units = 0.0
    entry_price = 0.0
    entry_time = 0
    stop_price = 0.0
    trades: list[Trade] = []
    last_signal = "none"

    def close_position(index: int, price: float, reason: str) -> None:
        nonlocal equity, position, units, entry_price
        if position == 0:
            return
        gross = (price - entry_price) * units * position
        commission = abs(price * units) * params.commission_rate
        pnl = gross - commission
        equity += pnl
        trades.append(
            Trade(
                "long" if position > 0 else "short",
                entry_time,
                entry_price,
                int(bars[index]["datetime"]),
                price,
                reason,
                pnl,
                pnl / max(1.0, equity - pnl) * 100.0,
            )
        )
        position = 0
        units = 0.0
        entry_price = 0.0

    for index in range(max(params.ema_period, params.donchian_period, params.atr_period) + 1, len(bars)):
        bar = bars[index]
        open_price = float(bar["open"])
        high = float(bar["high"])
        low = float(bar["low"])
        close = float(bar["close"])

        if position == 0:
            long_signal = close > ema_values[index] and close > upper[index]
            short_signal = close < ema_values[index] and close < lower[index]
            if long_signal or short_signal:
                next_index = index + 1
                if next_index >= len(bars) or not math.isfinite(atr_values[index]):
                    last_signal = "long" if long_signal else "short"
                    continue
                entry_price = float(bars[next_index]["open"])
                if entry_price <= 0:
                    continue
                position = 1 if long_signal else -1
                units = (equity * params.allocation) / entry_price
                entry_time = int(bars[next_index]["datetime"])
                stop_price = entry_price - position * params.stop_atr * atr_values[index]
                last_signal = "long" if long_signal else "short"
                # Consume the next bar's open without re-evaluating its close in
                # this iteration; the next loop iteration handles stop/exit.
                continue

        if position != 0:
            stopped = (position > 0 and low <= stop_price) or (position < 0 and high >= stop_price)
            if stopped:
                exit_price = min(open_price, stop_price) if position > 0 else max(open_price, stop_price)
                close_position(index, exit_price, "atr_stop")
                peak = max(peak, equity)
                max_dd = max(max_dd, (peak - equity) / peak * 100.0)
                continue
            exit_signal = (
                position > 0
                and (close < ema_values[index] or close < lower[index])
            ) or (
                position < 0
                and (close > ema_values[index] or close > upper[index])
            )
            if exit_signal:
                if index + 1 < len(bars):
                    close_position(index + 1, float(bars[index + 1]["open"]), "channel_or_ema")
                else:
                    close_position(index, close, "channel_or_ema")
            peak = max(peak, equity)
            max_dd = max(max_dd, (peak - equity) / peak * 100.0)

    if position != 0:
        close_position(len(bars) - 1, float(bars[-1]["close"]), "end_of_data")

    wins = sum(1 for trade in trades if trade.pnl > 0)
    gross_profit = sum(max(0.0, trade.pnl) for trade in trades)
    gross_loss = abs(sum(min(0.0, trade.pnl) for trade in trades))
    profit_factor = gross_profit / gross_loss if gross_loss > 0 else (999.0 if gross_profit > 0 else 0.0)
    return Result(
        symbol,
        len(bars),
        len(trades),
        wins,
        100.0 * wins / len(trades) if trades else 0.0,
        equity,
        (equity / params.initial_equity - 1.0) * 100.0,
        max_dd,
        profit_factor,
        last_signal,
        bars_from=int(bars[0]["datetime"]),
        bars_to=int(bars[-1]["datetime"]),
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--node-url", default="http://127.0.0.1:18081")
    parser.add_argument("--products", nargs="*", default=DEFAULT_PRODUCTS)
    parser.add_argument("--symbol-prefix", default="KQ.i@")
    parser.add_argument("--width", type=int, default=200)
    parser.add_argument("--workers", type=int, default=6)
    parser.add_argument("--output-dir", default="artifacts/strategy_scan")
    parser.add_argument("--execute", action="store_true", help="reserved; not implemented")
    args = parser.parse_args()

    if args.execute:
        raise SystemExit("--execute is intentionally not implemented in the scanner; use the CTP acceptance scripts")

    base = args.node_url.rstrip("/")
    symbols = [f"{args.symbol_prefix}{product}" for product in args.products]
    results: list[Result] = []
    failures: dict[str, str] = {}

    def work(symbol: str) -> None:
        try:
            bars, _ = fetch_bars(base, symbol, args.width)
            if not bars:
                failures[symbol] = "empty bars"
                return
            results.append(backtest(symbol, bars, Params()))
        except Exception as exc:  # noqa: BLE001 - report per-symbol failures
            failures[symbol] = str(exc)

    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.workers)) as pool:
        list(pool.map(work, symbols))

    output = Path(args.output_dir)
    output.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime("%Y%m%d_%H%M%S")
    payload = {
        "generated_at": stamp,
        "node": base,
        "symbol_prefix": args.symbol_prefix,
        "params": asdict(Params()),
        "results": [asdict(item) for item in sorted(results, key=lambda x: x.total_return_pct, reverse=True)],
        "failures": failures,
    }
    (output / f"strategy_scan_{stamp}.json").write_text(json.dumps(payload, ensure_ascii=False, indent=2))
    rows = ["symbol,bars,trades,win_rate,final_equity,total_return_pct,max_drawdown_pct,profit_factor,last_signal"]
    for item in sorted(results, key=lambda x: x.total_return_pct, reverse=True):
        rows.append(
            f"{item.symbol},{item.bars},{item.trades},{item.win_rate:.2f},"
            f"{item.final_equity:.2f},{item.total_return_pct:.2f},{item.max_drawdown_pct:.2f},"
            f"{item.profit_factor:.3f},{item.last_signal}"
        )
    (output / f"strategy_scan_{stamp}.csv").write_text("\n".join(rows) + "\n")
    print("\n".join(rows[: min(len(rows), 18)]))
    if failures:
        print(f"\nfailures: {len(failures)}")
    print(f"\noutput: {output / f'strategy_scan_{stamp}.json'}")
    return 0 if results else 1


if __name__ == "__main__":
    raise SystemExit(main())

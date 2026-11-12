#!/usr/bin/env python3
"""Formal ten-year daily backtest for EMA60 + Donchian22 + ATR stop.

Data input is the Tq backtest MD daily index history produced by
scripts/fetch_tq_daily.py. No third-party plotting library is required: charts
are emitted as self-contained SVG files, which GitHub and browsers render.

Strategy:
  * daily KQ.i@<product> index bars (no roll jump)
  * EMA60, Donchian(22), ATR14
  * long: close > EMA60 and close > prior Donchian upper
  * short: close < EMA60 and close < prior Donchian lower
  * next-open entry, 2*ATR14 initial stop, channel/EMA exit
  * 30% of sleeve equity as initial margin, integer lots
  * contract multiplier and per-lot commission from config/daily_backtest_specs.json
  * one position per product; no pyramiding

This is a frozen research/report simulator for the published ten-year
report. The reusable strategy implementation used by the framework and live
runner is `python/tqsdk_py/strategies/ema_donchian.py`; the generic backtest CLI
is `python/tqsdk_backtest_cli.py --strategy ema_donchian`.

Run:
  python3 scripts/fetch_tq_daily.py --output-dir artifacts/tq_daily
  python3 scripts/backtest_daily.py \
    --data-dir artifacts/tq_daily \
    --report docs/DAILY_STRATEGY_BACKTEST.md \
    --charts-dir docs/backtest_charts \
    --output-dir artifacts/daily_backtest
"""

from __future__ import annotations

import argparse
import csv
import glob
import json
import math
import os
import statistics
import time
from dataclasses import dataclass, field
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any

CST = timezone(timedelta(hours=8))

# ---------------------------------------------------------------------------
# Data and indicators
# ---------------------------------------------------------------------------


@dataclass
class Bar:
    dt: int
    date: Any
    open: float
    high: float
    low: float
    close: float
    volume: float


@dataclass
class Params:
    ema_period: int = 60
    donchian_period: int = 22
    atr_period: int = 14
    stop_atr: float = 2.0
    allocation: float = 0.30
    initial_equity: float = 1_000_000.0
    # Used only when an instrument has no exchange commission snapshot.
    commission_rate: float = 0.0001
    # margin: 30% of sleeve equity as initial margin (leverage from 1/margin_rate)
    # notional: 30% notional exposure (no leverage, comparison mode)
    sizing: str = "margin"
    maintenance_ratio: float = 0.75


@dataclass
class Spec:
    symbol: str
    multiplier: float
    margin_rate: float
    commission_per_lot: float
    price_tick: float = 0.0
    source: str = ""


def load_specs(path: Path) -> dict[str, Spec]:
    payload = json.loads(path.read_text())
    specs: dict[str, Spec] = {}
    for symbol, value in payload.items():
        try:
            specs[symbol] = Spec(
                symbol=symbol,
                multiplier=max(0.0, float(value.get("volume_multiple", 0.0))),
                margin_rate=max(0.0, float(value.get("margin_rate_est", 0.0))),
                commission_per_lot=max(0.0, float(value.get("commission_per_lot", 0.0))),
                price_tick=max(0.0, float(value.get("price_tick", 0.0))),
                source=str(value.get("source", "")),
            )
        except (TypeError, ValueError):
            continue
    return specs


@dataclass
class Trade:
    side: str
    entry_date: str
    exit_date: str
    entry_price: float
    exit_price: float
    pnl: float
    return_pct: float
    holding_days: int
    reason: str
    lots: float = 0.0
    multiplier: float = 0.0
    margin_rate: float = 0.0


@dataclass
class Result:
    symbol: str
    product: str
    bars: int
    start: str
    end: str
    trades: int
    wins: int
    win_rate: float
    final_equity: float
    total_return_pct: float
    cagr_pct: float
    max_drawdown_pct: float
    sharpe: float
    sortino: float
    calmar: float
    profit_factor: float
    avg_trade_pct: float
    median_trade_pct: float
    avg_holding_days: float
    long_trades: int
    short_trades: int
    long_pnl: float
    short_pnl: float
    fees: float
    exposure_pct: float
    buy_hold_pct: float
    yearly: dict[int, float] = field(default_factory=dict)
    equity_dates: list[str] = field(default_factory=list)
    equity: list[float] = field(default_factory=list)
    drawdowns: list[float] = field(default_factory=list)
    trade_details: list[Trade] = field(default_factory=list)
    status: str = "included"
    multiplier: float = 0.0
    margin_rate: float = 0.0
    commission_per_lot: float = 0.0
    avg_lots: float = 0.0
    avg_leverage: float = 0.0
    max_leverage: float = 0.0
    avg_margin_pct: float = 0.0
    skipped_entries: int = 0
    margin_calls: int = 0


def parse_date(ns: int):
    return datetime.fromtimestamp(ns / 1e9, tz=CST).date()


def load_product(path: Path) -> tuple[str, str, list[Bar]]:
    payload = json.loads(path.read_text())
    bars: list[Bar] = []
    for raw in payload.get("bars", []):
        try:
            dt = int(raw["datetime"])
            bar = Bar(
                dt=dt,
                date=parse_date(dt),
                open=float(raw["open"]),
                high=float(raw["high"]),
                low=float(raw["low"]),
                close=float(raw["close"]),
                volume=float(raw.get("volume", 0.0)),
            )
        except (KeyError, TypeError, ValueError):
            continue
        if not all(math.isfinite(x) for x in (bar.open, bar.high, bar.low, bar.close)):
            continue
        bars.append(bar)
    bars.sort(key=lambda item: item.dt)
    # Remove duplicate datetimes, keeping the last observation.
    dedup: list[Bar] = []
    for bar in bars:
        if dedup and dedup[-1].dt == bar.dt:
            dedup[-1] = bar
        else:
            dedup.append(bar)
    return payload.get("symbol", path.stem), payload.get("product", path.stem), dedup


def ema(values: list[float], period: int) -> list[float]:
    out: list[float] = []
    alpha = 2.0 / (period + 1.0)
    current = values[0] if values else math.nan
    for i, value in enumerate(values):
        current = value if i == 0 else current + alpha * (value - current)
        out.append(current)
    return out


def atr(bars: list[Bar], period: int) -> list[float]:
    out = [math.nan] * len(bars)
    trs: list[float] = []
    for i, bar in enumerate(bars):
        if i == 0:
            tr = bar.high - bar.low
        else:
            prev = bars[i - 1].close
            tr = max(bar.high - bar.low, abs(bar.high - prev), abs(bar.low - prev))
        trs.append(tr)
        if i >= period - 1:
            out[i] = sum(trs[i + 1 - period : i + 1]) / period
    return out


def donchian(bars: list[Bar], period: int) -> tuple[list[float], list[float]]:
    upper = [math.nan] * len(bars)
    lower = [math.nan] * len(bars)
    for i in range(period, len(bars)):
        window = bars[i - period : i]
        upper[i] = max(item.high for item in window)
        lower[i] = min(item.low for item in window)
    return upper, lower


def max_drawdown(equity: list[float]) -> float:
    peak = equity[0] if equity else 0.0
    worst = 0.0
    for value in equity:
        peak = max(peak, value)
        if peak > 0:
            worst = min(worst, value / peak - 1.0)
    return worst * 100.0


def daily_drawdowns(equity: list[float]) -> list[float]:
    peak = equity[0] if equity else 0.0
    out: list[float] = []
    for value in equity:
        peak = max(peak, value)
        out.append((value / peak - 1.0) * 100.0 if peak else 0.0)
    return out


def annualized_stats(equity: list[float], dates: list[str]) -> tuple[float, float, float, float]:
    if len(equity) < 2 or len(dates) < 2:
        return 0.0, 0.0, 0.0, 0.0
    returns = [
        equity[i] / equity[i - 1] - 1.0 for i in range(1, len(equity)) if equity[i - 1] > 0
    ]
    if not returns:
        return 0.0, 0.0, 0.0, 0.0
    mean = statistics.fmean(returns)
    std = statistics.pstdev(returns) if len(returns) > 1 else 0.0
    downside = [min(0.0, x) for x in returns]
    downside_std = math.sqrt(statistics.fmean([x * x for x in downside])) if downside else 0.0
    try:
        first = datetime.strptime(dates[0], "%Y-%m-%d").date()
        last = datetime.strptime(dates[-1], "%Y-%m-%d").date()
        years = max(1.0 / 365.25, (last - first).days / 365.25)
    except ValueError:
        years = len(returns) / 252.0
    cagr = (equity[-1] / equity[0]) ** (1.0 / years) - 1.0 if equity[0] > 0 else 0.0
    sharpe = mean / std * math.sqrt(252.0) if std > 0 else 0.0
    sortino = mean / downside_std * math.sqrt(252.0) if downside_std > 0 else 0.0
    calmar = cagr * 100.0 / abs(max_drawdown(equity)) if max_drawdown(equity) < 0 else 0.0
    return cagr * 100.0, sharpe, sortino, calmar


def yearly_returns(dates: list[str], equity: list[float]) -> dict[int, float]:
    if not dates:
        return {}
    by_year: dict[int, list[float]] = {}
    for date_text, value in zip(dates, equity):
        year = int(date_text[:4])
        by_year.setdefault(year, []).append(value)
    result: dict[int, float] = {}
    previous = equity[0]
    for year in sorted(by_year):
        end = by_year[year][-1]
        result[year] = (end / previous - 1.0) * 100.0 if previous > 0 else 0.0
        previous = end
    return result


# ---------------------------------------------------------------------------
# Backtest engine
# ---------------------------------------------------------------------------


def backtest(product: str, symbol: str, bars: list[Bar], params: Params, spec: Spec) -> Result:
    warmup = max(params.ema_period, params.donchian_period, params.atr_period) + 1
    if len(bars) <= warmup + 2:
        return Result(
            symbol, product, len(bars), str(bars[0].date) if bars else "",
            str(bars[-1].date) if bars else "", 0, 0, 0.0, params.initial_equity,
            0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0, 0, 0.0, 0.0,
            0.0, 0.0, {}, status="too_short",
            multiplier=spec.multiplier, margin_rate=spec.margin_rate,
            commission_per_lot=spec.commission_per_lot,
        )
    multiplier = spec.multiplier if spec.multiplier > 0 else 1.0
    margin_rate = spec.margin_rate if spec.margin_rate > 0 else 0.10
    closes = [bar.close for bar in bars]
    ema_values = ema(closes, params.ema_period)
    upper, lower = donchian(bars, params.donchian_period)
    atr_values = atr(bars, params.atr_period)

    cash = params.initial_equity
    position = 0
    lots = 0
    entry_price = 0.0
    entry_date = ""
    entry_equity = 0.0
    entry_leverage = 0.0
    stop_price = 0.0
    pending: tuple[str, str] | None = None
    trades: list[Trade] = []
    equity_dates: list[str] = []
    equity_values: list[float] = []
    fees_paid = 0.0
    exposure_days = 0
    long_trades = short_trades = 0
    long_pnl = short_pnl = 0.0
    lots_samples: list[float] = []
    leverage_samples: list[float] = []
    margin_samples: list[float] = []
    skipped_entries = 0
    margin_calls = 0

    def exit_fee(price: float) -> float:
        if spec.commission_per_lot > 0:
            return spec.commission_per_lot * lots
        return abs(price * multiplier * lots) * params.commission_rate

    def close_trade(exit_date: str, exit_price: float, reason: str, index: int) -> None:
        nonlocal cash, position, lots, entry_price, fees_paid, long_trades, short_trades
        nonlocal long_pnl, short_pnl
        if position == 0:
            return
        gross = (exit_price - entry_price) * multiplier * lots * position
        fee = exit_fee(exit_price)
        cash += gross - fee
        fees_paid += fee
        trade_return = gross / max(1.0, entry_equity) * 100.0
        trade = Trade(
            "long" if position > 0 else "short",
            entry_date,
            exit_date,
            entry_price,
            exit_price,
            gross - fee,
            trade_return,
            max(0, (bars[index].date - datetime.strptime(entry_date, "%Y-%m-%d").date()).days),
            reason,
            lots=float(lots),
            multiplier=multiplier,
            margin_rate=margin_rate,
        )
        trades.append(trade)
        if position > 0:
            long_trades += 1
            long_pnl += trade.pnl
        else:
            short_trades += 1
            short_pnl += trade.pnl
        position = 0
        lots = 0

    start_index = warmup
    for i in range(start_index, len(bars)):
        bar = bars[i]

        if pending is not None:
            action, side_or_reason = pending
            pending = None
            if action == "enter" and position == 0 and bar.open > 0:
                per_lot_notional = bar.open * multiplier
                if per_lot_notional <= 0:
                    continue
                if params.sizing == "notional":
                    lots = int(math.floor((cash * params.allocation) / per_lot_notional))
                    required_margin = lots * per_lot_notional * margin_rate
                else:
                    margin_per_lot = per_lot_notional * margin_rate
                    lots = int(math.floor((cash * params.allocation) / margin_per_lot)) if margin_per_lot > 0 else 0
                    required_margin = lots * per_lot_notional * margin_rate
                if lots < 1:
                    skipped_entries += 1
                    # Do not enter, but continue to evaluate the bar normally.
                else:
                    position = 1 if side_or_reason == "long" else -1
                    entry_price = bar.open
                    entry_date = str(bar.date)
                    entry_equity = cash
                    entry_leverage = lots * per_lot_notional / max(1.0, entry_equity)
                    fee = spec.commission_per_lot * lots if spec.commission_per_lot > 0 else per_lot_notional * lots * params.commission_rate
                    cash -= fee
                    fees_paid += fee
                    stop_price = entry_price - position * params.stop_atr * atr_values[i - 1]
                    lots_samples.append(float(lots))
            elif action == "exit" and position != 0:
                close_trade(str(bar.date), bar.open, side_or_reason, i)

        if position != 0:
            exposure_days += 1
            stop_hit = (position > 0 and bar.low <= stop_price) or (
                position < 0 and bar.high >= stop_price
            )
            if stop_hit:
                if position > 0:
                    exit_price = bar.open if bar.open <= stop_price else stop_price
                else:
                    exit_price = bar.open if bar.open >= stop_price else stop_price
                close_trade(str(bar.date), exit_price, "atr_stop", i)

        # Futures margin call: if mark-to-market equity falls below maintenance
        # margin, liquidate at the close. This is deliberately conservative.
        if position != 0:
            marked = cash + (bar.close - entry_price) * multiplier * lots * position
            required = lots * bar.close * multiplier * margin_rate
            if required > 0 and marked < required * params.maintenance_ratio:
                close_trade(str(bar.date), bar.close, "margin_call", i)
                margin_calls += 1

        if position != 0:
            if position > 0:
                exit_signal = bar.close < ema_values[i] or bar.close < lower[i]
            else:
                exit_signal = bar.close > ema_values[i] or bar.close > upper[i]
            if exit_signal:
                pending = ("exit", "channel_or_ema")
        elif pending is None:
            long_signal = bar.close > ema_values[i] and bar.close > upper[i]
            short_signal = bar.close < ema_values[i] and bar.close < lower[i]
            if long_signal or short_signal:
                if math.isfinite(atr_values[i]) and atr_values[i] > 0:
                    pending = ("enter", "long" if long_signal else "short")

        if position != 0:
            mark = cash + (bar.close - entry_price) * multiplier * lots * position
            notional = lots * bar.close * multiplier
            leverage_samples.append(notional / max(1.0, mark))
            margin_samples.append(lots * bar.close * multiplier * margin_rate / max(1.0, mark) * 100.0)
        else:
            mark = cash
        equity_dates.append(str(bar.date))
        equity_values.append(mark)

    if position != 0:
        close_trade(equity_dates[-1], bars[-1].close, "end_of_data", len(bars) - 1)
        equity_values[-1] = cash

    if not equity_dates:
        return Result(symbol, product, len(bars), str(bars[0].date), str(bars[-1].date), 0,
                      0, 0.0, params.initial_equity, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
                      0.0, 0.0, 0.0, 0, 0, 0.0, 0.0, fees_paid, 0.0, 0.0, {},
                      status="no_equity", multiplier=multiplier, margin_rate=margin_rate,
                      commission_per_lot=spec.commission_per_lot,
                      skipped_entries=skipped_entries, margin_calls=margin_calls)

    total_return = (equity_values[-1] / params.initial_equity - 1.0) * 100.0
    cagr, sharpe, sortino, calmar = annualized_stats(equity_values, equity_dates)
    wins = sum(1 for trade in trades if trade.pnl > 0)
    gross_profit = sum(max(0.0, trade.pnl) for trade in trades)
    gross_loss = abs(sum(min(0.0, trade.pnl) for trade in trades))
    profit_factor = gross_profit / gross_loss if gross_loss > 0 else (999.0 if gross_profit > 0 else 0.0)
    trade_pcts = [trade.return_pct for trade in trades]
    buy_hold = (
        (bars[-1].close / bars[start_index].close - 1.0) * 100.0 if bars[start_index].close else 0.0
    )
    return Result(
        symbol=symbol,
        product=product,
        bars=len(bars),
        start=str(bars[0].date),
        end=str(bars[-1].date),
        trades=len(trades),
        wins=wins,
        win_rate=100.0 * wins / len(trades) if trades else 0.0,
        final_equity=equity_values[-1],
        total_return_pct=total_return,
        cagr_pct=cagr,
        max_drawdown_pct=max_drawdown(equity_values),
        sharpe=sharpe,
        sortino=sortino,
        calmar=calmar,
        profit_factor=profit_factor,
        avg_trade_pct=statistics.fmean(trade_pcts) if trade_pcts else 0.0,
        median_trade_pct=statistics.median(trade_pcts) if trade_pcts else 0.0,
        avg_holding_days=statistics.fmean([trade.holding_days for trade in trades]) if trades else 0.0,
        long_trades=long_trades,
        short_trades=short_trades,
        long_pnl=long_pnl,
        short_pnl=short_pnl,
        fees=fees_paid,
        exposure_pct=100.0 * exposure_days / len(equity_dates),
        buy_hold_pct=buy_hold,
        yearly=yearly_returns(equity_dates, equity_values),
        equity_dates=equity_dates,
        equity=equity_values,
        drawdowns=daily_drawdowns(equity_values),
        trade_details=trades,
        multiplier=multiplier,
        margin_rate=margin_rate,
        commission_per_lot=spec.commission_per_lot,
        avg_lots=statistics.fmean(lots_samples) if lots_samples else 0.0,
        avg_leverage=statistics.fmean(leverage_samples) if leverage_samples else 0.0,
        max_leverage=max(leverage_samples, default=0.0),
        avg_margin_pct=statistics.fmean(margin_samples) if margin_samples else 0.0,
        skipped_entries=skipped_entries,
        margin_calls=margin_calls,
    )


# ---------------------------------------------------------------------------
# Portfolio / aggregation
# ---------------------------------------------------------------------------


def portfolio_curve(results: list[Result], initial: float = 1_000_000.0) -> tuple[list[str], list[float]]:
    dates = sorted({date for result in results for date in result.equity_dates})
    previous: dict[str, float] = {}
    equity = initial
    out_dates: list[str] = []
    out_equity: list[float] = []
    for date in dates:
        rets: list[float] = []
        for result in results:
            pair = dict(zip(result.equity_dates, result.equity))
            if date in pair:
                old = previous.get(result.symbol)
                if old is not None and old > 0:
                    rets.append(pair[date] / old - 1.0)
                previous[result.symbol] = pair[date]
        if rets:
            equity *= 1.0 + statistics.fmean(rets)
            out_dates.append(date)
            out_equity.append(equity)
    return out_dates, out_equity


def portfolio_stats(portfolio: list[Result]) -> dict[str, Any]:
    dates, equity = portfolio_curve(portfolio)
    if len(equity) < 2:
        return {}
    cagr, sharpe, sortino, calmar = annualized_stats(equity, dates)
    return {
        "start": dates[0],
        "end": dates[-1],
        "days": len(dates),
        "final_equity": equity[-1],
        "total_return_pct": (equity[-1] / equity[0] - 1.0) * 100.0,
        "cagr_pct": cagr,
        "max_drawdown_pct": max_drawdown(equity),
        "sharpe": sharpe,
        "sortino": sortino,
        "calmar": calmar,
        "yearly": yearly_returns(dates, equity),
        "equity_dates": dates,
        "equity": equity,
    }


# ---------------------------------------------------------------------------
# SVG charts (no matplotlib / numpy dependency)
# ---------------------------------------------------------------------------


class Svg:
    def __init__(self, width: int, height: int):
        self.width = width
        self.height = height
        self.body: list[str] = []
        self.body.append(
            f'<rect width="{width}" height="{height}" fill="#0d1117"/>'
        )

    def add(self, text: str) -> None:
        self.body.append(text)

    def text(self, x: float, y: float, content: str, size: int = 12,
             fill: str = "#c9d1d9", anchor: str = "start", weight: str = "normal") -> None:
        self.add(
            f'<text x="{x:.1f}" y="{y:.1f}" fill="{fill}" font-size="{size}" '
            f'font-family="DejaVu Sans, Arial, sans-serif" text-anchor="{anchor}" '
            f'font-weight="{weight}">{escape(content)}</text>'
        )

    def line(self, x1: float, y1: float, x2: float, y2: float,
             stroke: str = "#30363d", width: float = 1.0) -> None:
        self.add(
            f'<line x1="{x1:.1f}" y1="{y1:.1f}" x2="{x2:.1f}" y2="{y2:.1f}" '
            f'stroke="{stroke}" stroke-width="{width}"/>'
        )

    def polyline(self, points: list[tuple[float, float]], stroke: str,
                 width: float = 2.0) -> None:
        if len(points) < 2:
            return
        pts = " ".join(f"{x:.1f},{y:.1f}" for x, y in points)
        self.add(
            f'<polyline points="{pts}" fill="none" stroke="{stroke}" '
            f'stroke-width="{width}" stroke-linejoin="round" stroke-linecap="round"/>'
        )

    def rect(self, x: float, y: float, w: float, h: float, fill: str,
             stroke: str = "none") -> None:
        self.add(
            f'<rect x="{x:.1f}" y="{y:.1f}" width="{max(0.1, w):.1f}" '
            f'height="{max(0.1, h):.1f}" fill="{fill}" stroke="{stroke}"/>'
        )

    def circle(self, x: float, y: float, r: float, fill: str) -> None:
        self.add(f'<circle cx="{x:.1f}" cy="{y:.1f}" r="{r:.1f}" fill="{fill}"/>')

    def save(self, path: Path) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(
            f'<svg xmlns="http://www.w3.org/2000/svg" width="{self.width}" '
            f'height="{self.height}" viewBox="0 0 {self.width} {self.height}">\n'
            + "\n".join(self.body)
            + "\n</svg>\n"
        )


def escape(text: str) -> str:
    return (
        str(text).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
        .replace('"', "&quot;")
    )


def svg_line_chart(
    path: Path,
    title: str,
    series: list[tuple[str, list[float], str]],
    xlabels: list[str],
    ylabel: str,
    baseline: float | None = None,
    height: int = 460,
) -> None:
    width = 1000
    left, right, top, bottom = 70, 25, 42, 48
    plot_w = width - left - right
    plot_h = height - top - bottom
    svg = Svg(width, height)
    svg.text(left, 24, title, 16, "#f0f6fc", weight="bold")
    all_values = [value for _, values, _ in series for value in values]
    if not all_values:
        svg.save(path)
        return
    low, high = min(all_values), max(all_values)
    if baseline is not None:
        low = min(low, baseline)
        high = max(high, baseline)
    span = high - low or 1.0
    low -= span * 0.07
    high += span * 0.07
    span = high - low
    for i in range(6):
        y = top + plot_h * i / 5
        value = high - span * i / 5
        svg.line(left, y, width - right, y)
        svg.text(left - 8, y + 4, f"{value:,.2f}", 10, "#8b949e", "end")
    if baseline is not None:
        y = top + (high - baseline) / span * plot_h
        svg.line(left, y, width - right, y, "#8b949e", 1.2)
    n = max(len(values) for _, values, _ in series)
    for i, (_, values, color) in enumerate(series):
        points = []
        for index, value in enumerate(values):
            x = left + (index / max(1, n - 1)) * plot_w
            y = top + (high - value) / span * plot_h
            points.append((x, y))
        svg.polyline(points, color, 1.8)
        svg.text(width - right - 4, top + 16 + i * 17, series[i][0], 10, color, "end")
    for i in range(7):
        index = round(i * (len(xlabels) - 1) / 6) if xlabels else 0
        if xlabels and 0 <= index < len(xlabels):
            x = left + (index / max(1, len(xlabels) - 1)) * plot_w
            svg.line(x, top + plot_h, x, top + plot_h + 4, "#8b949e")
            svg.text(x, height - 20, xlabels[index], 10, "#8b949e", "middle")
    svg.text(16, top + plot_h / 2, ylabel, 10, "#8b949e", "middle")
    svg.save(path)


def svg_horizontal_bars(path: Path, title: str, items: list[tuple[str, float]], color_positive: str,
                        color_negative: str, width: int = 1000, row_h: int = 24,
                        suffix: str = "%") -> None:
    top, left, right = 50, 180, 80
    height = top + max(1, len(items)) * row_h + 30
    svg = Svg(width, height)
    svg.text(left, 24, title, 16, "#f0f6fc", weight="bold")
    if not items:
        svg.save(path)
        return
    low = min(0.0, min(value for _, value in items))
    high = max(0.0, max(value for _, value in items))
    span = high - low or 1.0
    zero_x = left + (0.0 - low) / span * (width - left - right)
    for i, (label, value) in enumerate(items):
        y = top + i * row_h
        x = left + (value - low) / span * (width - left - right)
        svg.line(left, y + row_h - 2, width - right, y + row_h - 2, "#21262d", 0.5)
        svg.text(left - 8, y + row_h // 2 + 3, label, 9, "#c9d1d9", "end")
        if value >= 0:
            svg.rect(zero_x, y + 4, x - zero_x, row_h - 8, color_positive)
            svg.text(max(x + 5, zero_x + 5), y + row_h // 2 + 3, f"{value:+.2f}{suffix}", 9, "#c9d1d9")
        else:
            svg.rect(x, y + 4, zero_x - x, row_h - 8, color_negative)
            svg.text(min(x - 5, zero_x - 5), y + row_h // 2 + 3, f"{value:+.2f}{suffix}", 9, "#c9d1d9", "end")
    svg.line(zero_x, top, zero_x, height - 20, "#8b949e", 1)
    svg.save(path)


def svg_scatter(path: Path, title: str, points: list[tuple[float, float, str]],
                xlabel: str, ylabel: str, height: int = 520) -> None:
    width = 1000
    left, right, top, bottom = 75, 30, 45, 55
    plot_w, plot_h = width - left - right, height - top - bottom
    svg = Svg(width, height)
    svg.text(left, 24, title, 16, "#f0f6fc", weight="bold")
    if not points:
        svg.save(path)
        return
    xs = [p[0] for p in points]
    ys = [p[1] for p in points]
    xmin, xmax = min(xs), max(xs)
    ymin, ymax = min(ys), max(ys)
    xspan, yspan = (xmax - xmin) or 1.0, (ymax - ymin) or 1.0
    xmin -= xspan * 0.08
    xmax += xspan * 0.08
    ymin -= yspan * 0.08
    ymax += yspan * 0.08
    for i in range(6):
        y = top + plot_h * i / 5
        value = ymax - (ymax - ymin) * i / 5
        svg.line(left, y, width - right, y)
        svg.text(left - 8, y + 4, f"{value:.1f}", 10, "#8b949e", "end")
        x = left + plot_w * i / 5
        value_x = xmin + (xmax - xmin) * i / 5
        svg.line(x, top + plot_h, x, top + plot_h + 4, "#8b949e")
        svg.text(x, height - 20, f"{value_x:.1f}", 10, "#8b949e", "middle")
    svg.text(left + plot_w / 2, height - 3, xlabel, 10, "#8b949e", "middle")
    svg.text(16, top + plot_h / 2, ylabel, 10, "#8b949e", "middle")
    for xval, yval, label in points:
        x = left + (xval - xmin) / (xmax - xmin) * plot_w
        y = top + (ymax - yval) / (ymax - ymin) * plot_h
        color = "#3fb950" if yval >= 0 else "#f85149"
        svg.circle(x, y, 4, color)
        if label in {"KQ.i@GFEX.lc", "KQ.i@SHFE.fu", "KQ.i@DCE.jm", "KQ.i@CZCE.SA"}:
            svg.text(x + 6, y - 4, label.split("@")[-1], 9, "#c9d1d9")
    svg.save(path)


def svg_heatmap(path: Path, title: str, years: list[int], rows: list[tuple[str, dict[int, float]]],
                width: int = 1100) -> None:
    row_h = 18
    left, top, right = 190, 70, 30
    height = top + max(1, len(rows)) * row_h + 45
    svg = Svg(width, height)
    svg.text(left, 25, title, 16, "#f0f6fc", weight="bold")
    values = [value for _, row in rows for value in row.values()]
    low, high = min(values, default=0.0), max(values, default=0.0)
    span = high - low or 1.0
    cell_w = (width - left - right) / max(1, len(years))
    for j, year in enumerate(years):
        x = left + j * cell_w
        svg.text(x + cell_w / 2, top - 8, str(year), 10, "#8b949e", "middle")
    for i, (label, row) in enumerate(rows):
        y = top + i * row_h
        svg.text(left - 8, y + 13, label, 9, "#c9d1d9", "end")
        for j, year in enumerate(years):
            value = row.get(year, math.nan)
            if math.isnan(value):
                fill = "#161b22"
                text = ""
            else:
                norm = (value - low) / span
                if norm >= 0.5:
                    t = (norm - 0.5) * 2
                    fill = f"rgb({int(35 + t * 25)},{int(100 + t * 70)},{int(60)})"
                else:
                    t = norm * 2
                    fill = f"rgb({int(130 - t * 80)},{int(35 + t * 20)},{int(40)})"
                text = f"{value:.1f}"
            svg.rect(left + j * cell_w + 1, y + 1, cell_w - 2, row_h - 2, fill)
            if text:
                svg.text(left + j * cell_w + cell_w / 2, y + 12, text, 8, "#f0f6fc", "middle")
    svg.save(path)


def svg_histogram(path: Path, title: str, values: list[float], width: int = 1000,
                  height: int = 420) -> None:
    left, right, top, bottom = 65, 25, 42, 45
    svg = Svg(width, height)
    svg.text(left, 24, title, 16, "#f0f6fc", weight="bold")
    if not values:
        svg.save(path)
        return
    bins = 25
    low, high = min(values), max(values)
    if high == low:
        high = low + 1
    counts = [0] * bins
    for value in values:
        index = min(bins - 1, int((value - low) / (high - low) * bins))
        counts[index] += 1
    max_count = max(counts) or 1
    plot_w, plot_h = width - left - right, height - top - bottom
    for i, count in enumerate(counts):
        x = left + i * plot_w / bins
        bar_h = count / max_count * plot_h
        svg.rect(x + 1, top + plot_h - bar_h, plot_w / bins - 2, bar_h, "#58a6ff")
    for i in range(5):
        x = left + plot_w * i / 4
        value = low + (high - low) * i / 4
        svg.line(x, top + plot_h, x, top + plot_h + 4, "#8b949e")
        svg.text(x, height - 20, f"{value:.1f}%", 10, "#8b949e", "middle")
    svg.text(16, top + plot_h / 2, "trades", 10, "#8b949e", "middle")
    svg.save(path)


# ---------------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------------


def fmt(value: float, digits: int = 2) -> str:
    return f"{value:,.{digits}f}"


def write_csv(path: Path, results: list[Result]) -> None:
    fields = [
        "symbol", "product", "bars", "start", "end", "trades", "wins", "win_rate",
        "final_equity", "total_return_pct", "cagr_pct", "max_drawdown_pct", "sharpe",
        "sortino", "calmar", "profit_factor", "avg_trade_pct", "median_trade_pct",
        "avg_holding_days", "long_trades", "short_trades", "long_pnl", "short_pnl",
        "fees", "exposure_pct", "buy_hold_pct", "multiplier", "margin_rate",
        "avg_lots", "avg_leverage", "max_leverage", "avg_margin_pct",
        "skipped_entries", "margin_calls",
    ]
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for result in results:
            row = {field: getattr(result, field) for field in fields}
            writer.writerow(row)


def write_report(
    path: Path,
    results: list[Result],
    excluded: list[dict[str, str]],
    params: Params,
    portfolio: dict[str, Any],
    first_half: dict[str, Any],
    second_half: dict[str, Any],
    charts: dict[str, str],
    specs: dict[str, Spec],
    notional_portfolio: dict[str, Any],
    sizing_mode: str,
) -> None:
    included = len(results)
    total_trades = sum(result.trades for result in results)
    winners = sum(result.wins for result in results)
    positive = sum(1 for result in results if result.total_return_pct > 0)
    avg_return = statistics.fmean([result.total_return_pct for result in results]) if results else 0
    median_return = statistics.median([result.total_return_pct for result in results]) if results else 0
    avg_dd = statistics.fmean([result.max_drawdown_pct for result in results]) if results else 0
    avg_sharpe = statistics.fmean([result.sharpe for result in results]) if results else 0
    total_long = sum(result.long_trades for result in results)
    total_short = sum(result.short_trades for result in results)
    long_wins = sum(trade.pnl > 0 for result in results for trade in result.trade_details if trade.side == "long")
    short_wins = sum(trade.pnl > 0 for result in results for trade in result.trade_details if trade.side == "short")
    long_trades = [trade for result in results for trade in result.trade_details if trade.side == "long"]
    short_trades = [trade for result in results for trade in result.trade_details if trade.side == "short"]
    years = sorted(portfolio.get("yearly", {}).keys())
    all_trades_for_stats = [trade for result in results for trade in result.trade_details]
    gross_profit_all = sum(max(0.0, trade.pnl) for trade in all_trades_for_stats)
    gross_loss_all = abs(sum(min(0.0, trade.pnl) for trade in all_trades_for_stats))
    portfolio_pf = gross_profit_all / gross_loss_all if gross_loss_all > 0 else (
        999.0 if gross_profit_all > 0 else 0.0
    )
    avg_trade_all = statistics.fmean([trade.return_pct for trade in all_trades_for_stats]) if all_trades_for_stats else 0.0
    avg_hold_all = statistics.fmean([trade.holding_days for trade in all_trades_for_stats]) if all_trades_for_stats else 0.0
    total_fees = sum(result.fees for result in results)
    positive_years = sum(1 for value in portfolio.get("yearly", {}).values() if value > 0)
    best_year = max(portfolio.get("yearly", {}).items(), key=lambda item: item[1], default=("", 0.0))
    worst_year = min(portfolio.get("yearly", {}).items(), key=lambda item: item[1], default=("", 0.0))

    lines: list[str] = []
    lines += [
        "# 日线 EMA60 + Donchian22 + ATR 策略回测报告",
        "",
        f"- 生成时间：{time.strftime('%Y-%m-%d %H:%M:%S')}",
        f"- 数据源：Tq 官方 Backtest MD `KQ.i@<product>` 日线指数连续",
        f"- 原始数据区间：2016-01-05 至 2026-11-11（各品种上市日不同）",
        f"- 可交易信号区间：{portfolio.get('start', '')} 至 {portfolio.get('end', '')}",
        f"- 信号周期：日线",
        f"- 参数：EMA{params.ema_period}、Donchian{params.donchian_period}、ATR{params.atr_period}、{params.stop_atr:.0f}ATR 止损、{params.allocation*100:.0f}% 保证金仓位、整数手",
        "- 执行：信号在收盘产生，下一交易日开盘成交；盘中止损；不进行同日反手；每品种最多 1 个方向仓位。",
        "- 杠杆：保证金模式按 `lots = floor(权益 × 30% / (价格 × 合约乘数 × 保证金率))` 计算；保证金率来自当前合约规格快照。",
        "",
        "## 1. 数据与样本",
        "",
        f"- 原始品种数：{included + len(excluded)}",
        f"- 纳入回测：**{included}** 个",
        f"- 排除：**{len(excluded)}** 个（成交量极低、历史不足或数据为空）",
        f"- 总交易笔数：{total_trades}",
        f"- 组合期初/期末权益：{fmt(params.initial_equity, 0)} → {fmt(portfolio.get('final_equity', params.initial_equity), 0)}",
        "",
        "### 1.1 排除清单",
        "",
        "| 品种 | 原因 |",
        "|---|---|",
    ]
    for item in excluded:
        lines.append(f"| {item['symbol']} | {item['reason']} |")
    lines += [
        "",
        "> 流动性规则：最近 250 根日线的成交量中位数低于阈值、零成交占比过高、历史少于最小 bar 数，或远端无数据。",
        "",
        "### 1.2 合约规格快照",
        "",
        "| 品种 | 合约乘数 | 保证金率估计 | 每手手续费 |",
        "|---|---:|---:|---:|",
    ]
    for symbol in sorted(specs):
        if symbol not in {result.symbol for result in results}:
            continue
        spec = specs[symbol]
        lines.append(
            f"| {symbol} | {spec.multiplier:g} | {spec.margin_rate*100:.2f}% | {spec.commission_per_lot:.2f} |"
        )
    lines += [

        "",
        "| 指标 | 数值 |",
        "|---|---:|",
        f"| 交易品种数 | {included} |",
        f"| 总交易数 | {total_trades} |",
        f"| 盈利品种数 | {positive} |",
        f"| 品种胜率（正收益占比） | {positive/included*100 if included else 0:.2f}% |",
        f"| 组合总收益 | {portfolio.get('total_return_pct', 0):+.2f}% |",
        f"| 组合 CAGR | {portfolio.get('cagr_pct', 0):+.2f}% |",
        f"| 组合最大回撤 | {portfolio.get('max_drawdown_pct', 0):.2f}% |",
        f"| 组合 Sharpe | {portfolio.get('sharpe', 0):.3f} |",
        f"| 组合 Sortino | {portfolio.get('sortino', 0):.3f} |",
        f"| 组合 Calmar | {portfolio.get('calmar', 0):.3f} |",
        f"| 单品种平均收益 | {avg_return:+.2f}% |",
        f"| 单品种收益中位数 | {median_return:+.2f}% |",
        f"| 单品种平均最大回撤 | {avg_dd:.2f}% |",
        f"| 单品种平均 Sharpe | {avg_sharpe:.3f} |",
        f"| 单品种平均名义杠杆 | {statistics.fmean([r.avg_leverage for r in results]):.2f}x |",
        f"| 单品种最大名义杠杆 | {max((r.max_leverage for r in results), default=0):.2f}x |",
        f"| 单品种平均保证金占用 | {statistics.fmean([r.avg_margin_pct for r in results]):.2f}% |",
        f"| 因一手保证金超过预算跳过的信号 | {sum(r.skipped_entries for r in results)} |",
        f"| 保证金强平次数 | {sum(r.margin_calls for r in results)} |",
        f"| 全部交易 Profit Factor | {portfolio_pf:.3f} |",
        f"| 全部交易平均单笔收益 | {avg_trade_all:+.3f}% |",
        f"| 全部交易平均持仓天 | {avg_hold_all:.1f} |",
        f"| 累计手续费（69 个独立账户） | {total_fees:,.0f} |",
        f"| 年度正收益/总年份 | {positive_years}/{len(portfolio.get('yearly', {}))} |",
        f"| 最好年度 | {best_year[0]} {best_year[1]:+.2f}% |",
        f"| 最差年度 | {worst_year[0]} {worst_year[1]:+.2f}% |",
        f"| 多头/空头交易数 | {total_long}/{total_short} |",
        f"| 多头/空头胜率 | {100*long_wins/len(long_trades) if long_trades else 0:.1f}% / {100*short_wins/len(short_trades) if short_trades else 0:.1f}% |",
        "",
        f"![组合权益与回撤]({charts['portfolio']})",
        "",
        "### 2.1 杠杆模式对照",
        "",
        "同一策略、同一品种池，分别按 **30% 保证金** 和 **30% 名义（无杠杆）** 计算：",
        "",
        "| 模型 | 总收益 | CAGR | 最大回撤 | Sharpe |",
        "|---|---:|---:|---:|---:|",
        f"| 30% 保证金（主模型） | {portfolio.get('total_return_pct', 0):+.2f}% | {portfolio.get('cagr_pct', 0):+.2f}% | {portfolio.get('max_drawdown_pct', 0):.2f}% | {portfolio.get('sharpe', 0):.2f} |",
        f"| 30% 名义（无杠杆对照） | {notional_portfolio.get('total_return_pct', 0):+.2f}% | {notional_portfolio.get('cagr_pct', 0):+.2f}% | {notional_portfolio.get('max_drawdown_pct', 0):.2f}% | {notional_portfolio.get('sharpe', 0):.2f} |",
        "",
        f"主模型：`{sizing_mode}`。保证金模式每品种只使用 30% 权益作为初始保证金，余下通过合约乘数/保证金率形成杠杆；名义模式每品种只持有 30% 名义价值，不使用杠杆。",
        "",
        "### 2.2 年度收益",
        "",
        "| 年份 | 组合收益 |",
        "|---|---:|",
    ]
    for year in years:
        lines.append(f"| {year} | {portfolio['yearly'][year]:+.2f}% |")
    lines += [
        "",
        f"![年度收益]({charts['yearly']})",
        "",
        "## 3. 横向对比：品种收益与风险",
        "",
        "### 3.1 收益排名（全部纳入品种）",
        "",
        "> 总收益是各品种从自己的上市日开始累计，新品种的样本期更短；横向比较请同时看 CAGR、最大回撤、Sharpe 和起止日期。",
        "",
        "| 排名 | 品种 | 总收益 | CAGR | 最大回撤 | Sharpe | 交易数 | 胜率 |",
        "|---:|---|---:|---:|---:|---:|---:|---:|",
    ]
    for rank, result in enumerate(sorted(results, key=lambda item: item.total_return_pct, reverse=True), 1):
        lines.append(
            f"| {rank} | {result.symbol} | {result.total_return_pct:+.2f}% | "
            f"{result.cagr_pct:+.2f}% | {result.max_drawdown_pct:.2f}% | "
            f"{result.sharpe:.2f} | {result.trades} | {result.win_rate:.1f}% |"
        )
    lines += [
        "",
        f"![收益排名]({charts['ranking']})",
        "",
        "### 3.2 收益-回撤散点",
        "",
        f"![风险收益散点]({charts['scatter']})",
        "",
        "### 3.2.1 杠杆分布",
        "",
        f"![品种杠杆分布]({charts['leverage']})",
        "",
        "### 3.3 分品种详细数据",
        "",
        "| 品种 | 区间 | Bars | 交易 | 胜率 | 期末权益 | 总收益 | CAGR | 最大回撤 | Sharpe | 杠杆均值 | 保证金占用 | 平均手数 | 跳过信号 | 多/空交易 | 买入持有 |",
        "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for result in sorted(results, key=lambda item: item.total_return_pct, reverse=True):
        lines.append(
            f"| {result.symbol} | {result.start}~{result.end} | {result.bars} | {result.trades} | "
            f"{result.win_rate:.1f}% | {result.final_equity:,.0f} | {result.total_return_pct:+.2f}% | {result.cagr_pct:+.2f}% | "
            f"{result.max_drawdown_pct:.2f}% | {result.sharpe:.2f} | {result.avg_leverage:.2f}x | "
            f"{result.avg_margin_pct:.1f}% | {result.avg_lots:.1f} | {result.skipped_entries} | "
            f"{result.long_trades}/{result.short_trades} | {result.buy_hold_pct:+.2f}% |"
        )
    quantile_returns = statistics.quantiles([r.total_return_pct for r in results], n=4)
    quantile_dd = statistics.quantiles([r.max_drawdown_pct for r in results], n=4)
    lines += [
        "",
        "### 3.4 横截面分布（69 个品种）",
        "",
        "| 指标 | Q1 | 中位数 | Q3 |",
        "|---|---:|---:|---:|",
        f"| 总收益 | {quantile_returns[0]:+.2f}% | {median_return:+.2f}% | {quantile_returns[2]:+.2f}% |",
        f"| 最大回撤 | {quantile_dd[0]:.2f}% | {statistics.median([r.max_drawdown_pct for r in results]):.2f}% | {quantile_dd[2]:.2f}% |",
        "",
        f"![品种年度收益热力图]({charts['heatmap']})",
        "",
        f"![头部品种权益曲线]({charts['top_equity']})",
        "",
        "## 4. 纵向对比：前后半段",
        "",
        "将回测期按时间分为前半段和后半段，观察策略稳定性：",
        "",
        "| 区间 | 组合收益 | CAGR | 最大回撤 | Sharpe | 正收益品种占比 |",
        "|---|---:|---:|---:|---:|---:|",
    ]
    for label, stats in (("前半段", first_half), ("后半段", second_half)):
        lines.append(
            f"| {label} | {stats.get('total_return_pct', 0):+.2f}% | "
            f"{stats.get('cagr_pct', 0):+.2f}% | {stats.get('max_drawdown_pct', 0):.2f}% | "
            f"{stats.get('sharpe', 0):.2f} | {stats.get('positive_pct', 0):.1f}% |"
        )
    lines += [
        "",
        "| 品种 | 前半段收益 | 后半段收益 | 变化 |",
        "|---|---:|---:|---:|",
    ]
    half_rows: list[tuple[str, float, float]] = []
    for result in results:
        early = 1.0
        late = 1.0
        for year, value in result.yearly.items():
            if year <= 2021:
                early *= 1.0 + value / 100.0
            if year >= 2022:
                late *= 1.0 + value / 100.0
        half_rows.append((result.symbol, (early - 1.0) * 100.0, (late - 1.0) * 100.0))
    for symbol, early, late in sorted(half_rows, key=lambda item: item[2] - item[1], reverse=True)[:20]:
        lines.append(f"| {symbol} | {early:+.2f}% | {late:+.2f}% | {late - early:+.2f}% |")
    lines += [
        "",
        "## 5. 多空拆解",
        "",
        "> 多空 PnL 是 69 个品种各自独立 100 万账户的合计，不是组合账户的现金；组合收益见第 2 节。",
        "",
        "| 方向 | 交易数 | 胜率 | 平均单笔收益 | 总 PnL |",
        "|---|---:|---:|---:|---:|",
        f"| 多 | {len(long_trades)} | {100*long_wins/len(long_trades) if long_trades else 0:.1f}% | {statistics.fmean([t.return_pct for t in long_trades]) if long_trades else 0:+.3f}% | {sum(t.pnl for t in long_trades):,.0f} |",
        f"| 空 | {len(short_trades)} | {100*short_wins/len(short_trades) if short_trades else 0:.1f}% | {statistics.fmean([t.return_pct for t in short_trades]) if short_trades else 0:+.3f}% | {sum(t.pnl for t in short_trades):,.0f} |",
        "",
        f"![多空交易]({charts['long_short']})",
        "",
        "## 6. 交易分布与风险",
        "",
        f"![交易收益分布]({charts['histogram']})",
        "",
        "### 6.1 最大回撤前 10 品种",
        "",
        "| 品种 | 最大回撤 | 总收益 | Sharpe |",
        "|---|---:|---:|---:|",
    ]
    for result in sorted(results, key=lambda item: item.max_drawdown_pct)[:10]:
        lines.append(
            f"| {result.symbol} | {result.max_drawdown_pct:.2f}% | "
            f"{result.total_return_pct:+.2f}% | {result.sharpe:.2f} |"
        )
    lines += [
        "",
        "## 7. 结论与边界",
        "",
        "1. 本报告使用 Tq 官方 Backtest MD 日线指数连续，覆盖 2016-01 至 2026-11，约 2,635 根日线；",
        "2. 回测信号和成交严格按“收盘产生信号、下一交易日开盘成交”，止损在盘中触发；",
        "3. 组合曲线是各品种策略日收益的等权平均，不是统一资金账户的逐笔撮合结果；每个品种使用 30% 权益作为初始保证金，组合平均杠杆见第 2 节；",
        "4. 使用当前品种池和当前流动性过滤，存在幸存者偏差；",
        "5. 合约乘数、每手手续费和保证金率来自当前 `config/daily_backtest_specs.json` 快照；保证金率是当前每手保证金/(最新指数收盘×乘数)的估算，历史保证金调整未逐日建模；",
        "6. 交易成本使用当前每手手续费，未模拟滑点、涨跌停、强平滑点和合约乘数/手续费历史变化；",
        "7. 指数连续没有换月跳空，适合研究信号，但不能直接当作可交易主连价格；",
        "8. 单品种交易次数有限时，胜率/PF 的统计显著性有限，需结合多品种和分年度结果看。",
        "",
        "### 7.1 机器可读文件",
        "",
        "```text",
        "artifacts/daily_backtest/results.csv",
        "artifacts/daily_backtest/results.json",
        "artifacts/daily_backtest/trades.csv",
        "artifacts/daily_backtest/yearly_returns.csv",
        "artifacts/daily_backtest/equity_curves.csv",
        "artifacts/daily_backtest/portfolio_equity.csv",
        "```",
        "",
        "## 8. 复现",
        "",
        "```bash",
        "python3 scripts/fetch_tq_daily.py --output-dir artifacts/tq_daily",
        "python3 scripts/backtest_daily.py \\",
        "  --data-dir artifacts/tq_daily \\",
        "  --report docs/DAILY_STRATEGY_BACKTEST.md \\",
        "  --charts-dir docs/backtest_charts \\",
        "  --output-dir artifacts/daily_backtest",
        "```",
        "",
    ]
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n")


def run_half(results: list[Result], start_year: int, end_year: int) -> dict[str, Any]:
    subset = []
    for result in results:
        eq_dates = [d for d in result.equity_dates if start_year <= int(d[:4]) <= end_year]
        if len(eq_dates) < 2:
            continue
        eq = [result.equity[result.equity_dates.index(d)] for d in eq_dates]
        subset.append(
            Result(
                symbol=result.symbol, product=result.product, bars=result.bars,
                start=eq_dates[0], end=eq_dates[-1], trades=result.trades,
                wins=result.wins, win_rate=result.win_rate, final_equity=eq[-1],
                total_return_pct=(eq[-1] / eq[0] - 1.0) * 100.0,
                cagr_pct=0.0, max_drawdown_pct=max_drawdown(eq), sharpe=0.0,
                sortino=0.0, calmar=0.0, profit_factor=result.profit_factor,
                avg_trade_pct=result.avg_trade_pct, median_trade_pct=result.median_trade_pct,
                avg_holding_days=result.avg_holding_days, long_trades=result.long_trades,
                short_trades=result.short_trades, long_pnl=result.long_pnl,
                short_pnl=result.short_pnl, fees=result.fees, exposure_pct=result.exposure_pct,
                buy_hold_pct=result.buy_hold_pct,
                equity_dates=eq_dates, equity=eq,
                drawdowns=daily_drawdowns(eq),
            )
        )
    dates, equity = portfolio_curve(subset) if subset else ([], [])
    if len(equity) < 2:
        return {}
    cagr, sharpe, sortino, calmar = annualized_stats(equity, dates)
    return {
        "start": dates[0], "end": dates[-1], "total_return_pct": (equity[-1] / equity[0] - 1.0) * 100.0,
        "cagr_pct": cagr, "max_drawdown_pct": max_drawdown(equity), "sharpe": sharpe,
        "sortino": sortino, "calmar": calmar,
        "positive_pct": 100.0 * sum(1 for item in subset if item.total_return_pct > 0) / len(subset) if subset else 0.0,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--data-dir", default="artifacts/tq_daily")
    parser.add_argument("--report", default="docs/DAILY_STRATEGY_BACKTEST.md")
    parser.add_argument("--charts-dir", default="docs/backtest_charts")
    parser.add_argument("--output-dir", default="artifacts/daily_backtest")
    parser.add_argument("--start", default="2016-01-01")
    parser.add_argument("--end", default="2026-12-31")
    parser.add_argument("--min-bars", type=int, default=500)
    parser.add_argument("--min-median-volume", type=float, default=5000.0)
    parser.add_argument("--min-positive-volume-ratio", type=float, default=0.8)
    parser.add_argument("--ema", type=int, default=60)
    parser.add_argument("--donchian", type=int, default=22)
    parser.add_argument("--atr", type=int, default=14)
    parser.add_argument("--stop-atr", type=float, default=2.0)
    parser.add_argument("--allocation", type=float, default=0.30)
    parser.add_argument("--commission", type=float, default=0.0001)
    parser.add_argument("--spec-file", default="config/daily_backtest_specs.json")
    parser.add_argument("--sizing", choices=["margin", "notional"], default="margin")
    parser.add_argument("--maintenance-ratio", type=float, default=0.75)
    args = parser.parse_args()
    params = Params(
        ema_period=args.ema,
        donchian_period=args.donchian,
        atr_period=args.atr,
        stop_atr=args.stop_atr,
        allocation=args.allocation,
        commission_rate=args.commission,
        sizing=args.sizing,
        maintenance_ratio=args.maintenance_ratio,
    )
    specs = load_specs(Path(args.spec_file))
    data_dir = Path(args.data_dir)
    paths = sorted(Path(item) for item in glob.glob(str(data_dir / "KQ.i@*.json")))
    results: list[Result] = []
    universe: list[tuple[str, str, list[Bar], Spec]] = []
    excluded: list[dict[str, str]] = []
    failure_file = data_dir / "_failures.json"
    if failure_file.exists():
        failures = json.loads(failure_file.read_text())
        for symbol, reason in sorted(failures.items()):
            excluded.append({"symbol": symbol, "reason": f"远端无数据：{reason}"})
    for path in paths:
        symbol, product, bars = load_product(path)
        bars = [bar for bar in bars if args.start <= str(bar.date) <= args.end]
        if not bars:
            excluded.append({"symbol": symbol, "reason": "区间内无数据"})
            continue
        if len(bars) < args.min_bars:
            excluded.append({"symbol": symbol, "reason": f"历史过短：{len(bars)} < {args.min_bars} bars"})
            continue
        recent_volume = [bar.volume for bar in bars[-250:]]
        median_volume = statistics.median(recent_volume) if recent_volume else 0.0
        positive_ratio = (
            sum(1 for value in recent_volume if value > 0) / len(recent_volume) if recent_volume else 0.0
        )
        if median_volume < args.min_median_volume or positive_ratio < args.min_positive_volume_ratio:
            excluded.append(
                {
                    "symbol": symbol,
                    "reason": f"流动性不足：median volume={median_volume:.0f}, 零成交占比={100*(1-positive_ratio):.1f}%",
                }
            )
            continue
        spec = specs.get(symbol)
        if spec is None or spec.multiplier <= 0 or spec.margin_rate <= 0:
            excluded.append({"symbol": symbol, "reason": "缺少合约乘数/保证金率规格"})
            continue
        universe.append((product, symbol, bars, spec))
        result = backtest(product, symbol, bars, params, spec)
        if result.trades == 0:
            excluded.append({"symbol": symbol, "reason": "区间内无交易信号"})
            continue
        results.append(result)

    if not results:
        print("no products included", file=os.sys.stderr)
        return 1
    output = Path(args.output_dir)
    charts = Path(args.charts_dir)
    output.mkdir(parents=True, exist_ok=True)
    charts.mkdir(parents=True, exist_ok=True)
    portfolio = portfolio_stats(results)
    notional_params = Params(
        ema_period=params.ema_period,
        donchian_period=params.donchian_period,
        atr_period=params.atr_period,
        stop_atr=params.stop_atr,
        allocation=params.allocation,
        initial_equity=params.initial_equity,
        commission_rate=params.commission_rate,
        sizing="notional",
        maintenance_ratio=params.maintenance_ratio,
    )
    notional_results = [
        result
        for product, symbol, bars, spec in universe
        for result in [backtest(product, symbol, bars, notional_params, spec)]
        if result.trades > 0
    ]
    notional_portfolio = portfolio_stats(notional_results)
    first_half = run_half(results, 2016, 2021)
    second_half = run_half(results, 2022, 2026)

    # Ensure chart references from the Markdown file are relative to docs/.
    try:
        chart_prefix = os.path.relpath(charts, Path(args.report).parent)
    except ValueError:
        chart_prefix = str(charts)

    def chart_ref(name: str) -> str:
        return f"{chart_prefix}/{name}.svg"

    dates = portfolio["equity_dates"]
    equity = portfolio["equity"]
    drawdowns = daily_drawdowns(equity)
    svg_line_chart(
        charts / "portfolio_equity.svg",
        "Portfolio equal-weight strategy equity",
        [("equity", equity, "#58a6ff")],
        dates,
        "Equity",
    )
    svg_line_chart(
        charts / "portfolio_drawdown.svg",
        "Portfolio drawdown",
        [("drawdown %", drawdowns, "#f85149")],
        dates,
        "Drawdown %",
        baseline=0.0,
    )
    # Combined equity/drawdown image: a second standalone drawdown chart is linked too.
    yearly_items = [(str(year), value) for year, value in sorted(portfolio["yearly"].items())]
    svg_horizontal_bars(
        charts / "yearly_portfolio.svg",
        "Portfolio yearly return",
        yearly_items,
        "#3fb950",
        "#f85149",
    )
    sorted_results = sorted(results, key=lambda item: item.total_return_pct, reverse=True)
    ranking = [
        (item.symbol.replace("KQ.i@", ""), item.total_return_pct)
        for item in sorted_results[:20] + sorted_results[-20:]
    ]
    svg_horizontal_bars(
        charts / "return_ranking.svg",
        "Top 20 / bottom 20 products: total return (full table in report)",
        ranking,
        "#3fb950",
        "#f85149",
        width=1100,
    )
    svg_scatter(
        charts / "risk_return_scatter.svg",
        "Risk / return by product",
        [(item.max_drawdown_pct, item.total_return_pct, item.symbol) for item in results],
        "Max drawdown %",
        "Total return %",
    )
    leverage_sorted = sorted(results, key=lambda item: item.avg_leverage, reverse=True)
    leverage_items = [
        (item.symbol.replace("KQ.i@", ""), item.avg_leverage)
        for item in leverage_sorted[:15] + leverage_sorted[-15:]
    ]
    svg_horizontal_bars(
        charts / "leverage_ranking.svg",
        "Average notional leverage by product (margin sizing)",
        leverage_items,
        "#d29922",
        "#8b949e",
        width=1000,
        suffix="x",
    )
    years = sorted({year for item in results for year in item.yearly})
    svg_heatmap(
        charts / "yearly_heatmap.svg",
        "Yearly return heatmap by product",
        years,
        [(item.symbol.replace("KQ.i@", ""), item.yearly) for item in sorted_results],
    )
    top = sorted_results[:8]
    common_start = max(item.equity_dates[0] for item in top)
    common_end = min(item.equity_dates[-1] for item in top)
    common_dates = sorted(
        set.intersection(*[{d for d in item.equity_dates if common_start <= d <= common_end} for item in top])
    )
    series_top = []
    top_colors = ["#58a6ff", "#3fb950", "#d29922", "#f85149", "#a371f7", "#39c5cf", "#ff7b72", "#8b949e"]
    for item, color in zip(top, top_colors):
        values_by_date = dict(zip(item.equity_dates, item.equity))
        base = values_by_date[common_dates[0]]
        series_top.append(
            (item.symbol.replace("KQ.i@", ""), [values_by_date[d] / base for d in common_dates], color)
        )
    svg_line_chart(
        charts / "top_equity.svg",
        "Top 8 products: normalized equity (common dates)",
        series_top,
        common_dates,
        "Normalized equity",
    )
    all_trade_returns = [trade.return_pct for item in results for trade in item.trade_details]
    svg_histogram(charts / "trade_histogram.svg", "Trade return distribution", all_trade_returns)
    long_pnls = [trade.pnl for item in results for trade in item.trade_details if trade.side == "long"]
    short_pnls = [trade.pnl for item in results for trade in item.trade_details if trade.side == "short"]
    svg_horizontal_bars(
        charts / "long_short.svg",
        "Average PnL by side",
        [
            (f"long ({len(long_pnls)})", statistics.fmean(long_pnls) if long_pnls else 0.0),
            (f"short ({len(short_pnls)})", statistics.fmean(short_pnls) if short_pnls else 0.0),
        ],
        "#3fb950",
        "#f85149",
        width=800,
    )
    charts_map = {
        "portfolio": chart_ref("portfolio_equity"),
        "drawdown": chart_ref("portfolio_drawdown"),
        "yearly": chart_ref("yearly_portfolio"),
        "ranking": chart_ref("return_ranking"),
        "scatter": chart_ref("risk_return_scatter"),
        "heatmap": chart_ref("yearly_heatmap"),
        "top_equity": chart_ref("top_equity"),
        "histogram": chart_ref("trade_histogram"),
        "long_short": chart_ref("long_short"),
        "leverage": chart_ref("leverage_ranking"),
    }

    # Include drawdown image in the report right after the equity image.
    report_text_path = Path(args.report)
    write_report(
        report_text_path,
        results,
        excluded,
        params,
        portfolio,
        first_half,
        second_half,
        charts_map,
        specs,
        notional_portfolio,
        params.sizing,
    )
    text = report_text_path.read_text()
    anchor = f"![组合权益与回撤]({charts_map['portfolio']})"
    text = text.replace(
        anchor,
        anchor + "\n\n" + f"![组合回撤]({charts_map['drawdown']})",
    )
    report_text_path.write_text(text)

    write_csv(output / "results.csv", sorted_results)
    with (output / "trades.csv").open("w", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["symbol", "side", "entry_date", "exit_date", "entry_price", "exit_price", "lots", "multiplier", "margin_rate", "pnl", "return_pct", "holding_days", "reason"])
        for result in sorted_results:
            for trade in result.trade_details:
                writer.writerow([
                    result.symbol, trade.side, trade.entry_date, trade.exit_date,
                    trade.entry_price, trade.exit_price, trade.lots, trade.multiplier,
                    trade.margin_rate, trade.pnl, trade.return_pct, trade.holding_days,
                    trade.reason,
                ])
    with (output / "yearly_returns.csv").open("w", newline="") as handle:
        years = sorted({year for result in sorted_results for year in result.yearly})
        writer = csv.writer(handle)
        writer.writerow(["symbol"] + [str(year) for year in years])
        for result in sorted_results:
            writer.writerow([result.symbol] + [result.yearly.get(year, "") for year in years])
    with (output / "equity_curves.csv").open("w", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["symbol", "date", "equity", "drawdown_pct"])
        for result in sorted_results:
            for date_text, value, dd in zip(result.equity_dates, result.equity, result.drawdowns):
                writer.writerow([result.symbol, date_text, value, dd])
    with (output / "portfolio_equity.csv").open("w", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["date", "equity", "drawdown_pct"])
        for date_text, value, dd in zip(portfolio["equity_dates"], portfolio["equity"], daily_drawdowns(portfolio["equity"])):
            writer.writerow([date_text, value, dd])
    (output / "results.json").write_text(
        json.dumps(
            {
                "data_source": "tqsdk-backtest-md/KQ.i@daily",
                "params": {
                    "ema_period": params.ema_period,
                    "donchian_period": params.donchian_period,
                    "atr_period": params.atr_period,
                    "stop_atr": params.stop_atr,
                    "allocation": params.allocation,
                    "commission_rate": params.commission_rate,
                    "sizing": params.sizing,
                    "maintenance_ratio": params.maintenance_ratio,
                    "spec_file": str(args.spec_file),
                },
                "portfolio": {k: v for k, v in portfolio.items() if k not in ("equity_dates", "equity")},
                "notional_portfolio": {k: v for k, v in notional_portfolio.items() if k not in ("equity_dates", "equity")},
                "first_half": first_half,
                "second_half": second_half,
                "excluded": excluded,
                "results": [
                    {k: v for k, v in result.__dict__.items() if k not in ("equity_dates", "equity", "drawdowns", "trade_details")}
                    for result in sorted_results
                ],
            },
            ensure_ascii=False,
            indent=2,
            default=str,
        )
    )
    print(
        f"included={len(results)} excluded={len(excluded)} trades={sum(r.trades for r in results)} "
        f"return={portfolio['total_return_pct']:+.2f}% drawdown={portfolio['max_drawdown_pct']:.2f}% "
        f"report={report_text_path}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

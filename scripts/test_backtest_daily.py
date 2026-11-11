#!/usr/bin/env python3
"""Small deterministic tests for the daily backtest primitives."""

from __future__ import annotations

import math
import unittest
from datetime import date, timedelta

import backtest_daily as bd


def make_bars(count: int = 100) -> list[bd.Bar]:
    bars: list[bd.Bar] = []
    start = date(2020, 1, 1)
    price = 100.0
    for i in range(count):
        price += 0.5 + (0.2 if i % 5 == 0 else 0.0)
        bars.append(
            bd.Bar(
                dt=i,
                date=start + timedelta(days=i),
                open=price - 0.2,
                high=price + 0.1,
                low=price - 1.0,
                close=price,
                volume=10_000,
            )
        )
    return bars


class BacktestDailyTest(unittest.TestCase):
    def test_indicators(self) -> None:
        bars = make_bars(30)
        closes = [bar.close for bar in bars]
        self.assertEqual(len(bd.ema(closes, 10)), len(closes))
        upper, lower = bd.donchian(bars, 5)
        self.assertTrue(math.isnan(upper[3]))
        self.assertGreaterEqual(upper[10], lower[10])
        self.assertTrue(math.isfinite(bd.atr(bars, 5)[10]))

    def test_backtest_is_finite_and_deterministic(self) -> None:
        params = bd.Params(
            ema_period=10,
            donchian_period=5,
            atr_period=5,
            stop_atr=2.0,
            allocation=0.30,
            initial_equity=1_000_000,
            commission_rate=0.0001,
        )
        bars = make_bars(120)
        spec = bd.Spec("KQ.i@TEST", multiplier=10.0, margin_rate=0.10, commission_per_lot=1.0)
        first = bd.backtest("TEST", "KQ.i@TEST", bars, params, spec)
        second = bd.backtest("TEST", "KQ.i@TEST", bars, params, spec)
        self.assertEqual(first.trades, second.trades)
        self.assertAlmostEqual(first.total_return_pct, second.total_return_pct)
        self.assertTrue(math.isfinite(first.total_return_pct))
        self.assertLessEqual(first.max_drawdown_pct, 0.0)
        self.assertGreaterEqual(first.buy_hold_pct, 0.0)

    def test_contract_multiplier_changes_pnl(self) -> None:
        params = bd.Params(ema_period=10, donchian_period=5, atr_period=5, sizing="margin")
        bars = make_bars(120)
        low = bd.backtest(
            "TEST", "KQ.i@TEST", bars, params,
            bd.Spec("KQ.i@TEST", multiplier=1.0, margin_rate=0.10, commission_per_lot=0.0),
        )
        high = bd.backtest(
            "TEST", "KQ.i@TEST", bars, params,
            bd.Spec("KQ.i@TEST", multiplier=10.0, margin_rate=0.10, commission_per_lot=0.0),
        )
        self.assertNotEqual(low.final_equity, high.final_equity)
        self.assertNotEqual(low.multiplier, high.multiplier)

    def test_drawdown_and_yearly(self) -> None:
        equity = [100, 110, 90, 120]
        dates = ["2020-01-01", "2020-12-31", "2021-12-31", "2022-12-31"]
        self.assertAlmostEqual(bd.max_drawdown(equity), -18.181818, places=5)
        yearly = bd.yearly_returns(dates, equity)
        self.assertAlmostEqual(yearly[2020], 10.0)
        self.assertAlmostEqual(yearly[2021], -18.181818, places=5)


if __name__ == "__main__":
    unittest.main()

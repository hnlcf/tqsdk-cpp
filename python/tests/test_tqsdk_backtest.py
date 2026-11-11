"""ctypes binding tests. Run through ctest or with:

    PYTHONPATH=python TQSDK_BACKTEST_LIB=build/libtqsdk_backtest.so \
      python3 -m unittest discover -s python/tests -v
"""

from __future__ import annotations

import unittest

from tqsdk_backtest import Bar, BacktestConfig, BacktestEngine, run_backtest


def make_bars() -> list[Bar]:
    return [
        Bar(1, 100.0, 101.0, 99.0, 100.0, 10),
        Bar(2, 110.0, 111.0, 109.0, 110.0, 10),
        Bar(3, 120.0, 121.0, 119.0, 120.0, 10),
    ]


class PythonBacktestTest(unittest.TestCase):
    def test_long_round_trip(self) -> None:
        config = BacktestConfig(initial_balance=1_000_000, volume_multiple=10)
        result = run_backtest(
            make_bars(),
            lambda ctx, bar: (
                ctx.buy_open(2) if bar.datetime == 1
                else ctx.sell_close(2) if bar.datetime == 2
                else None
            ),
            config,
        )
        self.assertEqual(result.stats.bars, 3)
        self.assertEqual(result.stats.fills, 2)
        self.assertEqual(result.stats.final_position, 0)
        self.assertAlmostEqual(result.stats.realized_pnl, 200.0)
        self.assertAlmostEqual(result.stats.final_equity, 1_000_200.0)
        self.assertEqual(len(result.fills), 2)
        self.assertEqual(result.fills[0].direction, "BUY")
        self.assertEqual(result.fills[1].offset, "CLOSE")

    def test_target_position_and_commission(self) -> None:
        bars = [
            Bar(1, 100.0, 101.0, 99.0, 100.0, 10),
            Bar(2, 90.0, 91.0, 89.0, 90.0, 10),
        ]
        config = BacktestConfig(volume_multiple=10, commission_per_lot=1.0)
        result = run_backtest(
            bars,
            lambda ctx, bar: (
                ctx.target_position(-1) if bar.datetime == 1
                else ctx.target_position(0) if bar.datetime == 2
                else None
            ),
            config,
        )
        self.assertEqual(result.stats.final_position, 0)
        self.assertAlmostEqual(result.stats.realized_pnl, 100.0)
        self.assertAlmostEqual(result.stats.commission, 2.0)
        self.assertAlmostEqual(result.stats.final_equity, 1_000_098.0)

    def test_context_equity_and_position(self) -> None:
        seen: list[tuple[int, float]] = []
        config = BacktestConfig(volume_multiple=1, commission_rate=0.0)

        def strategy(ctx, bar) -> None:
            seen.append((ctx.position(), ctx.equity()))
            if bar.datetime == 1:
                ctx.buy_open(1)

        with BacktestEngine(config) as engine:
            result = engine.run(make_bars(), strategy)
        self.assertEqual(result.stats.final_position, 1)
        self.assertGreater(result.equity_curve[-1][1], 1_000_000)
        self.assertEqual(seen[0][0], 0)
        self.assertEqual(seen[1][0], 1)

    def test_python_exception_is_propagated(self) -> None:
        with self.assertRaises(RuntimeError):
            run_backtest(
                make_bars(),
                lambda ctx, bar: (_ for _ in ()).throw(ValueError("strategy bug")),
                BacktestConfig(volume_multiple=1),
            )

    def test_input_dict(self) -> None:
        config = BacktestConfig(volume_multiple=1)
        bars = [
            {"datetime": 1, "open": 100, "high": 101, "low": 99, "close": 100, "volume": 1},
            {"datetime": 2, "open": 100, "high": 101, "low": 99, "close": 100, "volume": 1},
        ]
        result = run_backtest(bars, lambda ctx, bar: None, config)
        self.assertEqual(result.stats.bars, 2)


if __name__ == "__main__":
    unittest.main()

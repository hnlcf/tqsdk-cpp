#!/usr/bin/env python3
"""Small example: Python strategy + C++ BacktestEngine.

Build the binding first:
    cmake --build build --target tqsdk_backtest

Run:
    PYTHONPATH=python TQSDK_BACKTEST_LIB=build/libtqsdk_backtest.so \
      python3 python/examples/ema_backtest.py
"""

from __future__ import annotations

from tqsdk_backtest import BacktestConfig, Bar, run_backtest, lots_for_margin


def main() -> None:
    config = BacktestConfig(
        initial_balance=1_000_000,
        volume_multiple=10,
        margin_rate=0.10,
        commission_per_lot=1.0,
    )
    # Synthetic daily bars; replace with Tq daily JSON or your own dataframe.
    bars = [
        Bar(i, 100 + i, 102 + i, 99 + i, 101 + i, 10_000)
        for i in range(1, 40)
    ]

    def strategy(ctx, bar: Bar) -> None:
        # Toy strategy: hold +1 after bar 5, flat after bar 30.
        if bar.datetime == 5:
            lots = lots_for_margin(ctx.equity(), bar.close, config, allocation=0.30)
            ctx.target_position(lots)
        elif bar.datetime == 30:
            ctx.target_position(0)

    result = run_backtest(bars, strategy, config)
    print(result.stats)
    print(f"fills={len(result.fills)} equity_points={len(result.equity_curve)}")
    if result.equity_curve:
        print(f"equity: {result.equity_curve[0][1]:.2f} -> {result.equity_curve[-1][1]:.2f}")


if __name__ == "__main__":
    main()

"""Tests for the shared EMA strategy and live runner wiring."""

from __future__ import annotations

import unittest

from tqsdk_live import LiveRunner, NodeClient, closed_bars, entry_price, net_position
from tqsdk_strategy import (
    Bar,
    ContractSpec,
    EmaDonchianStrategy,
    StrategyConfig,
    resolve_spec,
)


def breakout_bars() -> list[Bar]:
    bars = [Bar(i, 99.5, 101.0, 99.0, 100.0, 10_000) for i in range(100)]
    bars.append(Bar(100, 100.0, 130.0, 100.0, 125.0, 10_000))
    return bars


class FakeNode:
    def __init__(self, target_hits: list[tuple[str, int]]):
        self.target_hits = target_hits
        self.position = 0
        self.executed = 0

    def readyz(self):
        return 200, {"ready": True, "td": {"phase": "trading"}}

    def kline(self, symbol, duration_ns, width=200):
        return breakout_bars()

    def positions(self):
        if self.position == 0:
            return {"positions": {}}
        return {
            "positions": {
                "X": {
                    "volume_long": max(0, self.position),
                    "volume_short": max(0, -self.position),
                    "open_price_long": 125.0 if self.position > 0 else None,
                    "open_price_short": 125.0 if self.position < 0 else None,
                }
            }
        }

    def account(self):
        return {"balance": 1_000_000.0}

    def quote(self, symbol):
        return {"last_price": 125.0}

    def target_position(self, symbol, target_volume, mode="ACTIVE", max_volume=0):
        self.target_hits.append((symbol, target_volume))
        self.executed += 1
        return {"success": True, "task_id": f"target-{self.executed}"}


class StrategyLiveTest(unittest.TestCase):
    def setUp(self) -> None:
        self.spec = ContractSpec("X", volume_multiple=10, margin_rate=0.10)
        self.config = StrategyConfig(
            symbol="X",
            duration_ns=86_400_000_000_000,
            allocation=0.30,
        )

    def test_breakout_and_stop(self) -> None:
        strategy = EmaDonchianStrategy(self.config, self.spec)
        decision = strategy.decide(breakout_bars(), 0, 1_000_000)
        self.assertEqual(decision.signal, "long")
        self.assertGreater(decision.target, 0)
        self.assertIsNotNone(strategy.stop_price)
        stopped = strategy.decide(
            breakout_bars(),
            decision.target,
            1_000_000,
            price=1.0,
        )
        self.assertEqual(stopped.target, 0)
        self.assertEqual(stopped.reason, "atr_stop")
        self.assertIsNone(strategy.stop_price)

    def test_closed_bar_filter(self) -> None:
        now = 100 + self.config.duration_ns // 2
        bars = breakout_bars()
        self.assertEqual(len(closed_bars(bars, self.config.duration_ns, now)), len(bars) - 1)
        future = breakout_bars()
        future[-1].datetime = now - 2 * self.config.duration_ns
        self.assertEqual(len(closed_bars(future, self.config.duration_ns, now)), len(future))

    def test_resolve_actual_contract_spec(self) -> None:
        specs = {"KQ.i@SHFE.rb": self.spec}
        self.assertIs(resolve_spec(specs, "SHFE.rb2701"), self.spec)
        self.assertIsNone(resolve_spec(specs, "DCE.jm2701"))

    def test_position_helpers(self) -> None:
        self.assertEqual(net_position({"volume_long": 2, "volume_short": 5}), -3)
        self.assertEqual(
            entry_price({"volume_long": 1, "open_price_long": 99.5}),
            99.5,
        )

    def test_runner_dry_run_does_not_submit(self) -> None:
        hits: list[tuple[str, int]] = []
        fake = FakeNode(hits)
        strategy = EmaDonchianStrategy(self.config, self.spec)
        runner = LiveRunner(
            fake, strategy, self.config, self.spec, execute=False, max_cycles=1,
            logger=lambda _line: None,
        )
        stats = runner.run()
        self.assertEqual(stats.cycles, 1)
        self.assertEqual(hits, [])
        self.assertGreater(stats.decisions, 0)

    def test_runner_execute_submits_target(self) -> None:
        hits: list[tuple[str, int]] = []
        fake = FakeNode(hits)
        strategy = EmaDonchianStrategy(self.config, self.spec)
        runner = LiveRunner(
            fake, strategy, self.config, self.spec, execute=True, max_cycles=1,
            logger=lambda _line: None,
        )
        stats = runner.run()
        self.assertEqual(stats.cycles, 1)
        self.assertEqual(len(hits), 1)
        self.assertEqual(hits[0][0], "X")
        self.assertGreater(hits[0][1], 0)
        self.assertEqual(stats.submitted, 1)


class NodeClientUrlTest(unittest.TestCase):
    def test_account_and_token_query_construction(self) -> None:
        client = NodeClient(
            base_url="http://127.0.0.1:18081",
            account="sim",
            token="secret-token",
        )
        self.assertEqual(
            client._url("/account"),
            "http://127.0.0.1:18081/account?account=sim",
        )
        self.assertEqual(
            client._url("/kline/X/1?width=200"),
            "http://127.0.0.1:18081/kline/X/1?width=200&account=sim",
        )


if __name__ == "__main__":
    unittest.main()

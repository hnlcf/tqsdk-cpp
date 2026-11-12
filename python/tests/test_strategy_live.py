"""Tests for the reusable strategy/signal/live abstraction layer."""

from __future__ import annotations

import json
import unittest
from unittest.mock import patch

from tqsdk_py import (
    BacktestRunner,
    BacktestSettings,
    Bar,
    CompositeConsumer,
    ContractSpec,
    DryRunExecutor,
    EmaDonchianStrategy,
    ExecutionResult,
    InMemoryBarSource,
    LiveConfig,
    LiveRunner,
    MemoryConsumer,
    Signal,
    SignalConsumer,
    SignalDispatcher,
    StrategyConfig,
    StrategyContext,
    TargetExecutor,
    WeComConsumer,
    net_position,
    entry_price,
    resolve_spec,
)


def breakout_bars() -> list[Bar]:
    bars = [Bar(i, 99.5, 101.0, 99.0, 100.0, 10_000) for i in range(100)]
    bars.append(Bar(100, 100.0, 130.0, 100.0, 125.0, 10_000))
    return bars


class FakeNode:
    def readyz(self):
        return 200, {"ready": True, "td": {"phase": "trading"}}

    def positions(self):
        return {"positions": {}}

    def account(self):
        return {"balance": 1_000_000.0}

    def quote(self, symbol):
        return {"last_price": 125.0}


class RecordingExecutor(TargetExecutor):
    def __init__(self):
        self.executed: list[Signal] = []

    def execute(self, signal: Signal, context=None) -> ExecutionResult:
        self.executed.append(signal)
        return ExecutionResult(accepted=True, target_volume=signal.target_volume, task_id="t1")


class FailingConsumer(SignalConsumer):
    def consume(self, signal: Signal, context=None) -> None:
        raise RuntimeError("notification down")


class StrategySignalTest(unittest.TestCase):
    def setUp(self) -> None:
        self.spec = ContractSpec("X", volume_multiple=10, margin_rate=0.10)
        self.config = StrategyConfig(symbol="X", allocation=0.30)

    def test_strategy_emits_actionable_signal(self) -> None:
        strategy = EmaDonchianStrategy(self.config, self.spec)
        context = StrategyContext(
            symbol="X",
            bars=tuple(breakout_bars()),
            position=0,
            equity=1_000_000,
            spec=self.spec,
            price=125.0,
        )
        signal = strategy.on_bar(context)
        self.assertIsNotNone(signal)
        assert signal is not None
        self.assertEqual(signal.signal, "long")
        self.assertGreater(signal.target_volume, 0)
        self.assertIsNotNone(signal.stop_price)
        strategy.ack(signal)
        self.assertIsNone(strategy.on_bar(context))

    def test_stop_emits_flat(self) -> None:
        strategy = EmaDonchianStrategy(self.config, self.spec)
        bars = tuple(breakout_bars())
        signal = strategy.on_bar(
            StrategyContext("X", bars, 0, 1_000_000, self.spec, price=125.0)
        )
        assert signal is not None
        strategy.ack(signal)
        stopped = strategy.on_bar(
            StrategyContext(
                "X", bars, signal.target_volume, 1_000_000, self.spec, price=1.0
            )
        )
        self.assertIsNotNone(stopped)
        assert stopped is not None
        self.assertEqual(stopped.target_volume, 0)
        self.assertEqual(stopped.reason, "atr_stop")

    def test_resolve_actual_contract(self) -> None:
        specs = {"KQ.i@SHFE.rb": self.spec}
        self.assertIs(resolve_spec(specs, "SHFE.rb2701"), self.spec)
        self.assertIsNone(resolve_spec(specs, "DCE.jm2701"))

    def test_position_helpers(self) -> None:
        self.assertEqual(net_position({"volume_long": 2, "volume_short": 5}), -3)
        self.assertEqual(entry_price({"volume_long": 1, "open_price_long": 99.5}), 99.5)


class SignalConsumerTest(unittest.TestCase):
    def test_composite_continues_after_notification_failure(self) -> None:
        memory = MemoryConsumer()
        composite = CompositeConsumer([FailingConsumer(), memory])
        signal = Signal("X", 1, "breakout", "long")
        composite.consume(signal)
        self.assertEqual(len(memory.signals), 1)
        self.assertEqual(len(composite.errors), 1)

    def test_dispatcher_notifies_then_executes(self) -> None:
        memory = MemoryConsumer()
        executor = RecordingExecutor()
        dispatcher = SignalDispatcher([memory], executor)
        signal = Signal("X", 2, "breakout", "long")
        result = dispatcher.dispatch(signal)
        self.assertEqual(len(memory.signals), 1)
        self.assertEqual(len(executor.executed), 1)
        self.assertTrue(result.execution.accepted)

    def test_wecom_payload(self) -> None:
        class Response:
            def __enter__(self):
                return self

            def __exit__(self, *args):
                return False

            def read(self):
                return b'{"errcode":0,"errmsg":"ok"}'

        signal = Signal("X", 1, "breakout", "long", stop_price=90.0, strategy_id="ema")
        with patch("tqsdk_py.signals.urllib.request.urlopen", return_value=Response()) as mocked:
            WeComConsumer("https://qyapi.weixin.qq.com/cgi-bin/webhook/send?key=test").consume(
                signal
            )
        request = mocked.call_args[0][0]
        body = json.loads(request.data)
        self.assertEqual(body["msgtype"], "markdown")
        self.assertIn("X", body["markdown"]["content"])
        self.assertIn("long", body["markdown"]["content"])


class LiveRunnerTest(unittest.TestCase):
    def setUp(self) -> None:
        self.spec = ContractSpec("X", volume_multiple=10, margin_rate=0.10)
        self.config = StrategyConfig(symbol="X", allocation=0.30)
        self.source = InMemoryBarSource({"X": breakout_bars()})

    def test_dry_run_notifies_without_order(self) -> None:
        memory = MemoryConsumer()
        strategy = EmaDonchianStrategy(self.config, self.spec)
        dispatcher = SignalDispatcher([memory], DryRunExecutor(logger=lambda _: None))
        runner = LiveRunner(
            FakeNode(),
            self.source,
            strategy,
            self.spec,
            dispatcher,
            LiveConfig(symbol="X", max_cycles=1),
            logger=lambda _: None,
        )
        stats = runner.run()
        self.assertEqual(stats.cycles, 1)
        self.assertEqual(stats.signals, 1)
        self.assertEqual(len(memory.signals), 1)
        self.assertEqual(stats.submitted, 0)

    def test_execute_path_submits_target(self) -> None:
        memory = MemoryConsumer()
        executor = RecordingExecutor()
        strategy = EmaDonchianStrategy(self.config, self.spec)
        runner = LiveRunner(
            FakeNode(),
            self.source,
            strategy,
            self.spec,
            SignalDispatcher([memory], executor),
            LiveConfig(symbol="X", max_cycles=1),
            logger=lambda _: None,
        )
        stats = runner.run()
        self.assertEqual(stats.signals, 1)
        self.assertEqual(len(executor.executed), 1)
        self.assertEqual(executor.executed[0].target_volume, memory.signals[0].target_volume)


class BacktestAdapterTest(unittest.TestCase):
    def test_cpp_backtest_runner_consumes_signal(self) -> None:
        spec = ContractSpec("X", volume_multiple=10, margin_rate=0.10)
        source = InMemoryBarSource({"X": breakout_bars()})
        memory = MemoryConsumer()
        runner = BacktestRunner(
            source,
            EmaDonchianStrategy(StrategyConfig(symbol="X", allocation=0.30), spec),
            spec,
            BacktestSettings(initial_balance=1_000_000, commission_rate=0.0),
            notifiers=[memory],
        )
        result = runner.run("X")
        self.assertGreaterEqual(result.bars, 101)
        self.assertGreaterEqual(len(result.signals), 1)
        self.assertGreaterEqual(len(memory.signals), 1)
        self.assertIsNotNone(result.stats)
        self.assertGreaterEqual(result.stats.fills, 1)


if __name__ == "__main__":
    unittest.main()

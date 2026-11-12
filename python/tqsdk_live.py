"""CLI and compatibility entry point for the reusable `tqsdk_py` live runner.

Topology:

    CTP -> tqsdk-gw -> tqsdk-node (REST) -> tqsdk_py LiveRunner

The library layer is `tqsdk_py`; this module keeps the previous CLI/import
surface available.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path

from tqsdk_py.data import NodeBarSource, closed_bars, load_specs, resolve_spec
from tqsdk_py.execution import DryRunExecutor, NodeTargetExecutor
from tqsdk_py.live import LiveConfig, LiveRunner, LiveStats
from tqsdk_py.node import NodeClient, NodeError, entry_price, net_position
from tqsdk_py.signals import (
    CompositeConsumer,
    LoggingConsumer,
    SignalDispatcher,
    WeComConsumer,
)
from tqsdk_py.strategy import EmaDonchianStrategy, StrategyConfig

__all__ = [
    "NodeClient",
    "NodeError",
    "LiveConfig",
    "LiveRunner",
    "LiveStats",
    "closed_bars",
    "entry_price",
    "net_position",
    "build_runner",
    "main",
]


def build_runner(args: argparse.Namespace) -> LiveRunner:
    specs = load_specs(Path(args.spec_file))
    spec = resolve_spec(specs, args.symbol)
    if spec is None:
        raise SystemExit(f"no spec for {args.symbol} in {args.spec_file}")
    strategy_config = StrategyConfig(
        symbol=args.symbol,
        duration_ns=args.duration_ns,
        ema_period=args.ema,
        donchian_period=args.donchian,
        atr_period=args.atr,
        stop_atr=args.stop_atr,
        allocation=args.allocation,
        max_volume=args.max_volume,
    )
    client = NodeClient(
        base_url=args.node_url,
        account=args.account,
        token=args.token,
    )
    notifiers = [LoggingConsumer(logger=print)]
    webhook = os.environ.get(args.wecom_webhook_env, "")
    if webhook:
        notifiers.append(WeComConsumer(webhook))
    executor = (
        NodeTargetExecutor(client, max_volume=args.max_volume)
        if args.execute
        else DryRunExecutor(logger=print)
    )
    dispatcher = SignalDispatcher(notifiers=notifiers, executor=executor)
    strategy = EmaDonchianStrategy(strategy_config, spec)
    return LiveRunner(
        node=client,
        data_source=NodeBarSource(client, width=args.width),
        strategy=strategy,
        spec=spec,
        dispatcher=dispatcher,
        config=LiveConfig(
            symbol=args.symbol,
            duration_ns=args.duration_ns,
            poll_interval=args.poll_interval,
            width=args.width,
            max_cycles=args.max_cycles,
        ),
        logger=print,
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Run EMA60/Donchian22/ATR against tqsdk-node")
    parser.add_argument("--node-url", default="http://127.0.0.1:18081")
    parser.add_argument("--account", default="default")
    parser.add_argument("--token", default=os.environ.get("TQSDK_SERVE_TOKEN", ""))
    parser.add_argument("--symbol", default="SHFE.rb2701")
    parser.add_argument("--duration-ns", type=int, default=86_400_000_000_000)
    parser.add_argument("--spec-file", default="config/daily_backtest_specs.json")
    parser.add_argument("--ema", type=int, default=60)
    parser.add_argument("--donchian", type=int, default=22)
    parser.add_argument("--atr", type=int, default=14)
    parser.add_argument("--stop-atr", type=float, default=2.0)
    parser.add_argument("--allocation", type=float, default=0.30)
    parser.add_argument("--max-volume", type=int, default=0)
    parser.add_argument("--poll-interval", type=float, default=5.0)
    parser.add_argument("--width", type=int, default=200)
    parser.add_argument("--max-cycles", type=int, default=0)
    parser.add_argument("--execute", action="store_true", help="actually submit target tasks")
    parser.add_argument(
        "--wecom-webhook-env",
        default="TQSDK_WECOM_WEBHOOK",
        help="environment variable containing the WeCom robot webhook URL",
    )
    args = parser.parse_args(argv)
    args.max_cycles = None if args.max_cycles <= 0 else args.max_cycles
    runner = build_runner(args)
    stats = runner.run()
    print(json.dumps(stats.__dict__, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

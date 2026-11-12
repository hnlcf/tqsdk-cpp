"""Generic backtest/live CLIs with runtime strategy selection.

The CLI knows no concrete strategy. It loads the strategy manager, resolves
`--strategy <name>`, builds it with `--param key=value`, and passes the same
Strategy interface to BacktestRunner or LiveRunner.
"""

from __future__ import annotations

import argparse
import ast
import json
import os
from dataclasses import asdict
from pathlib import Path
from typing import Any

from .backtest import BacktestRunner, BacktestSettings
from .data import JsonBarSource, NodeBarSource, load_specs, resolve_spec
from .execution import DryRunExecutor, NodeTargetExecutor
from .live import LiveConfig, LiveRunner
from .manager import StrategyManager, default_manager
from .registry import UnknownStrategyError
from .node import NodeClient
from .signals import LoggingConsumer, SignalDispatcher, WeComConsumer


def load_env_file(path: str | Path = ".env") -> str:
    """Load simple KEY=VALUE entries without overriding the process env.

    This is intentionally dependency-free. It is used for local notification
    settings such as TQSDK_WECOM_WEBHOOK; secrets are never printed.
    """
    env_path = Path(path)
    if not env_path.is_file():
        return ""
    for raw in env_path.read_text().splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("export "):
            line = line[7:].strip()
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        key = key.strip()
        if not key or key in os.environ:
            continue
        value = value.strip()
        if len(value) >= 2 and value[0] == value[-1] and value[0] in ("'", '"'):
            value = value[1:-1]
        os.environ[key] = value
    return str(env_path)


def add_strategy_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--strategy",
        default=None,
        help="registered strategy name/alias (required for a run)",
    )
    parser.add_argument(
        "--param",
        "--strategy-param",
        dest="params",
        action="append",
        default=[],
        metavar="KEY=VALUE",
        help="strategy parameter; repeatable",
    )
    parser.add_argument("--list-strategies", action="store_true")


def parse_params(items: list[str]) -> dict[str, Any]:
    params: dict[str, Any] = {}
    for item in items:
        if "=" not in item:
            raise SystemExit(f"invalid --param '{item}', expected KEY=VALUE")
        key, raw = item.split("=", 1)
        key = key.strip()
        if not key:
            raise SystemExit(f"invalid --param '{item}', empty key")
        if raw.strip().lower() in {"true", "false"}:
            value = raw.strip().lower() == "true"
        elif raw.strip().lower() in {"none", "null"}:
            value = None
        else:
            try:
                value = ast.literal_eval(raw)
            except (SyntaxError, ValueError):
                value = raw
        params[key] = value
    return params


def print_strategies(manager: StrategyManager) -> None:
    for definition in manager.definitions():
        print(
            json.dumps(
                {
                    "name": definition.name,
                    "aliases": list(definition.aliases),
                    "defaults": definition.defaults,
                    "description": definition.description,
                    "class": definition.strategy_class.__name__,
                },
                ensure_ascii=False,
                sort_keys=True,
            )
        )


def load_spec(path: str, symbol: str) -> Any:
    specs = load_specs(Path(path))
    spec = resolve_spec(specs, symbol)
    if spec is None:
        raise SystemExit(f"no contract spec for {symbol} in {path}")
    return spec


def require_strategy(args: argparse.Namespace) -> str:
    if not args.strategy:
        raise SystemExit(
            "--strategy is required; run --list-strategies to see registered names"
        )
    return str(args.strategy)


def build_strategy(
    args: argparse.Namespace,
    manager: StrategyManager | None = None,
) -> Any:
    manager = manager or default_manager()
    name = require_strategy(args)
    spec = load_spec(args.spec_file, args.symbol)
    return manager.create(
        name,
        spec=spec,
        symbol=args.symbol,
        duration_ns=args.duration_ns,
        params=parse_params(args.params),
    )


def _common_market_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--symbol", default="SHFE.rb2701")
    parser.add_argument("--duration-ns", type=int, default=86_400_000_000_000)
    parser.add_argument("--spec-file", default="config/daily_backtest_specs.json")


def build_live_runner(args: argparse.Namespace) -> LiveRunner:
    manager = default_manager()
    if args.list_strategies:
        raise SystemExit(0)
    name = require_strategy(args)
    spec = load_spec(args.spec_file, args.symbol)
    strategy = manager.create(
        name,
        spec=spec,
        symbol=args.symbol,
        duration_ns=args.duration_ns,
        params=parse_params(args.params),
    )
    client = NodeClient(base_url=args.node_url, account=args.account, token=args.token)
    notifiers = [LoggingConsumer(logger=print)]
    webhook = os.environ.get(args.wecom_webhook_env, "")
    if webhook:
        notifiers.append(WeComConsumer(webhook))
    executor = (
        NodeTargetExecutor(client, max_volume=int(args.max_volume))
        if args.execute
        else DryRunExecutor(logger=print)
    )
    dispatcher = SignalDispatcher(notifiers=notifiers, executor=executor)
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


def list_strategies_main(argv: list[str] | None = None) -> int:
    """Console entry point for registered strategy discovery."""
    print_strategies(default_manager())
    return 0


def live_main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Run a registered strategy against tqsdk-node")
    add_strategy_arguments(parser)
    _common_market_args(parser)
    parser.add_argument("--node-url", default="http://127.0.0.1:18081")
    parser.add_argument("--account", default="default")
    parser.add_argument("--token", default=os.environ.get("TQSDK_SERVE_TOKEN", ""))
    parser.add_argument("--max-volume", type=int, default=0)
    parser.add_argument("--env-file", default=".env", help="optional KEY=VALUE env file")
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
    load_env_file(args.env_file)
    args.max_cycles = None if args.max_cycles <= 0 else args.max_cycles
    manager = default_manager()
    if args.list_strategies:
        print_strategies(manager)
        return 0
    try:
        runner = build_live_runner(args)
    except UnknownStrategyError as exc:
        parser.error(str(exc))
    stats = runner.run()
    print(json.dumps(stats.__dict__, ensure_ascii=False))
    return 0


def build_backtest_runner(args: argparse.Namespace) -> BacktestRunner:
    manager = default_manager()
    name = require_strategy(args)
    spec = load_spec(args.spec_file, args.symbol)
    strategy = manager.create(
        name,
        spec=spec,
        symbol=args.symbol,
        duration_ns=args.duration_ns,
        params=parse_params(args.params),
    )
    source = JsonBarSource(args.data_file or args.data_dir)
    return BacktestRunner(
        source,
        strategy,
        spec,
        BacktestSettings(
            initial_balance=args.initial_balance,
            commission_rate=args.commission_rate,
            margin_rate_override=args.margin_rate,
            execute_on_bar_close=True,
        ),
        notifiers=[LoggingConsumer(logger=print)],
        width=args.width,
    )


def backtest_main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Run a registered strategy through C++ BacktestEngine")
    add_strategy_arguments(parser)
    _common_market_args(parser)
    parser.add_argument("--data-dir", default="artifacts/tq_daily")
    parser.add_argument("--data-file", default="")
    parser.add_argument("--width", type=int, default=2000)
    parser.add_argument("--initial-balance", type=float, default=1_000_000.0)
    parser.add_argument("--commission-rate", type=float, default=0.0)
    parser.add_argument("--margin-rate", type=float, default=None)
    parser.add_argument("--json-output", default="")
    args = parser.parse_args(argv)
    manager = default_manager()
    if args.list_strategies:
        print_strategies(manager)
        return 0
    try:
        runner = build_backtest_runner(args)
    except UnknownStrategyError as exc:
        parser.error(str(exc))
    result = runner.run(args.symbol, args.duration_ns, width=args.width)
    payload = {
        "strategy": args.strategy,
        "symbol": args.symbol,
        "bars": result.bars,
        "signals": len(result.signals),
        "fills": len(result.fills),
        "stats": asdict(result.stats) if result.stats is not None else None,
    }
    text = json.dumps(payload, ensure_ascii=False, indent=2, default=str)
    if args.json_output:
        Path(args.json_output).write_text(text + "\n")
    print(text)
    return 0

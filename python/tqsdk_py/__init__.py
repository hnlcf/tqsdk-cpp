"""Reusable Python strategy library for tqsdk-cpp.

The package separates:
  * models/data adapters;
  * strategy interfaces and implementations;
  * signal consumers/notifiers and execution adapters;
  * backtest and live runners.

The flat modules `tqsdk_strategy`, `tqsdk_live` and `tqsdk_backtest` remain
compatibility entry points.
"""

from .models import Bar, ContractSpec, Signal, StrategyContext
from .data import (
    BarSource,
    InMemoryBarSource,
    JsonBarSource,
    NodeBarSource,
    closed_bars,
    load_specs,
    resolve_spec,
)
from .strategy import Decision, EmaDonchianStrategy, Strategy, StrategyConfig
from .signals import (
    CompositeConsumer,
    HttpWebhookConsumer,
    LoggingConsumer,
    MemoryConsumer,
    NullConsumer,
    SignalConsumer,
    SignalDispatcher,
    WeComConsumer,
    OrderExecutorConsumer,
    DispatchResult,
    ConsumerError,
)
from .execution import (
    DryRunExecutor,
    ExecutionResult,
    NodeTargetExecutor,
    TargetExecutor,
)
from .backtest import BacktestRunResult, BacktestRunner, BacktestSettings
from .live import LiveConfig, LiveRunner, LiveStats
from .node import NodeClient, NodeError, entry_price, net_position

__all__ = [
    "Bar",
    "ContractSpec",
    "Signal",
    "StrategyContext",
    "BarSource",
    "JsonBarSource",
    "InMemoryBarSource",
    "NodeBarSource",
    "closed_bars",
    "load_specs",
    "resolve_spec",
    "Strategy",
    "StrategyConfig",
    "Decision",
    "EmaDonchianStrategy",
    "SignalConsumer",
    "NullConsumer",
    "LoggingConsumer",
    "MemoryConsumer",
    "WeComConsumer",
    "HttpWebhookConsumer",
    "CompositeConsumer",
    "SignalDispatcher",
    "OrderExecutorConsumer",
    "DispatchResult",
    "ConsumerError",
    "TargetExecutor",
    "DryRunExecutor",
    "NodeTargetExecutor",
    "ExecutionResult",
    "BacktestSettings",
    "BacktestRunResult",
    "BacktestRunner",
    "LiveConfig",
    "LiveRunner",
    "LiveStats",
    "NodeClient",
    "NodeError",
    "net_position",
    "entry_price",
]

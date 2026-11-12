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
    PandasBarSource,
    closed_bars,
    load_specs,
    resolve_spec,
)
from .strategy import Strategy
from .indicators import atr, donchian, ema, lots_for_margin
from .manager import StrategyManager, default_manager
from .registry import StrategyDefinition, register_strategy
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
    "PandasBarSource",
    "closed_bars",
    "load_specs",
    "resolve_spec",
    "Strategy",
    "StrategyManager",
    "StrategyDefinition",
    "default_manager",
    "register_strategy",
    "Decision",
    "EmaDonchianStrategy",
    "ema",
    "atr",
    "donchian",
    "lots_for_margin",
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


_LAZY_EXPORTS = {
    "EmaDonchianStrategy": "tqsdk_py.strategies.ema_donchian",
    "Decision": "tqsdk_py.strategies.ema_donchian",
}


def __getattr__(name: str):
    module = _LAZY_EXPORTS.get(name)
    if module:
        import importlib

        return getattr(importlib.import_module(module), name)
    raise AttributeError(name)

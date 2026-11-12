"""Compatibility entry point for the reusable `tqsdk_py` strategy library."""

from tqsdk_py.data import closed_bars, load_daily_json, load_specs, resolve_spec
from tqsdk_py.indicators import atr, donchian, ema, lots_for_margin
from tqsdk_py.manager import StrategyManager, default_manager
from tqsdk_py.models import Bar, ContractSpec, Signal, StrategyContext
from tqsdk_py.registry import StrategyDefinition, register_strategy
from tqsdk_py.strategies.ema_donchian import Decision, EmaDonchianStrategy
from tqsdk_py.strategy import Strategy

__all__ = [
    "Bar",
    "ContractSpec",
    "Signal",
    "StrategyContext",
    "Decision",
    "Strategy",
    "EmaDonchianStrategy",
    "StrategyManager",
    "StrategyDefinition",
    "default_manager",
    "register_strategy",
    "ema",
    "atr",
    "donchian",
    "lots_for_margin",
    "load_specs",
    "resolve_spec",
    "load_daily_json",
    "closed_bars",
]

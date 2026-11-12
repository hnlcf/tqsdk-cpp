"""Compatibility entry point for the reusable `tqsdk_py` strategy library."""

from tqsdk_py.data import closed_bars, load_daily_json, load_specs, resolve_spec
from tqsdk_py.models import Bar, ContractSpec, Signal, StrategyContext
from tqsdk_py.strategy import (
    Decision,
    EmaDonchianStrategy,
    Strategy,
    StrategyConfig,
    atr,
    donchian,
    ema,
    lots_for_margin,
)

__all__ = [
    "Bar",
    "ContractSpec",
    "Signal",
    "StrategyContext",
    "Decision",
    "Strategy",
    "StrategyConfig",
    "EmaDonchianStrategy",
    "ema",
    "atr",
    "donchian",
    "lots_for_margin",
    "load_specs",
    "resolve_spec",
    "load_daily_json",
    "closed_bars",
]

"""Compatibility entry point and CLI for the generic tqsdk_py live runner.

The live framework is strategy-agnostic. Select a registered strategy with:

    python3 python/tqsdk_live.py --strategy ema_donchian --list-strategies
    python3 python/tqsdk_live.py --strategy ema_donchian --param ema_period=60 ...
"""

from __future__ import annotations

from tqsdk_py.cli import build_live_runner as build_runner
from tqsdk_py.cli import live_main as main
from tqsdk_py.cli import print_strategies
from tqsdk_py.data import closed_bars
from tqsdk_py.live import LiveConfig, LiveRunner, LiveStats
from tqsdk_py.node import NodeClient, NodeError, entry_price, net_position

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
    "print_strategies",
    "main",
]


if __name__ == "__main__":
    raise SystemExit(main())

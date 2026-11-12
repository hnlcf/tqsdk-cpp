"""Strategy interface shared by backtest and live frameworks.

Concrete implementations live in `tqsdk_py.strategies/` and are discovered
through the strategy registry. The framework itself must not import or know a
specific strategy.
"""

from __future__ import annotations

from abc import ABC, abstractmethod

from .models import Signal, StrategyContext


class Strategy(ABC):
    """Minimal strategy contract.

    A strategy receives a context with the current closed-bar history and
    account state, and returns a desired net-position change or None.
    """

    @abstractmethod
    def on_bar(self, context: StrategyContext) -> Signal | None:
        raise NotImplementedError

    def reset(self) -> None:
        """Optional hook for starting a new backtest/live run."""

    def ack(self, signal: Signal) -> None:
        """Optional hook called after a signal was accepted/executed."""

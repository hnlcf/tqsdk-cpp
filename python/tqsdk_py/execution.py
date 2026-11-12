"""Execution adapters: turn a net-position Signal into a live order/task."""

from __future__ import annotations

from abc import ABC, abstractmethod
from dataclasses import dataclass, field
from typing import Any, Callable

from .models import Signal, StrategyContext


@dataclass
class ExecutionResult:
    accepted: bool
    target_volume: int
    task_id: str = ""
    response: dict[str, Any] = field(default_factory=dict)
    dry_run: bool = False
    error: str = ""


class TargetExecutor(ABC):
    @abstractmethod
    def execute(
        self, signal: Signal, context: StrategyContext | None = None
    ) -> ExecutionResult:
        raise NotImplementedError


class DryRunExecutor(TargetExecutor):
    def __init__(self, logger: Callable[[str], None] = print):
        self.logger = logger

    def execute(
        self, signal: Signal, context: StrategyContext | None = None
    ) -> ExecutionResult:
        self.logger(
            f"[dry-run] {signal.symbol} target={signal.target_volume:+d} "
            f"reason={signal.reason}"
        )
        return ExecutionResult(
            accepted=False,
            target_volume=signal.target_volume,
            dry_run=True,
        )


class NodeTargetExecutor(TargetExecutor):
    """Submit through tqsdk-node's TargetPosTask REST endpoint."""

    def __init__(
        self,
        client: Any,
        *,
        mode: str = "ACTIVE",
        max_volume: int = 0,
    ):
        self.client = client
        self.mode = mode
        self.max_volume = max_volume
        self._task_id = ""

    def execute(
        self, signal: Signal, context: StrategyContext | None = None
    ) -> ExecutionResult:
        # A TargetPosTask remains active until cancelled. Cancel the previous
        # task before creating a task for the new desired net position,
        # otherwise an old +N task can re-open a position after a later flat
        # task has closed it.
        if self._task_id:
            try:
                self.client.cancel_target(self._task_id)
            except Exception:
                pass
            self._task_id = ""
        response = self.client.target_position(
            signal.symbol,
            signal.target_volume,
            mode=self.mode,
            max_volume=self.max_volume,
        )
        accepted = bool(response.get("success", True))
        if accepted:
            self._task_id = str(response.get("task_id", ""))
        return ExecutionResult(
            accepted=accepted,
            target_volume=signal.target_volume,
            task_id=str(response.get("task_id", "")),
            response=response,
            error=str(response.get("error", "")),
        )

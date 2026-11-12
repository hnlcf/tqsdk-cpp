"""Signal consumers and composition.

A strategy emits a `Signal` (desired net position). Consumers can:
  * notify a human (log, memory, WeCom, generic webhook);
  * execute an order via a TargetExecutor;
  * fan out to a CompositeConsumer or a SignalDispatcher.

Notification failures are normally isolated so they do not block order
execution. Execution failures remain visible in the DispatchResult and/or are
raised by the executor.
"""

from __future__ import annotations

import json
import logging
import urllib.error
import urllib.request
from abc import ABC, abstractmethod
from dataclasses import dataclass, field
from typing import Any, Callable, Iterable

from .models import Signal, StrategyContext


class ConsumerError(RuntimeError):
    pass


class CompositeConsumerError(ConsumerError):
    def __init__(self, errors: list[tuple[str, BaseException]]):
        self.errors = errors
        detail = "; ".join(f"{name}: {exc}" for name, exc in errors)
        super().__init__(detail)


class SignalConsumer(ABC):
    @abstractmethod
    def consume(self, signal: Signal, context: StrategyContext | None = None) -> None:
        raise NotImplementedError


def format_signal(signal: Signal) -> str:
    lines = [
        f"策略: {signal.strategy_id or 'unknown'}",
        f"合约: {signal.symbol}",
        f"信号: {signal.signal} ({signal.reason})",
        f"目标净持仓: {signal.target_volume:+d} 手",
    ]
    if signal.stop_price is not None:
        lines.append(f"止损参考: {signal.stop_price:.4f}")
    if signal.timestamp_ns:
        lines.append(f"时间(ns): {signal.timestamp_ns}")
    return "\n".join(lines)


class NullConsumer(SignalConsumer):
    def consume(self, signal: Signal, context: StrategyContext | None = None) -> None:
        return None


class LoggingConsumer(SignalConsumer):
    def __init__(
        self,
        logger: logging.Logger | Callable[[str], None] | None = None,
        *,
        level: int = logging.INFO,
    ):
        self.logger = logger
        self.level = level

    def consume(self, signal: Signal, context: StrategyContext | None = None) -> None:
        text = format_signal(signal)
        if self.logger is None:
            logging.getLogger("tqsdk_py.signal").log(self.level, text)
        elif hasattr(self.logger, "log"):
            self.logger.log(self.level, text)  # type: ignore[union-attr]
        else:
            self.logger(text)  # type: ignore[operator]


class MemoryConsumer(SignalConsumer):
    def __init__(self) -> None:
        self.signals: list[Signal] = []

    def consume(self, signal: Signal, context: StrategyContext | None = None) -> None:
        self.signals.append(signal)

    def clear(self) -> None:
        self.signals.clear()


class HttpWebhookConsumer(SignalConsumer):
    """Generic JSON webhook consumer."""

    def __init__(
        self,
        url: str,
        *,
        formatter: Callable[[Signal], dict[str, Any]] | None = None,
        timeout: float = 10.0,
        headers: dict[str, str] | None = None,
    ):
        self.url = url
        self.formatter = formatter or (lambda signal: {"text": format_signal(signal)})
        self.timeout = timeout
        self.headers = {"Content-Type": "application/json", **(headers or {})}

    def _payload(self, signal: Signal) -> dict[str, Any]:
        return self.formatter(signal)

    def consume(self, signal: Signal, context: StrategyContext | None = None) -> None:
        data = json.dumps(self._payload(signal), ensure_ascii=False).encode("utf-8")
        request = urllib.request.Request(
            self.url, data=data, headers=self.headers, method="POST"
        )
        try:
            with urllib.request.urlopen(request, timeout=self.timeout) as response:
                raw = response.read()
        except urllib.error.HTTPError as exc:
            raise ConsumerError(f"webhook HTTP {exc.code}") from exc
        except Exception as exc:  # noqa: BLE001 - notification adapters are pluggable
            raise ConsumerError(f"webhook request failed: {exc}") from exc
        if raw:
            try:
                body = json.loads(raw)
            except json.JSONDecodeError:
                return
            if isinstance(body, dict) and body.get("errcode") not in (None, 0):
                raise ConsumerError(
                    f"webhook rejected: errcode={body.get('errcode')} "
                    f"errmsg={body.get('errmsg', '')}"
                )


class WeComConsumer(HttpWebhookConsumer):
    """Enterprise WeChat group robot webhook consumer.

    The webhook URL is a secret and is deliberately never included in errors or
    logs. Configure it via an env var in the CLI, not in source.
    """

    def __init__(
        self,
        webhook_url: str,
        *,
        timeout: float = 10.0,
        mention: str | None = None,
    ):
        if not webhook_url:
            raise ValueError("WeCom webhook URL is empty")
        self.mention = mention

        def payload(signal: Signal) -> dict[str, Any]:
            content = format_signal(signal)
            if self.mention:
                content += f"\n<@{self.mention}>"
            return {"msgtype": "markdown", "markdown": {"content": content}}

        super().__init__(webhook_url, formatter=payload, timeout=timeout)


class CompositeConsumer(SignalConsumer):
    """Sequential fan-out; notification failures are collected by default."""

    def __init__(
        self,
        consumers: Iterable[SignalConsumer],
        *,
        continue_on_error: bool = True,
        logger: Callable[[str], None] | None = None,
    ):
        self.consumers = list(consumers)
        self.continue_on_error = continue_on_error
        self.logger = logger
        self.errors: list[tuple[str, BaseException]] = []

    def consume(self, signal: Signal, context: StrategyContext | None = None) -> None:
        errors: list[tuple[str, BaseException]] = []
        for consumer in self.consumers:
            try:
                consumer.consume(signal, context)
            except Exception as exc:  # noqa: BLE001 - isolate notification failures
                errors.append((type(consumer).__name__, exc))
                if self.logger is not None:
                    self.logger(f"{type(consumer).__name__} failed: {exc}")
        self.errors = errors
        if errors and not self.continue_on_error:
            raise CompositeConsumerError(errors)


class OrderExecutorConsumer(SignalConsumer):
    """SignalConsumer adapter around a TargetExecutor.

    Useful when a caller wants `CompositeConsumer([wecom, order])`.
    """

    def __init__(self, executor: Any):
        self.executor = executor

    def consume(self, signal: Signal, context: StrategyContext | None = None) -> None:
        self.executor.execute(signal, context)


@dataclass
class DispatchResult:
    signal: Signal
    errors: list[tuple[str, BaseException]] = field(default_factory=list)
    execution: Any = None


class SignalDispatcher:
    """Route a signal to notifiers, then optionally to a critical executor."""

    def __init__(
        self,
        notifiers: Iterable[SignalConsumer] = (),
        executor: Any = None,
        *,
        continue_on_notifier_error: bool = True,
        logger: Callable[[str], None] | None = None,
    ):
        self.notifiers = list(notifiers)
        self.executor = executor
        self.continue_on_notifier_error = continue_on_notifier_error
        self.logger = logger

    def dispatch(
        self, signal: Signal, context: StrategyContext | None = None
    ) -> DispatchResult:
        errors: list[tuple[str, BaseException]] = []
        for consumer in self.notifiers:
            try:
                consumer.consume(signal, context)
            except Exception as exc:  # noqa: BLE001 - notifications must not hide orders
                errors.append((type(consumer).__name__, exc))
                if self.logger is not None:
                    self.logger(f"{type(consumer).__name__} failed: {exc}")
                if not self.continue_on_notifier_error:
                    raise CompositeConsumerError(errors)
        execution = None
        if self.executor is not None:
            # Execution is deliberately not caught: the runner/operator must
            # see a failed order submission.
            execution = self.executor.execute(signal, context)
        return DispatchResult(signal=signal, errors=errors, execution=execution)


# Convenience aliases used by Chinese documentation/examples.
企业微信Consumer = WeComConsumer
组合Consumer = CompositeConsumer
信号分发器 = SignalDispatcher

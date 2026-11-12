"""Language-neutral data models shared by backtest and live adapters."""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Sequence


@dataclass
class Bar:
    datetime: int
    open: float
    high: float
    low: float
    close: float
    volume: float = 0.0

    @classmethod
    def from_value(cls, value: Any) -> "Bar":
        if isinstance(value, cls):
            return value
        if isinstance(value, dict):
            return cls(
                int(value["datetime"]),
                float(value["open"]),
                float(value["high"]),
                float(value["low"]),
                float(value["close"]),
                float(value.get("volume", 0.0)),
            )
        if isinstance(value, Sequence) and not isinstance(value, (str, bytes)):
            return cls(
                int(value[0]),
                float(value[1]),
                float(value[2]),
                float(value[3]),
                float(value[4]),
                float(value[5]) if len(value) > 5 else 0.0,
            )
        if hasattr(value, "datetime") and hasattr(value, "close"):
            return cls(
                int(value.datetime),
                float(value.open),
                float(value.high),
                float(value.low),
                float(value.close),
                float(getattr(value, "volume", 0.0)),
            )
        raise TypeError(f"cannot convert {type(value)!r} to Bar")


@dataclass
class ContractSpec:
    symbol: str
    volume_multiple: float
    margin_rate: float
    commission_per_lot: float = 0.0
    price_tick: float = 0.0

    @classmethod
    def from_dict(cls, symbol: str, value: dict[str, Any]) -> "ContractSpec":
        return cls(
            symbol=symbol,
            volume_multiple=float(value.get("volume_multiple", 0.0)),
            margin_rate=float(value.get("margin_rate_est", value.get("margin_rate", 0.0))),
            commission_per_lot=float(value.get("commission_per_lot", 0.0)),
            price_tick=float(value.get("price_tick", 0.0)),
        )


@dataclass(frozen=True)
class Signal:
    """A desired net-position signal, not an order type.

    target_volume is signed: +N long, -N short, 0 flat. This is the language
    shared by strategies, notifiers, order executors and backtest/live runners.
    """

    symbol: str
    target_volume: int
    reason: str
    signal: str = "flat"
    stop_price: float | None = None
    lots: int = 0
    timestamp_ns: int = 0
    strategy_id: str = ""
    metadata: dict[str, Any] = field(default_factory=dict)


@dataclass(frozen=True)
class StrategyContext:
    symbol: str
    bars: tuple[Bar, ...]
    position: int
    equity: float
    spec: ContractSpec
    price: float | None = None
    entry_price: float | None = None
    now_ns: int = 0

"""Data-source adapters: JSON research files and live tqsdk-node sources."""

from __future__ import annotations

import json
import re
import time
from abc import ABC, abstractmethod
from pathlib import Path
from typing import Protocol, Sequence

from .models import Bar, ContractSpec


class BarSource(ABC):
    """Common history adapter for backtest and live warm-up."""

    @abstractmethod
    def load(self, symbol: str, duration_ns: int, width: int = 200) -> list[Bar]:
        raise NotImplementedError


class InMemoryBarSource(BarSource):
    """Small deterministic source for tests/examples."""

    def __init__(self, bars: Sequence[Bar] | dict[str, Sequence[Bar]]):
        self.bars = bars

    def load(self, symbol: str, duration_ns: int, width: int = 200) -> list[Bar]:
        values = self.bars.get(symbol, []) if isinstance(self.bars, dict) else self.bars
        out = [Bar.from_value(item) for item in values]
        return out[-width:] if width > 0 else out


class JsonBarSource(BarSource):
    """Load Tq backtest daily JSON files from a file or directory."""

    def __init__(self, path: Path | str, *, strip_incomplete: bool = False):
        self.path = Path(path)
        self.strip_incomplete = strip_incomplete

    def _resolve(self, symbol: str) -> Path:
        if self.path.is_file():
            return self.path
        if not self.path.is_dir():
            raise FileNotFoundError(f"bar source does not exist: {self.path}")
        direct = self.path / f"{symbol}.json"
        if direct.exists():
            return direct
        # Accept KQ.m@... for a KQ.i@... research file as a convenience.
        product = symbol.split("@")[-1].split(".")[-1]
        candidates = sorted(self.path.glob(f"KQ.i@*.{product}.json"))
        if candidates:
            return candidates[0]
        raise FileNotFoundError(f"no bar file for {symbol} under {self.path}")

    def load(self, symbol: str, duration_ns: int, width: int = 200) -> list[Bar]:
        payload = json.loads(self._resolve(symbol).read_text())
        bars = [Bar.from_value(item) for item in payload.get("bars", [])]
        bars.sort(key=lambda item: item.datetime)
        dedup: list[Bar] = []
        for bar in bars:
            if dedup and dedup[-1].datetime == bar.datetime:
                dedup[-1] = bar
            else:
                dedup.append(bar)
        if width > 0:
            dedup = dedup[-width:]
        if self.strip_incomplete:
            dedup = closed_bars(dedup, duration_ns)
        return dedup


class MarketDataClient(Protocol):
    def kline(self, symbol: str, duration_ns: int, width: int = 200) -> list[Bar]:
        ...


class NodeBarSource(BarSource):
    """Wrap a NodeClient-like object with the BarSource interface."""

    def __init__(self, client: MarketDataClient, *, width: int = 200, strip_incomplete: bool = True):
        self.client = client
        self.width = width
        self.strip_incomplete = strip_incomplete

    def load(self, symbol: str, duration_ns: int, width: int = 0) -> list[Bar]:
        bars = self.client.kline(symbol, duration_ns, width or self.width)
        if self.strip_incomplete:
            bars = closed_bars(bars, duration_ns)
        return bars


def closed_bars(
    bars: Sequence[Bar], duration_ns: int, now_ns: int | None = None
) -> list[Bar]:
    """Drop the latest bar unless its interval has ended.

    Tq bar datetime is the bar start. A live process must not use a partial bar
    for an EMA breakout signal.
    """
    bars = list(bars)
    if not bars:
        return []
    now_ns = now_ns if now_ns is not None else time.time_ns()
    if bars[-1].datetime + duration_ns > now_ns:
        return bars[:-1]
    return bars


def load_specs(path: Path | str) -> dict[str, ContractSpec]:
    payload = json.loads(Path(path).read_text())
    return {
        symbol: ContractSpec.from_dict(symbol, value)
        for symbol, value in payload.items()
        if isinstance(value, dict)
    }


def resolve_spec(specs: dict[str, ContractSpec], trading_symbol: str) -> ContractSpec | None:
    """Map an actual contract (SHFE.rb2701) to the KQ.i@ product spec."""
    if trading_symbol in specs:
        return specs[trading_symbol]
    if "." not in trading_symbol:
        return None
    exchange, contract = trading_symbol.split(".", 1)
    match = re.match(r"([A-Za-z]+)", contract)
    if not match:
        return None
    return specs.get(f"KQ.i@{exchange}.{match.group(1)}")


def load_daily_json(path: Path | str) -> list[Bar]:
    source = JsonBarSource(path)
    return source.load("", 0, width=0)

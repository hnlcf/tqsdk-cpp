"""Python ctypes binding for the TqSdk C++ BacktestEngine.

The binding intentionally uses a small C ABI instead of pybind11/CPython
headers, so it works with the project's existing CMake build and any CPython 3
interpreter. The strategy callback still runs in Python; order execution and
accounting run in the C++ engine.

Build the shared library first:

    cmake --build build --target tqsdk_backtest

Then run with:

    PYTHONPATH=python TQSDK_BACKTEST_LIB=build/libtqsdk_backtest.so \
      python3 python/examples/ema_backtest.py

The C++ engine is single-threaded; a strategy context is valid only inside the
callback. Do not retain it or call it after `run()` returns.
"""

from __future__ import annotations

import ctypes
import ctypes.util
import os
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Iterable, Sequence

__all__ = [
    "Bar",
    "BacktestConfig",
    "BacktestEngine",
    "BacktestResult",
    "Context",
    "Fill",
    "Stats",
    "find_library",
    "lots_for_margin",
]


class _CConfig(ctypes.Structure):
    _fields_ = [
        ("initial_balance", ctypes.c_double),
        ("volume_multiple", ctypes.c_double),
        ("margin_rate", ctypes.c_double),
        ("commission_rate", ctypes.c_double),
        ("commission_per_lot", ctypes.c_double),
        ("execute_on_bar_close", ctypes.c_int),
    ]


class _CBar(ctypes.Structure):
    _fields_ = [
        ("datetime", ctypes.c_int64),
        ("open", ctypes.c_double),
        ("high", ctypes.c_double),
        ("low", ctypes.c_double),
        ("close", ctypes.c_double),
        ("volume", ctypes.c_uint64),
    ]


class _CFill(ctypes.Structure):
    _fields_ = [
        ("datetime", ctypes.c_int64),
        ("direction", ctypes.c_int32),
        ("offset", ctypes.c_int32),
        ("price", ctypes.c_double),
        ("volume", ctypes.c_uint64),
        ("commission", ctypes.c_double),
        ("realized_pnl", ctypes.c_double),
    ]


class _CStats(ctypes.Structure):
    _fields_ = [
        ("bars", ctypes.c_size_t),
        ("fills", ctypes.c_size_t),
        ("win_fills", ctypes.c_size_t),
        ("initial_balance", ctypes.c_double),
        ("final_equity", ctypes.c_double),
        ("total_return", ctypes.c_double),
        ("realized_pnl", ctypes.c_double),
        ("commission", ctypes.c_double),
        ("max_drawdown", ctypes.c_double),
        ("max_drawdown_pct", ctypes.c_double),
        ("final_position", ctypes.c_int64),
        ("status", ctypes.c_int32),
        ("error", ctypes.c_char_p),
    ]


_StrategyCallback = ctypes.CFUNCTYPE(
    ctypes.c_int,
    ctypes.c_void_p,
    ctypes.POINTER(_CBar),
    ctypes.c_void_p,
)


def find_library() -> Path:
    """Locate libtqsdk_backtest.so.

    Priority:
      1. TQSDK_BACKTEST_LIB
      2. the current directory / package-relative source tree
      3. standard linker search (installed package)
    """
    configured = os.environ.get("TQSDK_BACKTEST_LIB")
    if configured:
        path = Path(configured)
        if path.is_dir():
            path = path / "libtqsdk_backtest.so"
        if not path.exists():
            raise FileNotFoundError(f"TQSDK_BACKTEST_LIB does not exist: {path}")
        return path

    package_root = Path(__file__).resolve().parent
    candidates: list[Path] = []
    for root in (Path.cwd(), package_root, package_root.parent):
        candidates.extend(root.glob("build*/libtqsdk_backtest.so"))
        candidates.extend(root.glob("build*/lib/libtqsdk_backtest.so"))
        candidates.extend(root.glob("build*/**/libtqsdk_backtest.so"))
    for candidate in candidates:
        if candidate.is_file():
            return candidate

    found = ctypes.util.find_library("tqsdk_backtest")
    if found:
        return Path(found)
    raise FileNotFoundError(
        "找不到 libtqsdk_backtest.so；先运行 "
        "`cmake --build build --target tqsdk_backtest`，"
        "或设置 TQSDK_BACKTEST_LIB"
    )


_LIB: ctypes.CDLL | None = None


def _library() -> ctypes.CDLL:
    global _LIB
    if _LIB is not None:
        return _LIB

    lib = ctypes.CDLL(str(find_library()))
    lib.tq_bt_create.argtypes = [ctypes.POINTER(_CConfig)]
    lib.tq_bt_create.restype = ctypes.c_void_p
    lib.tq_bt_destroy.argtypes = [ctypes.c_void_p]
    lib.tq_bt_destroy.restype = None
    lib.tq_bt_run.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(_CBar),
        ctypes.c_size_t,
        _StrategyCallback,
        ctypes.c_void_p,
        ctypes.POINTER(_CStats),
    ]
    lib.tq_bt_run.restype = ctypes.c_int
    lib.tq_bt_position.argtypes = [ctypes.c_void_p]
    lib.tq_bt_position.restype = ctypes.c_int64
    lib.tq_bt_equity.argtypes = [ctypes.c_void_p]
    lib.tq_bt_equity.restype = ctypes.c_double
    lib.tq_bt_fill_count.argtypes = [ctypes.c_void_p]
    lib.tq_bt_fill_count.restype = ctypes.c_size_t
    lib.tq_bt_fill_at.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(_CFill)]
    lib.tq_bt_fill_at.restype = ctypes.c_int
    lib.tq_bt_last_error.argtypes = [ctypes.c_void_p]
    lib.tq_bt_last_error.restype = ctypes.c_char_p
    lib.tq_bt_context_position.argtypes = [ctypes.c_void_p]
    lib.tq_bt_context_position.restype = ctypes.c_int64
    lib.tq_bt_context_equity.argtypes = [ctypes.c_void_p]
    lib.tq_bt_context_equity.restype = ctypes.c_double
    lib.tq_bt_buy_open.argtypes = [ctypes.c_void_p, ctypes.c_uint64]
    lib.tq_bt_buy_open.restype = ctypes.c_int
    lib.tq_bt_sell_open.argtypes = [ctypes.c_void_p, ctypes.c_uint64]
    lib.tq_bt_sell_open.restype = ctypes.c_int
    lib.tq_bt_buy_close.argtypes = [ctypes.c_void_p, ctypes.c_uint64]
    lib.tq_bt_buy_close.restype = ctypes.c_int
    lib.tq_bt_sell_close.argtypes = [ctypes.c_void_p, ctypes.c_uint64]
    lib.tq_bt_sell_close.restype = ctypes.c_int
    lib.tq_bt_target_position.argtypes = [ctypes.c_void_p, ctypes.c_int64]
    lib.tq_bt_target_position.restype = ctypes.c_int
    _LIB = lib
    return lib


@dataclass
class BacktestConfig:
    initial_balance: float = 1_000_000.0
    volume_multiple: float = 1.0
    margin_rate: float = 0.10
    commission_rate: float = 0.0
    commission_per_lot: float = 0.0
    execute_on_bar_close: bool = True

    def _to_c(self) -> _CConfig:
        return _CConfig(
            self.initial_balance,
            self.volume_multiple,
            self.margin_rate,
            self.commission_rate,
            self.commission_per_lot,
            int(self.execute_on_bar_close),
        )


def lots_for_margin(
    equity: float,
    price: float,
    config: BacktestConfig,
    allocation: float = 0.30,
) -> int:
    """Integer lots using `equity * allocation` as initial margin.

    This is intentionally a Python helper: the C++ engine executes explicit
    volumes, while the strategy owns sizing and risk policy.
    """

    margin_per_lot = price * config.volume_multiple * config.margin_rate
    if price <= 0 or margin_per_lot <= 0 or equity <= 0:
        return 0
    return max(0, int((equity * allocation) / margin_per_lot))


@dataclass
class Bar:
    datetime: int
    open: float
    high: float
    low: float
    close: float
    volume: int = 0

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
                int(value.get("volume", 0)),
            )
        if isinstance(value, Sequence) and not isinstance(value, (str, bytes)):
            return cls(
                int(value[0]),
                float(value[1]),
                float(value[2]),
                float(value[3]),
                float(value[4]),
                int(value[5]) if len(value) > 5 else 0,
            )
        if hasattr(value, "datetime") and hasattr(value, "close"):
            return cls(
                int(value.datetime),
                float(value.open),
                float(value.high),
                float(value.low),
                float(value.close),
                int(getattr(value, "volume", 0)),
            )
        raise TypeError(f"cannot convert {type(value)!r} to Bar")

    def _to_c(self) -> _CBar:
        return _CBar(
            self.datetime,
            self.open,
            self.high,
            self.low,
            self.close,
            self.volume,
        )


@dataclass
class Fill:
    datetime: int
    direction: str
    offset: str
    price: float
    volume: int
    commission: float
    realized_pnl: float


@dataclass
class Stats:
    bars: int
    fills: int
    win_fills: int
    initial_balance: float
    final_equity: float
    total_return: float
    realized_pnl: float
    commission: float
    max_drawdown: float
    max_drawdown_pct: float
    final_position: int

    @property
    def total_return_pct(self) -> float:
        return self.total_return / self.initial_balance * 100.0 if self.initial_balance else 0.0


@dataclass
class BacktestResult:
    stats: Stats
    fills: list[Fill] = field(default_factory=list)
    equity_curve: list[tuple[int, float]] = field(default_factory=list)


class Context:
    """Order context passed to a Python strategy callback."""

    __slots__ = ("_context", "_lib")

    def __init__(self, context: int, lib: ctypes.CDLL):
        self._context = context
        self._lib = lib

    def position(self) -> int:
        return int(self._lib.tq_bt_context_position(self._context))

    def equity(self) -> float:
        return float(self._lib.tq_bt_context_equity(self._context))

    def buy_open(self, volume: int) -> bool:
        return bool(self._lib.tq_bt_buy_open(self._context, int(volume)))

    def sell_open(self, volume: int) -> bool:
        return bool(self._lib.tq_bt_sell_open(self._context, int(volume)))

    def buy_close(self, volume: int) -> bool:
        return bool(self._lib.tq_bt_buy_close(self._context, int(volume)))

    def sell_close(self, volume: int) -> bool:
        return bool(self._lib.tq_bt_sell_close(self._context, int(volume)))

    def target_position(self, target: int) -> bool:
        return bool(self._lib.tq_bt_target_position(self._context, int(target)))


class BacktestEngine:
    def __init__(self, config: BacktestConfig | None = None):
        self.config = config or BacktestConfig()
        self._lib = _library()
        self._engine = self._lib.tq_bt_create(ctypes.byref(self.config._to_c()))
        if not self._engine:
            raise RuntimeError("tq_bt_create returned null")

    def close(self) -> None:
        if self._engine:
            self._lib.tq_bt_destroy(self._engine)
            self._engine = None

    def __enter__(self) -> "BacktestEngine":
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.close()

    def __del__(self) -> None:
        try:
            self.close()
        except Exception:
            pass

    def run(
        self,
        bars: Iterable[Any],
        strategy: Callable[[Context, Bar], None] | None = None,
    ) -> BacktestResult:
        if self._engine is None:
            raise RuntimeError("backtest engine is closed")
        converted = [Bar.from_value(bar) for bar in bars]
        array = (_CBar * len(converted))(*[bar._to_c() for bar in converted])
        equity_curve: list[tuple[int, float]] = []
        callback_error: BaseException | None = None

        def callback(_user: int, bar_ptr, context_ptr: int) -> int:
            nonlocal callback_error
            try:
                bar = Bar(
                    int(bar_ptr.contents.datetime),
                    float(bar_ptr.contents.open),
                    float(bar_ptr.contents.high),
                    float(bar_ptr.contents.low),
                    float(bar_ptr.contents.close),
                    int(bar_ptr.contents.volume),
                )
                context = Context(int(context_ptr), self._lib)
                if strategy is not None:
                    strategy(context, bar)
                equity_curve.append((bar.datetime, context.equity()))
                return 0
            except BaseException as exc:  # propagate after ctypes returns
                callback_error = exc
                return 1

        c_callback = _StrategyCallback(callback)
        stats_c = _CStats()
        rc = self._lib.tq_bt_run(
            self._engine,
            array,
            len(converted),
            c_callback if strategy is not None else _StrategyCallback(),
            None,
            ctypes.byref(stats_c),
        )
        if callback_error is not None:
            raise RuntimeError(f"Python strategy failed: {callback_error}") from callback_error
        if rc != 0 or stats_c.status != 0:
            error = stats_c.error.decode("utf-8", "replace") if stats_c.error else ""
            if not error:
                last = self._lib.tq_bt_last_error(self._engine)
                error = last.decode("utf-8", "replace") if last else "unknown error"
            raise RuntimeError(f"backtest failed: {error}")

        fills: list[Fill] = []
        count = int(self._lib.tq_bt_fill_count(self._engine))
        for index in range(count):
            fill_c = _CFill()
            if self._lib.tq_bt_fill_at(self._engine, index, ctypes.byref(fill_c)) != 0:
                raise RuntimeError(f"failed to read fill {index}")
            fills.append(
                Fill(
                    datetime=int(fill_c.datetime),
                    direction="BUY" if fill_c.direction == 0 else "SELL",
                    offset="OPEN" if fill_c.offset == 0 else "CLOSE",
                    price=float(fill_c.price),
                    volume=int(fill_c.volume),
                    commission=float(fill_c.commission),
                    realized_pnl=float(fill_c.realized_pnl),
                )
            )
        stats = Stats(
            bars=int(stats_c.bars),
            fills=int(stats_c.fills),
            win_fills=int(stats_c.win_fills),
            initial_balance=float(stats_c.initial_balance),
            final_equity=float(stats_c.final_equity),
            total_return=float(stats_c.total_return),
            realized_pnl=float(stats_c.realized_pnl),
            commission=float(stats_c.commission),
            max_drawdown=float(stats_c.max_drawdown),
            max_drawdown_pct=float(stats_c.max_drawdown_pct),
            final_position=int(stats_c.final_position),
        )
        return BacktestResult(stats, fills, equity_curve)


def run_backtest(
    bars: Iterable[Any],
    strategy: Callable[[Context, Bar], None],
    config: BacktestConfig | None = None,
) -> BacktestResult:
    with BacktestEngine(config) as engine:
        return engine.run(bars, strategy)

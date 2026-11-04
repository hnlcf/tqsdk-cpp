#!/usr/bin/env python3
"""TQSDK Qt indicator sidecar.

The sidecar is intentionally a separate process. The Qt UI only sends numeric
OHLCV bars and receives numeric series; no Python executes in the GUI thread.

Wire format (both directions):
    4-byte big-endian payload length + UTF-8 compact JSON

Requests:
    {"type":"init","indicator_id":"ma","params":{"fast":5,"slow":20}}
    {"type":"bars","indicator_id":"ma","reset":true,"bars":[...]}
    {"type":"bars","indicator_id":"ma","reset":false,"bars":[...]}

Responses:
    {"type":"ready","protocol":1}
    {"type":"series","indicator_id":"ma","series":[{"name":"ma_fast",
       "panel":"main","points":[{"datetime":..., "value":...}]}]}
    {"type":"error","indicator_id":"ma","code":"...","message":"..."}
"""

from __future__ import annotations

import json
import math
import struct
import sys
from typing import Any

PROTOCOL_VERSION = 1
MAX_BARS = 20000

DEFAULTS: dict[str, dict[str, float]] = {
    "ma": {"fast": 5, "slow": 20},
    "ema": {"fast": 12, "slow": 26},
    "boll": {"period": 20, "mult": 2.0},
    "macd": {"fast": 12, "slow": 26, "signal": 9},
}


def read_exact(stream: Any, size: int) -> bytes | None:
    data = bytearray()
    while len(data) < size:
        chunk = stream.read(size - len(data))
        if not chunk:
            return None
        data.extend(chunk)
    return bytes(data)


def send(message: dict[str, Any]) -> None:
    payload = json.dumps(message, separators=(",", ":"), allow_nan=False).encode("utf-8")
    sys.stdout.buffer.write(struct.pack(">I", len(payload)) + payload)
    sys.stdout.buffer.flush()


def finite(value: Any) -> float | None:
    try:
        number = float(value)
    except (TypeError, ValueError):
        return None
    return number if math.isfinite(number) else None


def sma(values: list[float | None], period: int) -> list[float | None]:
    period = max(1, int(period))
    result: list[float | None] = [None] * len(values)
    total = 0.0
    valid = 0
    for i, value in enumerate(values):
        if value is None:
            continue
        total += value
        valid += 1
        if i >= period and values[i - period] is not None:
            total -= values[i - period]
            valid -= 1
        if valid == period:
            result[i] = total / period
    return result


def ema(values: list[float | None], period: int) -> list[float | None]:
    period = max(1, int(period))
    result: list[float | None] = [None] * len(values)
    alpha = 2.0 / (period + 1.0)
    previous: float | None = None
    for i, value in enumerate(values):
        if value is None:
            continue
        previous = value if previous is None else alpha * value + (1.0 - alpha) * previous
        result[i] = previous
    return result


def stdev(values: list[float | None], period: int) -> list[float | None]:
    period = max(1, int(period))
    result: list[float | None] = [None] * len(values)
    for i in range(period - 1, len(values)):
        window = values[i - period + 1 : i + 1]
        if any(v is None for v in window):
            continue
        mean = sum(v for v in window if v is not None) / period
        variance = sum((v - mean) ** 2 for v in window if v is not None) / period
        result[i] = math.sqrt(variance)
    return result


def series(name: str, panel: str, datetimes: list[int], values: list[float | None]) -> dict[str, Any]:
    points = [
        {"datetime": int(datetime), "value": float(value)}
        for datetime, value in zip(datetimes, values)
        if value is not None and math.isfinite(value)
    ]
    return {"name": name, "panel": panel, "points": points}


def compute(indicator_id: str, params: dict[str, Any], bars: list[dict[str, Any]]) -> list[dict[str, Any]]:
    datetimes = [int(bar["datetime"]) for bar in bars]
    closes = [finite(bar.get("close")) for bar in bars]

    if indicator_id == "ma":
        fast = max(1, int(params.get("fast", 5)))
        slow = max(1, int(params.get("slow", 20)))
        return [
            series("ma_fast", "main", datetimes, sma(closes, fast)),
            series("ma_slow", "main", datetimes, sma(closes, slow)),
        ]
    if indicator_id == "ema":
        fast = max(1, int(params.get("fast", 12)))
        slow = max(1, int(params.get("slow", 26)))
        return [
            series("ema_fast", "main", datetimes, ema(closes, fast)),
            series("ema_slow", "main", datetimes, ema(closes, slow)),
        ]
    if indicator_id == "boll":
        period = max(1, int(params.get("period", 20)))
        mult = max(0.0, float(params.get("mult", 2.0)))
        middle = sma(closes, period)
        deviation = stdev(closes, period)
        upper = [None if m is None or d is None else m + mult * d for m, d in zip(middle, deviation)]
        lower = [None if m is None or d is None else m - mult * d for m, d in zip(middle, deviation)]
        return [
            series("boll_mid", "main", datetimes, middle),
            series("boll_upper", "main", datetimes, upper),
            series("boll_lower", "main", datetimes, lower),
        ]
    if indicator_id == "macd":
        fast = max(1, int(params.get("fast", 12)))
        slow = max(1, int(params.get("slow", 26)))
        signal_period = max(1, int(params.get("signal", 9)))
        fast_ema = ema(closes, fast)
        slow_ema = ema(closes, slow)
        macd_values = [
            None if f is None or s is None else f - s for f, s in zip(fast_ema, slow_ema)
        ]
        signal_values = ema(macd_values, signal_period)
        histogram_values = [
            None if m is None or s is None else m - s
            for m, s in zip(macd_values, signal_values)
        ]
        return [
            series("macd", "sub", datetimes, macd_values),
            series("signal", "sub", datetimes, signal_values),
            series("histogram", "sub", datetimes, histogram_values),
        ]
    raise ValueError(f"unknown indicator: {indicator_id}")


def merge_bars(existing: list[dict[str, Any]], incoming: list[dict[str, Any]]) -> list[dict[str, Any]]:
    by_time = {int(bar["datetime"]): bar for bar in existing}
    for bar in incoming:
        by_time[int(bar["datetime"])] = bar
    merged = sorted(by_time.values(), key=lambda bar: int(bar["datetime"]))
    return merged[-MAX_BARS:]


def main() -> int:
    send({"type": "ready", "protocol": PROTOCOL_VERSION})
    states: dict[str, dict[str, Any]] = {}
    while True:
        header = read_exact(sys.stdin.buffer, 4)
        if header is None:
            return 0
        length = struct.unpack(">I", header)[0]
        if length == 0 or length > 16 * 1024 * 1024:
            send({"type": "error", "code": "frame", "message": "invalid frame length"})
            return 1
        payload = read_exact(sys.stdin.buffer, length)
        if payload is None:
            return 0
        try:
            message = json.loads(payload)
            indicator_id = str(message.get("indicator_id", ""))
            kind = str(message.get("type", ""))
            if kind == "init":
                if indicator_id not in DEFAULTS:
                    raise ValueError(f"unknown indicator: {indicator_id}")
                defaults = DEFAULTS[indicator_id].copy()
                defaults.update(message.get("params") or {})
                states[indicator_id] = {"params": defaults, "bars": []}
                send({"type": "initialized", "indicator_id": indicator_id})
            elif kind == "bars":
                if indicator_id not in states:
                    raise ValueError(f"indicator not initialized: {indicator_id}")
                state = states[indicator_id]
                incoming = [bar for bar in message.get("bars", []) if isinstance(bar, dict)]
                state["bars"] = incoming[-MAX_BARS:] if message.get("reset") else merge_bars(
                    state["bars"], incoming
                )
                result = compute(indicator_id, state["params"], state["bars"])
                send({"type": "series", "indicator_id": indicator_id, "series": result})
            elif kind == "stop":
                return 0
            else:
                raise ValueError(f"unknown request type: {kind}")
        except Exception as exc:  # noqa: BLE001 - report plugin errors over the protocol
            send(
                {
                    "type": "error",
                    "indicator_id": str(locals().get("indicator_id", "")),
                    "code": "indicator_error",
                    "message": str(exc),
                }
            )


if __name__ == "__main__":
    raise SystemExit(main())

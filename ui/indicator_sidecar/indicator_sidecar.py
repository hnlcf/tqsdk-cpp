#!/usr/bin/env python3
"""TQSDK Qt indicator sidecar.

The sidecar is intentionally a separate process. The Qt UI only sends numeric
OHLCV bars and receives numeric series; no Python executes in the GUI thread.

Plugin discovery
----------------
The manager passes ``TQSDK_INDICATOR_PLUGIN_DIRS`` (path-separator separated).
Each plugin directory contains one subdirectory per indicator:

    ma/
      manifest.json
      indicator.py       # optional for built-in indicators

Built-in indicators are implemented below and registered by manifest id. A
third-party plugin only needs a manifest and an ``indicator.py`` exposing:

    def compute(params, bars):
        return {"my_series": [value_or_None, ...]}

``bars`` is a list of dicts with datetime/open/high/low/close/volume/open_oi/
close_oi. The returned list must align with the input bars; None/NaN points are
not sent to the chart.

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

import importlib.util
import json
import math
import os
import struct
import sys
from pathlib import Path
from typing import Any, Callable

PROTOCOL_VERSION = 1
MAX_BARS = 20000


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


def closes(bars: list[dict[str, Any]]) -> list[float | None]:
    return [finite(bar.get("close")) for bar in bars]


def compute_ma(params: dict[str, Any], bars: list[dict[str, Any]]) -> dict[str, list[Any]]:
    values = closes(bars)
    fast = max(1, int(params.get("fast", 5)))
    slow = max(1, int(params.get("slow", 20)))
    return {"ma_fast": sma(values, fast), "ma_slow": sma(values, slow)}


def compute_ema(params: dict[str, Any], bars: list[dict[str, Any]]) -> dict[str, list[Any]]:
    values = closes(bars)
    fast = max(1, int(params.get("fast", 12)))
    slow = max(1, int(params.get("slow", 26)))
    return {"ema_fast": ema(values, fast), "ema_slow": ema(values, slow)}


def compute_boll(params: dict[str, Any], bars: list[dict[str, Any]]) -> dict[str, list[Any]]:
    values = closes(bars)
    period = max(1, int(params.get("period", 20)))
    mult = max(0.0, float(params.get("mult", 2.0)))
    middle = sma(values, period)
    deviation = stdev(values, period)
    upper = [None if m is None or d is None else m + mult * d for m, d in zip(middle, deviation)]
    lower = [None if m is None or d is None else m - mult * d for m, d in zip(middle, deviation)]
    return {"boll_mid": middle, "boll_upper": upper, "boll_lower": lower}


def compute_macd(params: dict[str, Any], bars: list[dict[str, Any]]) -> dict[str, list[Any]]:
    values = closes(bars)
    fast = max(1, int(params.get("fast", 12)))
    slow = max(1, int(params.get("slow", 26)))
    signal_period = max(1, int(params.get("signal", 9)))
    fast_ema = ema(values, fast)
    slow_ema = ema(values, slow)
    macd_values = [None if f is None or s is None else f - s for f, s in zip(fast_ema, slow_ema)]
    signal_values = ema(macd_values, signal_period)
    histogram_values = [
        None if m is None or s is None else m - s for m, s in zip(macd_values, signal_values)
    ]
    return {"macd": macd_values, "signal": signal_values, "histogram": histogram_values}


BUILTIN_COMPUTERS: dict[str, Callable[[dict[str, Any], list[dict[str, Any]]], dict[str, list[Any]]]] = {
    "ma": compute_ma,
    "ema": compute_ema,
    "boll": compute_boll,
    "macd": compute_macd,
}


def plugin_directories() -> list[Path]:
    configured = os.environ.get("TQSDK_INDICATOR_PLUGIN_DIRS", "")
    directories = [Path(item) for item in configured.split(os.pathsep) if item.strip()]
    if not directories:
        directories = [Path(__file__).resolve().parent / "plugins"]
    result: list[Path] = []
    seen: set[str] = set()
    for directory in directories:
        resolved = str(directory.resolve())
        if resolved not in seen:
            seen.add(resolved)
            result.append(directory)
    return result


def load_plugin_module(indicator_id: str, entry: Path) -> Callable[..., Any]:
    spec = importlib.util.spec_from_file_location(f"tqsdk_indicator_{indicator_id}", entry)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load plugin module: {entry}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    compute = getattr(module, "compute", None)
    if not callable(compute):
        raise RuntimeError(f"plugin {indicator_id} has no compute(params, bars)")
    return compute


def load_catalog() -> dict[str, dict[str, Any]]:
    catalog: dict[str, dict[str, Any]] = {}
    for directory in plugin_directories():
        if not directory.is_dir():
            continue
        candidates = sorted(directory.glob("*/manifest.json"))
        direct = directory / "manifest.json"
        if direct.exists():
            candidates.append(direct)
        for manifest_path in candidates:
            try:
                manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
                indicator_id = str(manifest.get("id", "")).strip()
                if not indicator_id or indicator_id in catalog:
                    continue
                if manifest.get("panel") not in ("main", "sub"):
                    raise ValueError("panel must be main or sub")
                if indicator_id in BUILTIN_COMPUTERS:
                    compute = BUILTIN_COMPUTERS[indicator_id]
                else:
                    entry_name = str(manifest.get("entry", "indicator.py"))
                    entry = (manifest_path.parent / entry_name).resolve()
                    base = manifest_path.parent.resolve()
                    try:
                        entry.relative_to(base)
                    except ValueError as exc:
                        raise ValueError("plugin entry escapes plugin directory") from exc
                    compute = load_plugin_module(indicator_id, entry)
                catalog[indicator_id] = {"manifest": manifest, "compute": compute}
            except Exception as exc:  # noqa: BLE001 - one bad plugin must not kill others
                print(f"indicator plugin skipped: {manifest_path}: {exc}", file=sys.stderr)
    return catalog


def default_params(manifest: dict[str, Any]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for item in manifest.get("params", []):
        if isinstance(item, dict) and "key" in item:
            result[str(item["key"])] = item.get("default", 0)
    return result


def series_specs(manifest: dict[str, Any], result: dict[str, Any]) -> list[dict[str, Any]]:
    specs = [item for item in manifest.get("series", []) if isinstance(item, dict)]
    if specs:
        return specs
    return [{"name": name, "label": name, "color": "#9aa4b2"} for name in result]


def build_series(
    indicator_id: str,
    manifest: dict[str, Any],
    datetimes: list[int],
    values_by_name: dict[str, Any],
) -> list[dict[str, Any]]:
    output: list[dict[str, Any]] = []
    for item in series_specs(manifest, values_by_name):
        name = str(item.get("name", ""))
        values = values_by_name.get(name)
        if values is None or not isinstance(values, (list, tuple)):
            values = []
        if len(values) != len(datetimes):
            raise ValueError(
                f"series {name} length {len(values)} does not match bars {len(datetimes)}"
            )
        points = []
        for datetime, raw in zip(datetimes, values):
            value = finite(raw)
            if value is not None:
                points.append({"datetime": int(datetime), "value": value})
        output.append(
            {
                "name": name,
                "panel": str(manifest.get("panel", "sub")),
                "points": points,
            }
        )
    return output


def run_compute(
    indicator_id: str,
    state: dict[str, Any],
    bars: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    manifest = state["manifest"]
    values = state["compute"](state["params"], bars)
    if not isinstance(values, dict):
        raise ValueError("plugin compute must return a dict of series")
    return build_series(indicator_id, manifest, [int(bar["datetime"]) for bar in bars], values)


def merge_bars(existing: list[dict[str, Any]], incoming: list[dict[str, Any]]) -> list[dict[str, Any]]:
    by_time = {int(bar["datetime"]): bar for bar in existing}
    for bar in incoming:
        by_time[int(bar["datetime"])] = bar
    merged = sorted(by_time.values(), key=lambda bar: int(bar["datetime"]))
    return merged[-MAX_BARS:]


def main() -> int:
    catalog = load_catalog()
    send({"type": "ready", "protocol": PROTOCOL_VERSION, "indicators": sorted(catalog)})
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
        indicator_id = ""
        try:
            message = json.loads(payload)
            indicator_id = str(message.get("indicator_id", ""))
            kind = str(message.get("type", ""))
            if kind == "init":
                if indicator_id not in catalog:
                    raise ValueError(f"unknown indicator: {indicator_id}")
                entry = catalog[indicator_id]
                params = default_params(entry["manifest"])
                params.update(message.get("params") or {})
                states[indicator_id] = {
                    "params": params,
                    "manifest": entry["manifest"],
                    "compute": entry["compute"],
                    "bars": [],
                }
                send({"type": "initialized", "indicator_id": indicator_id})
            elif kind == "bars":
                if indicator_id not in states:
                    raise ValueError(f"indicator not initialized: {indicator_id}")
                state = states[indicator_id]
                incoming = [bar for bar in message.get("bars", []) if isinstance(bar, dict)]
                state["bars"] = (
                    incoming[-MAX_BARS:]
                    if message.get("reset")
                    else merge_bars(state["bars"], incoming)
                )
                send(
                    {
                        "type": "series",
                        "indicator_id": indicator_id,
                        "series": run_compute(indicator_id, state, state["bars"]),
                    }
                )
            elif kind == "stop":
                return 0
            else:
                raise ValueError(f"unknown request type: {kind}")
        except Exception as exc:  # noqa: BLE001 - report plugin errors over the protocol
            send(
                {
                    "type": "error",
                    "indicator_id": indicator_id,
                    "code": "indicator_error",
                    "message": str(exc),
                }
            )


if __name__ == "__main__":
    raise SystemExit(main())

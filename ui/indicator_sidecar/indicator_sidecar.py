#!/usr/bin/env python3
"""TQSDK Qt indicator sidecar.

One Python file is both metadata and implementation. The sidecar discovers
module files and reads either:

    MANIFEST = {"id": "rsi", ...}
    def compute(params, bars): ...

or the equivalent manifest() function:

    def manifest():
        return {"id": "rsi", ...}

No separate manifest.json is needed. The Qt UI receives the catalog from the
sidecar's ``ready`` message and never executes Python in the GUI thread.

Plugin discovery
----------------
``TQSDK_INDICATOR_PLUGIN_DIRS`` is a path-separator separated list. Each
directory may contain ``foo.py`` or ``foo/indicator.py`` (any .py file with
metadata works). Bundled indicators live in ``plugins/`` next to this script.

``compute(params, bars)`` must return ``{series_name: values}`` where each
values list has the same length as bars. ``None``/NaN values are filtered
before reaching the chart.

Wire format (both directions):
    4-byte big-endian payload length + UTF-8 compact JSON

Requests:
    {"type":"init","indicator_id":"ma","params":{"fast":5,"slow":20}}
    {"type":"bars","indicator_id":"ma","reset":true,"bars":[...]}
    {"type":"bars","indicator_id":"ma","reset":false,"bars":[...]}

Responses:
    {"type":"ready","protocol":1,"indicators":[{"id":"ma",...}]}
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

# 让用户目录插件也能 import indicator_lib。
_SIDECAR_ROOT = Path(__file__).resolve().parent
if str(_SIDECAR_ROOT) not in sys.path:
    sys.path.insert(0, str(_SIDECAR_ROOT))

PROTOCOL_VERSION = 2
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


def read_metadata(module: Any) -> dict[str, Any] | None:
    candidate = getattr(module, "manifest", None)
    if callable(candidate):
        candidate = candidate()
    if candidate is None:
        candidate = getattr(module, "MANIFEST", None)
    if callable(candidate):
        candidate = candidate()
    return candidate if isinstance(candidate, dict) else None


def validate_metadata(metadata: dict[str, Any], source: Path) -> dict[str, Any]:
    indicator_id = str(metadata.get("id", "")).strip()
    if not indicator_id:
        raise ValueError("manifest id is empty")
    panel = str(metadata.get("panel", "sub"))
    if panel not in ("main", "sub"):
        raise ValueError("manifest panel must be main or sub")
    series = [item for item in metadata.get("series", []) if isinstance(item, dict)]
    if not series or any(not str(item.get("name", "")).strip() for item in series):
        raise ValueError("manifest series must contain named series")

    params = [item for item in metadata.get("params", []) if isinstance(item, dict)]
    normalized = {
        "id": indicator_id,
        "name": str(metadata.get("name", indicator_id)),
        "version": str(metadata.get("version", "1.0")),
        "panel": panel,
        "params": params,
        "series": series,
        "source": str(source),
    }
    return normalized


def load_plugin(module_path: Path) -> dict[str, Any] | None:
    spec = importlib.util.spec_from_file_location(
        f"tqsdk_indicator_{module_path.stem}_{abs(hash(str(module_path)))}", module_path
    )
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load plugin module: {module_path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    metadata = read_metadata(module)
    if metadata is None:
        return None
    metadata = validate_metadata(metadata, module_path)
    compute = getattr(module, "compute", None)
    if not callable(compute):
        raise RuntimeError(f"plugin {metadata['id']} has no compute(params, bars)")
    return {"manifest": metadata, "compute": compute}


def candidate_files(directory: Path) -> list[Path]:
    if not directory.is_dir():
        return []
    files: list[Path] = []
    direct = directory / "indicator.py"
    if direct.is_file():
        files.append(direct)
    for child in sorted(directory.iterdir(), key=lambda item: item.name):
        if child.is_dir():
            files.extend(
                path for path in sorted(child.glob("*.py")) if not path.name.startswith("_")
            )
        elif child.suffix == ".py" and child.name != "indicator.py" and not child.name.startswith("_"):
            files.append(child)
    return files


def load_catalog() -> dict[str, dict[str, Any]]:
    catalog: dict[str, dict[str, Any]] = {}
    seen_files: set[str] = set()
    for directory in plugin_directories():
        for module_path in candidate_files(directory):
            resolved = str(module_path.resolve())
            if resolved in seen_files:
                continue
            seen_files.add(resolved)
            try:
                entry = load_plugin(module_path)
                if entry is None:
                    continue
                indicator_id = entry["manifest"]["id"]
                if indicator_id in catalog:
                    continue
                catalog[indicator_id] = entry
            except Exception as exc:  # noqa: BLE001 - one bad plugin must not kill others
                print(f"indicator plugin skipped: {module_path}: {exc}", file=sys.stderr)
    return catalog


def default_params(manifest: dict[str, Any]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for item in manifest.get("params", []):
        if isinstance(item, dict) and "key" in item:
            result[str(item["key"])] = item.get("default", 0)
    return result


def public_manifest(manifest: dict[str, Any]) -> dict[str, Any]:
    return {
        "id": manifest["id"],
        "name": manifest["name"],
        "version": manifest["version"],
        "panel": manifest["panel"],
        "params": manifest["params"],
        "series": manifest["series"],
    }


def merge_bars(existing: list[dict[str, Any]], incoming: list[dict[str, Any]]) -> list[dict[str, Any]]:
    by_time = {int(bar["datetime"]): bar for bar in existing}
    for bar in incoming:
        by_time[int(bar["datetime"])] = bar
    merged = sorted(by_time.values(), key=lambda bar: int(bar["datetime"]))
    return merged[-MAX_BARS:]


def build_series(
    manifest: dict[str, Any],
    datetimes: list[int],
    values_by_name: dict[str, Any],
) -> list[dict[str, Any]]:
    output: list[dict[str, Any]] = []
    for item in manifest.get("series", []):
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
                "panel": manifest.get("panel", "sub"),
                "points": points,
            }
        )
    return output


def run_compute(state: dict[str, Any], bars: list[dict[str, Any]]) -> list[dict[str, Any]]:
    manifest = state["manifest"]
    values = state["compute"](state["params"], bars)
    if not isinstance(values, dict):
        raise ValueError("plugin compute must return a dict of series")
    return build_series(manifest, [int(bar["datetime"]) for bar in bars], values)


def main() -> int:
    catalog = load_catalog()
    send(
        {
            "type": "ready",
            "protocol": PROTOCOL_VERSION,
            "indicators": [public_manifest(entry["manifest"]) for entry in catalog.values()],
        }
    )
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
                        "series": run_compute(state, state["bars"]),
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

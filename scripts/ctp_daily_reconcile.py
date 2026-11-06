#!/usr/bin/env python3
"""CTP 每日对账快照（只读）。

在交易日开盘前或收盘后运行，抓取 /readyz、/account、/positions、/orders、
/trades 并写入 artifacts。若提供 --previous，则比较持仓/账户/订单数量并输出
差异；不会下单、撤单或修改任何状态。

退出码：
  0  数据可用（ready 或结算预期断线）
  1  node/HTTP 失败，或非预期 not-ready
  2  参数/本地错误
"""
from __future__ import annotations

import argparse
import json
import urllib.error
import urllib.request
from datetime import datetime
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parent.parent


def http_json(base: str, path: str, timeout: float = 5.0) -> tuple[int, Any]:
    request = urllib.request.Request(base.rstrip("/") + path,
                                     headers={"Accept": "application/json"})
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            raw = response.read()
            return response.status, json.loads(raw) if raw else {}
    except urllib.error.HTTPError as exc:
        raw = exc.read()
        try:
            body = json.loads(raw) if raw else {}
        except json.JSONDecodeError:
            body = {"error": raw.decode("utf-8", "replace")}
        return exc.code, body
    except (urllib.error.URLError, TimeoutError, OSError) as exc:
        return 0, {"error": str(exc)}


def as_map(value: Any) -> dict[str, Any]:
    return value if isinstance(value, dict) else {}


def flatten_positions(body: Any) -> dict[str, dict[str, Any]]:
    positions = as_map(body).get("positions", {})
    return {str(k): v for k, v in as_map(positions).items() if isinstance(v, dict)}


def pos_key(position: dict[str, Any]) -> tuple[int, int]:
    return int(position.get("volume_long", 0)), int(position.get("volume_short", 0))


def compare(previous: dict[str, Any], current: dict[str, Any]) -> dict[str, Any]:
    prev_pos = previous.get("positions", {}) if isinstance(previous.get("positions"), dict) else {}
    cur_pos = current.get("positions", {}) if isinstance(current.get("positions"), dict) else {}
    changed = []
    for symbol in sorted(set(prev_pos) | set(cur_pos)):
        before = pos_key(prev_pos.get(symbol, {})) if symbol in prev_pos else (0, 0)
        after = pos_key(cur_pos.get(symbol, {})) if symbol in cur_pos else (0, 0)
        if before != after:
            changed.append({
                "symbol": symbol,
                "before": {"volume_long": before[0], "volume_short": before[1]},
                "after": {"volume_long": after[0], "volume_short": after[1]},
            })
    return {
        "balance_before": previous.get("balance"),
        "balance_after": current.get("balance"),
        "orders_before": previous.get("order_count"),
        "orders_after": current.get("order_count"),
        "trades_before": previous.get("trade_count"),
        "trades_after": current.get("trade_count"),
        "position_changes": changed,
    }


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--node-url", default="http://127.0.0.1:18081")
    p.add_argument("--out-dir", type=Path, default=ROOT / "artifacts" / "ctp_reconcile")
    p.add_argument("--previous", type=Path, default=None,
                   help="上次 reconcile 的 JSON 文件，可选")
    p.add_argument("--require-ready", action="store_true",
                   help="非 ready 一律失败（默认允许 expected_disconnect）")
    return p.parse_args()


def main() -> int:
    args = parse_args()
    args.out_dir.mkdir(parents=True, exist_ok=True)
    status, ready = http_json(args.node_url, "/readyz")
    _, account = http_json(args.node_url, "/account")
    _, positions = http_json(args.node_url, "/positions")
    _, orders = http_json(args.node_url, "/orders")
    _, trades = http_json(args.node_url, "/trades")

    td = as_map(ready).get("td", {}) if isinstance(ready, dict) else {}
    md = as_map(ready).get("md", {}) if isinstance(ready, dict) else {}
    payload: dict[str, Any] = {
        "ts": datetime.now().astimezone().isoformat(timespec="seconds"),
        "ready_http": status,
        "ready": bool(ready.get("ready")) if isinstance(ready, dict) else False,
        "phase": td.get("phase", "unknown"),
        "expected_disconnect": bool(td.get("expected_disconnect")),
        "md_connected": bool(md.get("connected")),
        "td_connected": bool(td.get("connected")),
        "balance": account.get("balance") if isinstance(account, dict) else None,
        "available": account.get("available") if isinstance(account, dict) else None,
        "margin": account.get("margin") if isinstance(account, dict) else None,
        "positions": flatten_positions(positions),
        "order_count": len(as_map(as_map(orders).get("orders"))),
        "trade_count": len(as_map(as_map(trades).get("trades"))),
        "http_errors": [],
    }
    for label, body in (("ready", ready), ("account", account), ("positions", positions),
                        ("orders", orders), ("trades", trades)):
        if not isinstance(body, dict) or body.get("error"):
            payload["http_errors"].append({"endpoint": label,
                                           "error": body.get("error") if isinstance(body, dict)
                                           else "invalid response"})

    path = args.out_dir / f"ctp_reconcile_{datetime.now().strftime('%Y%m%d_%H%M%S')}.json"
    path.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n",
                    encoding="utf-8")
    result: dict[str, Any] = {"file": str(path), "snapshot": {
        "ready": payload["ready"],
        "phase": payload["phase"],
        "expected_disconnect": payload["expected_disconnect"],
        "balance": payload["balance"],
        "positions": len(payload["positions"]),
        "orders": payload["order_count"],
        "trades": payload["trade_count"],
        "http_errors": payload["http_errors"],
    }}

    if args.previous is not None:
        try:
            previous = json.loads(args.previous.read_text(encoding="utf-8"))
            result["comparison"] = compare(previous, payload)
        except (OSError, json.JSONDecodeError) as exc:
            result["previous_error"] = str(exc)

    print(json.dumps(result, ensure_ascii=False, indent=2), flush=True)

    if payload["http_errors"]:
        return 1
    if not payload["ready"]:
        if args.require_ready or not payload["expected_disconnect"]:
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

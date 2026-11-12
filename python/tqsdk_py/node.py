"""tqsdk-node REST adapter shared by live data/account/trading adapters."""

from __future__ import annotations

import json
import os
import urllib.error
import urllib.parse
import urllib.request
from typing import Any

from .models import Bar


class NodeError(RuntimeError):
    def __init__(self, status: int, message: str, body: Any = None):
        super().__init__(f"HTTP {status}: {message}")
        self.status = status
        self.body = body


class NodeClient:
    """Small dependency-free REST client for tqsdk-node."""

    def __init__(
        self,
        base_url: str = "http://127.0.0.1:18081",
        account: str = "default",
        token: str = "",
        timeout: float = 15.0,
    ):
        self.base_url = base_url.rstrip("/")
        # Keep the account id separate from the account() REST method.
        self.account_id = account
        self.token = token or os.environ.get("TQSDK_SERVE_TOKEN", "")
        self.timeout = timeout

    def _url(self, path: str, account: bool = True) -> str:
        if not path.startswith("/"):
            path = "/" + path
        if account and "account=" not in path:
            sep = "&" if "?" in path else "?"
            path = f"{path}{sep}account={urllib.parse.quote(self.account_id)}"
        return self.base_url + path

    def _request(
        self,
        method: str,
        path: str,
        body: Any = None,
        *,
        account: bool = True,
        allow_error_status: bool = False,
    ) -> tuple[int, Any]:
        data = None
        headers = {"Accept": "application/json"}
        if self.token:
            headers["Authorization"] = f"Bearer {self.token}"
        if body is not None:
            headers["Content-Type"] = "application/json"
            data = json.dumps(body, ensure_ascii=False).encode("utf-8")
        request = urllib.request.Request(
            self._url(path, account=account), data=data, headers=headers, method=method
        )
        try:
            with urllib.request.urlopen(request, timeout=self.timeout) as response:
                raw = response.read()
                status = response.status
        except urllib.error.HTTPError as exc:
            raw = exc.read()
            status = exc.code
        try:
            parsed = json.loads(raw) if raw else {}
        except json.JSONDecodeError:
            parsed = raw.decode("utf-8", "replace")
        if status < 200 or status >= 300:
            if not allow_error_status:
                message = parsed.get("error") if isinstance(parsed, dict) else parsed
                raise NodeError(status, str(message), parsed)
        return status, parsed

    def readyz(self) -> tuple[int, dict[str, Any]]:
        status, body = self._request("GET", "/readyz", allow_error_status=True)
        return status, body if isinstance(body, dict) else {}

    def is_ready(self) -> bool:
        try:
            status, body = self.readyz()
            return status == 200 and bool(body.get("ready"))
        except Exception:
            return False

    def quote(self, symbol: str) -> dict[str, Any]:
        _, body = self._request("GET", f"/quote/{urllib.parse.quote(symbol)}")
        return body if isinstance(body, dict) else {}

    def quotes(self, symbols: list[str]) -> dict[str, Any]:
        query = urllib.parse.urlencode({"symbols": ",".join(symbols)})
        _, body = self._request("GET", f"/quotes?{query}")
        return body if isinstance(body, dict) else {}

    def kline(
        self,
        symbol: str,
        duration_ns: int,
        width: int = 200,
        adjust: str = "none",
    ) -> list[Bar]:
        query = f"?width={int(width)}"
        if adjust and adjust != "none":
            query += f"&adjust={urllib.parse.quote(adjust)}"
        path = f"/kline/{urllib.parse.quote(symbol)}/{int(duration_ns)}{query}"
        _, body = self._request("GET", path)
        bars = body.get("bars", []) if isinstance(body, dict) else []
        return [Bar.from_value(item) for item in bars]

    def account(self) -> dict[str, Any]:
        _, body = self._request("GET", "/account")
        return body if isinstance(body, dict) else {}

    def positions(self) -> dict[str, Any]:
        _, body = self._request("GET", "/positions")
        return body if isinstance(body, dict) else {}

    def orders(self) -> dict[str, Any]:
        _, body = self._request("GET", "/orders")
        return body if isinstance(body, dict) else {}

    def trades(self) -> dict[str, Any]:
        _, body = self._request("GET", "/trades")
        return body if isinstance(body, dict) else {}

    def tasks(self) -> dict[str, Any]:
        _, body = self._request("GET", "/tasks")
        return body if isinstance(body, dict) else {}

    def target_position(
        self,
        symbol: str,
        target_volume: int,
        mode: str = "ACTIVE",
        max_volume: int = 0,
    ) -> dict[str, Any]:
        body = {"symbol": symbol, "target_volume": int(target_volume), "mode": mode}
        if max_volume > 0:
            body["max_volume"] = int(max_volume)
        _, response = self._request("POST", "/task/target", body=body)
        return response if isinstance(response, dict) else {}

    def cancel_target(self, task_id: str) -> dict[str, Any]:
        path = f"/task/target/{urllib.parse.quote(task_id)}"
        _, response = self._request("DELETE", path)
        return response if isinstance(response, dict) else {}

    def insert_order(self, order: dict[str, Any]) -> dict[str, Any]:
        _, response = self._request("POST", "/order", body=order)
        return response if isinstance(response, dict) else {}


def net_position(position: dict[str, Any]) -> int:
    return int(position.get("volume_long", 0)) - int(position.get("volume_short", 0))


def entry_price(position: dict[str, Any]) -> float | None:
    if int(position.get("volume_long", 0)) > 0:
        value = position.get("open_price_long") or position.get("position_price_long")
        if value is not None:
            return float(value)
    if int(position.get("volume_short", 0)) > 0:
        value = position.get("open_price_short") or position.get("position_price_short")
        if value is not None:
            return float(value)
    return None

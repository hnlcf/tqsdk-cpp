#!/usr/bin/env python3
"""Generic backtest CLI: select any registered strategy at runtime."""

from __future__ import annotations

from tqsdk_py.cli import backtest_main


def main(argv: list[str] | None = None) -> int:
    return backtest_main(argv)


if __name__ == "__main__":
    raise SystemExit(main())

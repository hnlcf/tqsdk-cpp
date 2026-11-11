#!/usr/bin/env python3
"""Fail on likely credentials accidentally added to tracked files.

This is a guardrail, not a replacement for secret scanning in CI. It reports
only file/line/rule, never the matched value.
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ENV_KEYS = re.compile(
    r"^\s*(TQ_AUTH_USER|TQ_AUTH_PASSWORD|CTP_USER_ID|CTP_PASSWORD|"
    r"TQSDK_USERNAME|TQSDK_PASSWORD|TQSDK_SERVE_TOKEN)\s*=\s*(.*)$"
)
PLACEHOLDER = re.compile(r"your_|replace|example|change_me|<|\.\.\.", re.I)
TOKEN_PATTERNS = [
    ("private-key", re.compile(r"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----")),
    ("github-token", re.compile(r"gh[pousr]_[A-Za-z0-9_]{20,}")),
    ("openai-key", re.compile(r"sk-[A-Za-z0-9]{20,}")),
    ("aws-key", re.compile(r"AKIA[0-9A-Z]{16}")),
]


def tracked_files() -> list[Path]:
    raw = subprocess.check_output(["git", "-C", str(ROOT), "ls-files", "-z"])
    return [ROOT / item for item in raw.decode().split("\0") if item]


def unquote(value: str) -> str:
    value = value.strip()
    if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
        value = value[1:-1]
    return value


def scan() -> int:
    findings = 0
    for path in tracked_files():
        if not path.is_file():
            continue
        try:
            text = path.read_text(errors="replace")
        except OSError:
            continue
        relative = path.relative_to(ROOT)
        # Examples and documentation intentionally contain placeholders.
        example = relative.suffix == ".example" or "docs/" in relative.as_posix()
        for lineno, line in enumerate(text.splitlines(), 1):
            match = ENV_KEYS.match(line)
            if match and not example:
                value = unquote(match.group(2))
                if value and not PLACEHOLDER.search(value):
                    print(f"{relative}:{lineno}: possible hardcoded {match.group(1)}")
                    findings += 1
            for name, pattern in TOKEN_PATTERNS:
                if pattern.search(line):
                    print(f"{relative}:{lineno}: possible {name}")
                    findings += 1
    return findings


def main() -> int:
    findings = scan()
    if findings:
        print(f"check_secrets: FAIL ({findings} finding(s))", file=sys.stderr)
        return 1
    print("check_secrets: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

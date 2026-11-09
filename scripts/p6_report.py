#!/usr/bin/env python3
"""Aggregate P6 campaign artifacts into report.json/report.md."""
from __future__ import annotations

import argparse
import json
from datetime import datetime
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parent.parent


def read_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None


def read_rc(path: Path) -> int | None:
    try:
        return int(path.read_text(encoding="utf-8").strip())
    except (OSError, ValueError):
        return None


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--dir", type=Path, default=ROOT / "artifacts/p6_campaign_20261109")
    args = p.parse_args()
    d = args.dir
    d.mkdir(parents=True, exist_ok=True)
    data: dict[str, Any] = {"generated_at": datetime.now().astimezone().isoformat(timespec="seconds")}

    rcs = {path.stem: read_rc(path) for path in sorted(d.glob("*.rc"))}
    data["phase_rc"] = rcs

    p61_reports = []
    for path in sorted(d.glob("p61_week*/*.report.json")):
        report = read_json(path)
        if report is not None:
            p61_reports.append({"file": str(path), "monitor": report.get("monitor", {}),
                                "exit_code": report.get("exit_code")})
    data["p61_reports"] = p61_reports

    p62 = {}
    for name in ("p62_acceptance_readonly.json", "p62_acceptance_write.json"):
        value = read_json(d / name)
        if value is not None:
            p62[name] = value
    data["p62_acceptance"] = p62
    data["p62_notes"] = {
        path.name: path.read_text(encoding="utf-8", errors="replace").strip()
        for path in sorted(d.glob("p62_*skip*.txt"))
    }
    recon = []
    for path in sorted(d.glob("p62_reconcile*/ctp_reconcile_*.json")):
        value = read_json(path)
        if isinstance(value, dict):
            recon.append({"file": str(path), "ready": value.get("ready"),
                          "phase": value.get("phase"),
                          "expected_disconnect": value.get("expected_disconnect"),
                          "positions": len(value.get("positions", {})),
                          "orders": value.get("order_count")})
    data["p62_reconcile"] = recon

    p63 = {
        "soak_log_tail": "",
        "asan_log_tail": "",
        "fuzz_note": "",
    }
    soak = d / "p63_soak_72h.out"
    if soak.exists():
        p63["soak_log_tail"] = soak.read_text(encoding="utf-8", errors="replace")[-4000:]
    p63["soak_files"] = [str(path) for path in sorted(d.glob("p63_soak_72h*"))]
    asan = d / "p63_asan_build.log"
    if asan.exists():
        p63["asan_log_tail"] = asan.read_text(encoding="utf-8", errors="replace")[-4000:]
    fuzz = d / "p63_fuzz_skip.txt"
    if fuzz.exists():
        p63["fuzz_note"] = fuzz.read_text(encoding="utf-8", errors="replace")
    data["p63"] = p63

    (d / "report.json").write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n",
                                   encoding="utf-8")

    lines = ["# P6 campaign report", "",
             f"- Generated: {data['generated_at']}",
             f"- Directory: `{d}`", "",
             "## Phase return codes", ""]
    for name, rc in rcs.items():
        lines.append(f"- `{name}`: `{rc}`")
    lines += ["", "## P6.1", ""]
    if p61_reports:
        for item in p61_reports:
            m = item.get("monitor", {})
            lines.append(f"- `{Path(item['file']).name}` samples={m.get('samples')} "
                         f"expected_gaps={m.get('expected_gaps')} "
                         f"unexpected_gaps={m.get('unexpected_gaps')} fatal={m.get('fatal')!r}")
    else:
        lines.append("- no P6.1 report yet")
    lines += ["", "## P6.2", ""]
    if p62:
        for name, value in p62.items():
            if isinstance(value, dict):
                lines.append(f"- `{name}`: {json.dumps({k: value.get(k) for k in ('ok','passed','failed','summary') if k in value}, ensure_ascii=False)}")
    if recon:
        lines.append(f"- reconciliation snapshots: {len(recon)}")
        for item in recon[-3:]:
            lines.append(f"  - {Path(item['file']).name}: ready={item['ready']} phase={item['phase']} "
                         f"expected={item['expected_disconnect']} positions={item['positions']} orders={item['orders']}")
    if data["p62_notes"]:
        for name, note in data["p62_notes"].items():
            lines.append(f"- note `{name}`: {note}")
    if not p62 and not recon:
        lines.append("- no P6.2 artifacts yet")
    lines += ["", "## P6.3", ""]
    if p63.get("soak_files"):
        lines.append("- files: " + ", ".join(f"`{Path(x).name}`" for x in p63["soak_files"]))
    lines.append("### 72h SimTd soak tail")
    lines.append("```text")
    lines.append(p63["soak_log_tail"].strip() or "no soak log yet")
    lines.append("```")
    lines.append("### ASan tail")
    lines.append("```text")
    lines.append(p63["asan_log_tail"].strip() or "no ASan log yet")
    lines.append("```")
    if p63["fuzz_note"]:
        lines += ["### Fuzz note", "", p63["fuzz_note"]]
    (d / "report.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(json.dumps({"report": str(d / "report.md"), "phase_rc": rcs}, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

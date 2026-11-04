"""EMA indicator plugin."""

from indicator_lib import ema, param, series_def

MANIFEST = {
    "id": "ema",
    "name": "EMA",
    "version": "1.0",
    "panel": "main",
    "params": [
        param("fast", "快线", 12, 1, 500),
        param("slow", "慢线", 26, 1, 1000),
    ],
    "series": [
        series_def("ema_fast", "EMA快", "orange"),
        series_def("ema_slow", "EMA慢", "purple"),
    ],
}


def compute(params, bars):
    closes = [float(bar["close"]) for bar in bars]
    fast = max(1, int(params.get("fast", 12)))
    slow = max(1, int(params.get("slow", 26)))
    return {"ema_fast": ema(closes, fast), "ema_slow": ema(closes, slow)}

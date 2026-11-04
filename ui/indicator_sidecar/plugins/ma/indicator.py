"""MA indicator plugin."""

from indicator_lib import param, series_def, sma

MANIFEST = {
    "id": "ma",
    "name": "MA",
    "version": "1.0",
    "panel": "main",
    "params": [
        param("fast", "快线", 5, 1, 500),
        param("slow", "慢线", 20, 1, 1000),
    ],
    "series": [
        series_def("ma_fast", "MA快", "gold"),
        series_def("ma_slow", "MA慢", "blue"),
    ],
}


def compute(params, bars):
    closes = [float(bar["close"]) for bar in bars]
    fast = max(1, int(params.get("fast", 5)))
    slow = max(1, int(params.get("slow", 20)))
    return {"ma_fast": sma(closes, fast), "ma_slow": sma(closes, slow)}

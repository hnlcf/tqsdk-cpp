"""MACD indicator plugin."""

from indicator_lib import macd, param, series_def

MANIFEST = {
    "id": "macd",
    "name": "MACD",
    "version": "1.0",
    "panel": "sub",
    "params": [
        param("fast", "快线", 12, 1, 500),
        param("slow", "慢线", 26, 1, 1000),
        param("signal", "信号", 9, 1, 500),
    ],
    "series": [
        series_def("macd", "MACD", "gold"),
        series_def("signal", "SIGNAL", "blue"),
        series_def("histogram", "HIST", "muted"),
    ],
}


def compute(params, bars):
    closes = [float(bar["close"]) for bar in bars]
    fast = max(1, int(params.get("fast", 12)))
    slow = max(1, int(params.get("slow", 26)))
    signal_period = max(1, int(params.get("signal", 9)))
    macd_line, signal_line, histogram = macd(closes, fast, slow, signal_period)
    return {"macd": macd_line, "signal": signal_line, "histogram": histogram}

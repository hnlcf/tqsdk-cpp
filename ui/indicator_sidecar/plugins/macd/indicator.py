"""MACD indicator plugin."""

MANIFEST = {
    "id": "macd",
    "name": "MACD",
    "version": "1.0",
    "panel": "sub",
    "params": [
        {"key": "fast", "label": "快线", "type": "int", "default": 12, "min": 1, "max": 500},
        {"key": "slow", "label": "慢线", "type": "int", "default": 26, "min": 1, "max": 1000},
        {"key": "signal", "label": "信号", "type": "int", "default": 9, "min": 1, "max": 500},
    ],
    "series": [
        {"name": "macd", "label": "MACD", "color": "#f0c75e"},
        {"name": "signal", "label": "SIGNAL", "color": "#53b7ff"},
        {"name": "histogram", "label": "HIST", "color": "#9aa4b2"},
    ],
}


def ema(values, period):
    period = max(1, int(period))
    result = [None] * len(values)
    alpha = 2.0 / (period + 1.0)
    previous = None
    for i, value in enumerate(values):
        if value is None:
            continue
        previous = value if previous is None else alpha * value + (1.0 - alpha) * previous
        result[i] = previous
    return result


def compute(params, bars):
    closes = [float(bar["close"]) for bar in bars]
    fast = max(1, int(params.get("fast", 12)))
    slow = max(1, int(params.get("slow", 26)))
    signal_period = max(1, int(params.get("signal", 9)))
    fast_ema = ema(closes, fast)
    slow_ema = ema(closes, slow)
    macd_values = [None if f is None or s is None else f - s for f, s in zip(fast_ema, slow_ema)]
    signal_values = ema(macd_values, signal_period)
    histogram_values = [
        None if m is None or s is None else m - s for m, s in zip(macd_values, signal_values)
    ]
    return {"macd": macd_values, "signal": signal_values, "histogram": histogram_values}

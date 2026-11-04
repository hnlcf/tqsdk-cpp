"""EMA indicator plugin."""

MANIFEST = {
    "id": "ema",
    "name": "EMA",
    "version": "1.0",
    "panel": "main",
    "params": [
        {"key": "fast", "label": "快线", "type": "int", "default": 12, "min": 1, "max": 500},
        {"key": "slow", "label": "慢线", "type": "int", "default": 26, "min": 1, "max": 1000},
    ],
    "series": [
        {"name": "ema_fast", "label": "EMA快", "color": "#ffb86b"},
        {"name": "ema_slow", "label": "EMA慢", "color": "#c792ea"},
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
    return {"ema_fast": ema(closes, fast), "ema_slow": ema(closes, slow)}

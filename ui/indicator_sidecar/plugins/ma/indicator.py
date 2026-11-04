"""MA indicator plugin: metadata and computation live in one file."""

MANIFEST = {
    "id": "ma",
    "name": "MA",
    "version": "1.0",
    "panel": "main",
    "params": [
        {"key": "fast", "label": "快线", "type": "int", "default": 5, "min": 1, "max": 500},
        {"key": "slow", "label": "慢线", "type": "int", "default": 20, "min": 1, "max": 1000},
    ],
    "series": [
        {"name": "ma_fast", "label": "MA快", "color": "#f0c75e"},
        {"name": "ma_slow", "label": "MA慢", "color": "#53b7ff"},
    ],
}


def sma(values, period):
    period = max(1, int(period))
    result = [None] * len(values)
    total = 0.0
    valid = 0
    for i, value in enumerate(values):
        if value is None:
            continue
        total += value
        valid += 1
        if i >= period and values[i - period] is not None:
            total -= values[i - period]
            valid -= 1
        if valid == period:
            result[i] = total / period
    return result


def compute(params, bars):
    closes = [float(bar["close"]) for bar in bars]
    fast = max(1, int(params.get("fast", 5)))
    slow = max(1, int(params.get("slow", 20)))
    return {"ma_fast": sma(closes, fast), "ma_slow": sma(closes, slow)}

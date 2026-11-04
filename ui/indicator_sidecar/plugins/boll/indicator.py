"""BOLL indicator plugin."""

from indicator_lib import boll, param, series_def

MANIFEST = {
    "id": "boll",
    "name": "BOLL",
    "version": "1.0",
    "panel": "main",
    "params": [
        param("period", "周期", 20, 2, 500),
        param("mult", "倍数", 2.0, 0.1, 10.0, kind="float", decimals=2),
    ],
    "series": [
        series_def("boll_mid", "中轨", "gold"),
        series_def("boll_upper", "上轨", "red"),
        series_def("boll_lower", "下轨", "teal"),
    ],
}


def compute(params, bars):
    closes = [float(bar["close"]) for bar in bars]
    period = max(1, int(params.get("period", 20)))
    mult = max(0.0, float(params.get("mult", 2.0)))
    middle, upper, lower = boll(closes, period, mult)
    return {"boll_mid": middle, "boll_upper": upper, "boll_lower": lower}

"""EMA indicator plugin."""

from indicator_lib import ema, param, series_def

MANIFEST = {
    "id": "ema",
    "name": "EMA",
    "version": "1.0",
    "panel": "main",
    "params": [
        param("N1", "N1", 5, 1, 1000),
        param("N2", "N2", 10, 1, 1000),
        param("N3", "N3", 20, 1, 1000),
        param("N4", "N4", 60, 1, 1000),
        param("N5", "N5", 120, 1, 1000),
        param("N6", "N6", 200, 1, 1000),
    ],
    "series": [
        series_def("ema_n1", "EMA N1", "white"),
        series_def("ema_n2", "EMA N2", "yellow"),
        series_def("ema_n3", "EMA N3", "purple"),
        series_def("ema_n4", "EMA N4", "green"),
        series_def("ema_n5", "EMA N5", "blue"),
        series_def("ema_n6", "EMA N6", "red"),
    ],
}


def compute(params, bars):
    closes = [float(bar["close"]) for bar in bars]
    ema_n1 = max(1, int(params.get("N1", 5)))
    ema_n2 = max(1, int(params.get("N2", 10)))
    ema_n3 = max(1, int(params.get("N3", 20)))
    ema_n4 = max(1, int(params.get("N4", 60)))
    ema_n5 = max(1, int(params.get("N5", 120)))
    ema_n6 = max(1, int(params.get("N6", 200)))
    return {
        "ema_n1": ema(closes, ema_n1),
        "ema_n2": ema(closes, ema_n2),
        "ema_n3": ema(closes, ema_n3),
        "ema_n4": ema(closes, ema_n4),
        "ema_n5": ema(closes, ema_n5),
        "ema_n6": ema(closes, ema_n6),
    }

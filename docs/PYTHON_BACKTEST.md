# Python 使用 C++ 回测引擎

## 1. 结论

项目已有 C++ `BacktestEngine`：

```text
include/tqsdk/backtest/backtest_engine.h
src/backtest/backtest_engine.cpp
```

现在新增：

```text
include/tqsdk/backtest/backtest_c_api.h
src/backtest/backtest_c_api.cpp
python/tqsdk_backtest.py
```

- C++ 仍然是唯一的回测/撮合实现；
- C ABI 只负责把 bar、配置、成交和统计跨语言传递；
- Python 通过 `ctypes` 加载 `libtqsdk_backtest.so`；
- Python 策略回调内部调用 `buy_open/sell_open/buy_close/sell_close/target_position`；
- 不需要 pybind11，也不需要 Python 开发头文件。

## 2. 构建

```bash
cmake --build build --target tqsdk_backtest
```

产物：

```text
build/libtqsdk_backtest.so
```

`tqsdk_backtest` 会自动链接 `libtqsdk_core.so`，构建目录的 RPATH 已设为 `$ORIGIN`。

安装包中会安装：

```text
/usr/lib/libtqsdk_backtest.so
/usr/share/tqsdk/python/tqsdk_backtest.py
```

## 3. 最小示例

```python
from tqsdk_backtest import Bar, BacktestConfig, run_backtest

config = BacktestConfig(
    initial_balance=1_000_000,
    volume_multiple=10,
    margin_rate=0.10,
    commission_per_lot=1.0,
)

bars = [
    Bar(1, 100, 101, 99, 100, 10_000),
    Bar(2, 110, 111, 109, 110, 10_000),
    Bar(3, 120, 121, 119, 120, 10_000),
]


def strategy(ctx, bar):
    if bar.datetime == 1:
        ctx.buy_open(2)
    elif bar.datetime == 2:
        ctx.target_position(0)


result = run_backtest(bars, strategy, config)
print(result.stats.final_equity)
print(result.stats.max_drawdown_pct)
for fill in result.fills:
    print(fill.datetime, fill.direction, fill.offset, fill.price, fill.volume)
print(result.equity_curve[-1])
```

运行：

```bash
PYTHONPATH=python \
TQSDK_BACKTEST_LIB=build/libtqsdk_backtest.so \
python3 python/examples/ema_backtest.py
```

## 4. API

### `BacktestConfig`

对应 C++ `BacktestConfig`：

- `initial_balance`
- `volume_multiple`
- `margin_rate`
- `commission_rate`
- `commission_per_lot`
- `execute_on_bar_close`

### `Bar`

字段：

```text
datetime, open, high, low, close, volume
```

支持从 `Bar`、dict、5/6 元组或带同名属性的对象转换。

### `Context`

策略回调内有效：

```text
ctx.position()
ctx.equity()
ctx.buy_open(volume)
ctx.sell_open(volume)
ctx.buy_close(volume)
ctx.sell_close(volume)
ctx.target_position(target)
```

`ctx` 只在回调期间有效，不能保存到回调外。

### `BacktestResult`

- `stats`: 最终权益、盈亏、手续费、最大回撤、成交数、最终持仓等；
- `fills`: 每笔成交；
- `equity_curve`: Python 绑定在每个 bar 回调后通过 `ctx.equity()` 记录的权益曲线。

### 保证金/手数

C++ 引擎执行显式手数，不替策略决定仓位。Python 提供：

```python
lots = lots_for_margin(ctx.equity(), bar.close, config, allocation=0.30)
ctx.target_position(lots)
```

公式：

```text
floor(equity * allocation / (price * volume_multiple * margin_rate))
```

这和你当前的十年日线回测口径一致：**30% 权益作为初始保证金，整数手，杠杆由保证金率决定**。

## 5. 引擎语义与限制

- 每个 bar 先 mark-to-market，再调用策略回调；
- `execute_on_bar_close=true` 时成交价为当前 bar 收盘，否则为开盘；
- 支持多空双向、开平仓、目标净持仓；
- 引擎支持 bar 回测和 tick 回测；当前 Python C ABI 先暴露 bar 回测，tick 绑定可后续增加；
- 引擎**不自动计算保证金、强平或仓位**：由 Python 策略通过 `lots_for_margin` 和 `target_position` 实现；
- `equity_curve` 是回测结束后由 Python 侧记录的，不是 C++ 另存的大数组；
- 回调异常会通过 `RuntimeError` 抛回 Python，C++ 不会吞掉。

## 6. 测试

```bash
PYTHONPATH=python \
TQSDK_BACKTEST_LIB=build/libtqsdk_backtest.so \
python3 -m unittest discover -s python/tests -v
```

CTest 也会自动注册 `PythonTests`（构建 Python 解释器可用时）。

覆盖：

- 多头开平；
- 目标净持仓/空头；
- 手续费；
- 上下文 position/equity；
- Python 异常传播；
- dict bar 输入。

## 7. 实时策略

实时 node -> CTP 策略适配器见 [PYTHON_LIVE_TRADING.md](PYTHON_LIVE_TRADING.md)：

```text
python/tqsdk_strategy.py  共享 EMA60/Donchian22/ATR 策略
python/tqsdk_live.py      node REST + TargetPosTask + CTP
```

## 8. 和现有 Python 回测脚本的关系

现有 `scripts/backtest_daily.py` 是独立的研究脚本，优势是十年级数据清洗、报告和 SVG 图表；它不依赖 C++ 回测器。

现在有两条可结合路径：

1. **研究/报告**：继续使用 `scripts/backtest_daily.py`；
2. **策略执行验证**：Python 策略通过 `tqsdk_backtest` 调用 C++ `BacktestEngine`，确保实际撮合/成交/手续费链路和研究脚本的规则一致。

后续如果需要把 `backtest_daily` 的指标、数据加载和报告直接接到 C++ 引擎，可以复用 `python/tqsdk_backtest.py` 的 `Bar`/`BacktestConfig` 接口，而不需要重写 C++。

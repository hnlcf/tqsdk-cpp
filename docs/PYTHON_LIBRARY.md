# Python 策略库抽象层

## 1. 目录

```text
python/tqsdk_py/
├── models.py        Bar / ContractSpec / Signal / StrategyContext
├── data.py          BarSource、JSON/Node/内存数据适配器
├── strategy.py      Strategy 接口/基类（无具体策略）
├── registry.py      策略注册表/装饰器
├── manager.py       StrategyManager：自动发现 strategies/
├── indicators.py    EMA/ATR/Donchian 等通用指标
├── strategies/      具体策略实现（ema_donchian.py）
├── cli.py           通用 backtest/live CLI（--strategy）
├── signals.py       SignalConsumer、企业微信、HTTP webhook、组合消费者
├── execution.py     TargetExecutor、DryRun、Node TargetPosTask
├── backtest.py      C++ BacktestEngine 适配器
├── live.py          LiveRunner：数据/策略/信号/执行组合
├── node.py          tqsdk-node REST 客户端
└── __init__.py      公共导出
```

兼容入口仍保留：

```text
python/tqsdk_backtest.py   C++ ctypes 低层接口
python/tqsdk_strategy.py   策略旧导入名
python/tqsdk_live.py       live CLI/旧导入名
python/tqsdk_backtest_cli.py 通用回测 CLI
```

## 2. 数据适配层

统一接口：

```python
from tqsdk_py import BarSource, JsonBarSource, NodeBarSource, InMemoryBarSource

bars = JsonBarSource("artifacts/tq_daily").load("KQ.i@DCE.jm", 86_400_000_000_000)
```

实现：

- `JsonBarSource`：研究 JSON 文件/目录；
- `NodeBarSource`：包装 node REST，live 预热和轮询；
- `InMemoryBarSource`：测试、示例、策略调试；
- `closed_bars()`：live 去掉可能未收盘的最后一根。

## 3. 策略接口

```python
from tqsdk_py import Strategy, Signal, StrategyContext

class MyStrategy(Strategy):
    def on_bar(self, ctx: StrategyContext) -> Signal | None:
        # ctx.bars / ctx.position / ctx.equity / ctx.spec / ctx.price
        ...
```

`Signal` 是目标净持仓：

```text
target_volume > 0  目标净多
target_volume = 0  平仓
target_volume < 0  目标净空
```

策略只在目标改变时返回 `Signal`；回测和 live 使用同一个接口。

具体策略放在 `python/tqsdk_py/strategies/`，通过注册表自动发现：

```python
from tqsdk_py import default_manager

manager = default_manager()
print(manager.names())          # ['ema_donchian']
strategy = manager.create(
    "ema_donchian",              # 或别名 ema / ema60
    spec=spec,
    symbol="SHFE.rb2701",
    params={"ema_period": 60, "allocation": 0.30},
)
```

策略文件只写策略逻辑：

```python
from tqsdk_py import Strategy, register_strategy

@register_strategy("my_strategy", defaults={"period": 20})
class MyStrategy(Strategy):
    def __init__(self, *, spec, symbol="", duration_ns=0, period=20):
        ...
    def on_bar(self, ctx):
        ...
```

`StrategyManager` 会扫描 `tqsdk_py.strategies` 下所有模块，新增策略通常不需要改
框架或 CLI。

## 4. 信号消费者

统一接口：

```python
class SignalConsumer:
    def consume(self, signal: Signal, context: StrategyContext | None = None) -> None: ...
```

内置：

- `NullConsumer`
- `LoggingConsumer`
- `MemoryConsumer`（测试/回放）
- `WeComConsumer`（企业微信群机器人 webhook）
- `HttpWebhookConsumer`（通用 JSON webhook）
- `CompositeConsumer`（顺序 fan-out，默认隔离通知失败）
- `OrderExecutorConsumer`（把 consumer 适配为 TargetExecutor 调用）

### 企业微信

```python
from tqsdk_py import WeComConsumer, LoggingConsumer, SignalDispatcher

notifiers = [
    LoggingConsumer(),
    WeComConsumer(os.environ["TQSDK_WECOM_WEBHOOK"]),
]
```

企业微信 URL 是敏感信息，示例只从环境变量读取，不写死、不打印。

### 组合消费者

```python
from tqsdk_py import CompositeConsumer, MemoryConsumer

composite = CompositeConsumer([MemoryConsumer(), WeComConsumer(webhook)])
composite.consume(signal, context)
# composite.errors 可查看通知失败；默认不会阻断其他 consumer
```

### 通知 + 下单

```python
from tqsdk_py import SignalDispatcher, NodeTargetExecutor, WeComConsumer

dispatcher = SignalDispatcher(
    notifiers=[WeComConsumer(webhook)],
    executor=NodeTargetExecutor(node, max_volume=1),
)
result = dispatcher.dispatch(signal, context)
```

执行器是独立接口：

```python
class TargetExecutor:
    def execute(self, signal, context=None) -> ExecutionResult: ...
```

因此可以替换：

- `DryRunExecutor`：dry-run；
- `NodeTargetExecutor`：node `/task/target`；
- 自己的执行器：检查风控、拆分订单、记录审计等。

## 5. 回测

```python
from tqsdk_py import BacktestRunner, BacktestSettings, JsonBarSource, default_manager
from tqsdk_py import load_specs, resolve_spec

specs = load_specs("config/daily_backtest_specs.json")
spec = resolve_spec(specs, "SHFE.rb2701")
strategy = default_manager().create(
    "ema_donchian", spec=spec, symbol="SHFE.rb2701"
)

runner = BacktestRunner(
    JsonBarSource("artifacts/tq_daily"),
    strategy,
    spec,
    BacktestSettings(initial_balance=1_000_000),
    notifiers=[LoggingConsumer()],
)
result = runner.run("KQ.i@SHFE.rb")
print(result.stats)
```

通用 CLI：

```bash
PYTHONPATH=python TQSDK_BACKTEST_LIB=build/libtqsdk_backtest.so \
python3 python/tqsdk_backtest_cli.py \
  --strategy ema_donchian \
  --param ema_period=60 \
  --data-dir artifacts/tq_daily \
  --symbol KQ.i@SHFE.rb \
  --list-strategies
```

`BacktestRunner` 调用 C++ `BacktestEngine`（通过 `tqsdk_backtest` ctypes 绑定），
并复用同一套 `Strategy.on_bar` / `Signal` 接口。

> 精确的十年多品种研究报告仍由 `scripts/backtest_daily.py` 生成；它包含
> 逐 bar 的 ATR 盘中止损和报告生成。`BacktestRunner` 是通用库适配层，默认
> 按 C++ 引擎的 bar close 成交，文档中明确这是不同的执行模型。

## 6. 实时

```python
from tqsdk_py import NodeClient, NodeBarSource, LiveRunner, LiveConfig

runner = LiveRunner(
    node=node,
    data_source=NodeBarSource(node),
    strategy=strategy,
    spec=spec,
    dispatcher=dispatcher,
    config=LiveConfig(symbol="SHFE.rb2701", max_cycles=1),
)
runner.run()
```

CLI：

```bash
PYTHONPATH=python \
python3 python/tqsdk_live.py \
  --strategy ema_donchian \
  --param ema_period=60 \
  --node-url http://127.0.0.1:18081 \
  --symbol SHFE.rb2701 \
  --execute --max-volume 1
```

查看已注册策略：

```bash
PYTHONPATH=python python3 python/tqsdk_live.py --list-strategies
```

不传 `--execute` 时使用 `DryRunExecutor`，但企业微信/日志 consumer 仍会收到信号。

`TQSDK_WECOM_WEBHOOK` 存在时自动加入企业微信通知。

## 7. 策略注册与运行选择

```bash
# 列出注册表
PYTHONPATH=python python3 python/tqsdk_live.py --list-strategies

# 运行时选择策略
PYTHONPATH=python python3 python/tqsdk_live.py --strategy ema_donchian --param ema_period=60 ...
PYTHONPATH=python TQSDK_BACKTEST_LIB=build/libtqsdk_backtest.so \
  python3 python/tqsdk_backtest_cli.py --strategy ema_donchian ...
```

新增策略：在 `python/tqsdk_py/strategies/` 放置模块，使用
`@register_strategy("name", defaults={...})` 装饰 Strategy 子类；
`StrategyManager` 会自动 import 该目录下的模块。策略只实现 `on_bar()`，
不需要写 CLI、node、通知或回测启动代码。

## 8. 边界

- `LiveRunner` 当前使用 REST 轮询，适合日线/4h；不是 tick 级；
- node 的 `/readyz` 不 ready 时不发信号/订单；
- 通知失败默认隔离，订单执行失败不吞；
- `TargetPosTask` 是异步的，下一轮读取 `/positions` 对账；
- 一个进程建议控制一个 account/symbol/strategy；
- 实盘先 dry-run，并人工确认已有持仓归属。

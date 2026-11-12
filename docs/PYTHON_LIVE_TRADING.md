# Python 实时 CTP 策略

## 1. 总体链路

```text
CTP 前置
  -> tqsdk-gw（CTP Td：登录、报单、撤单、查询）
  -> tqsdk-node（Tq MD + DataManager + REST/WS）
  -> Python strategy（指标、信号、仓位、风控）
```

Python **不直接连接 CTP，也不持有 CTP 密码**。CTP 凭据只在 `tqsdk-gw`
配置和本地 env 文件中；Python 只通过 node 的 HTTP JSON 接口和 WebSocket
访问行情/账户/订单，并通过 `TargetPosTask` 表达目标净持仓。

## 2. 启动 CTP 服务

准备本地配置：

```text
config/live.local.env   # 0600，不提交 Git
```

启动：

```bash
./scripts/run_live.sh --no-ui
```

检查：

```bash
curl -s http://127.0.0.1:18081/readyz
```

只有当 body 中 `ready:true` 且 `td.connected/ready` 正常时才允许写操作。
CTP 结算窗口可能返回 HTTP 503，这是正常的，不要据此认为进程崩溃。

## 3. Python 策略模块

```text
python/tqsdk_py/strategies/  具体策略实现和注册
python/tqsdk_py/manager.py   策略管理器/注册表
python/tqsdk_py/cli.py       通用 backtest/live CLI
python/tqsdk_live.py         兼容入口
python/tqsdk_backtest_cli.py 通用回测 CLI
scripts/backtest_daily.py    十年研究回测/报告
```

内置 `ema_donchian` 的规则和正式回测一致：

- 多头：`close > EMA60` 且突破前 22 根 Donchian 上轨；
- 空头：`close < EMA60` 且跌破前 22 根 Donchian 下轨；
- 初始止损：入场价 ∓ `2 × ATR14`；
- 退出：收盘跌破/突破通道或 EMA；
- 仓位：`equity × 30%` 作为初始保证金，整数手；
- 多空对称。

## 4. 常用命令

### Dry-run：只打印目标，不发单

```bash
PYTHONPATH=python python3 python/tqsdk_live.py \
  --strategy ema_donchian \
  --param ema_period=60 \
  --node-url http://127.0.0.1:18081 \
  --account default \
  --symbol SHFE.rb2701 \
  --duration-ns 86400000000000 \
  --poll-interval 5
```

### 明确允许真实提交

```bash
PYTHONPATH=python python3 python/tqsdk_live.py \
  --strategy ema_donchian \
  --param ema_period=60 \
  --node-url http://127.0.0.1:18081 \
  --account default \
  --symbol SHFE.rb2701 \
  --duration-ns 86400000000000 \
  --execute --max-volume 1
```

查看所有已注册策略：

```bash
PYTHONPATH=python python3 python/tqsdk_live.py --list-strategies
```

`--execute` 是显式安全开关；没有它绝不会 POST `/task/target`。

### 认证

```bash
export TQSDK_SERVE_TOKEN='...'
```

`NodeClient` 会带：

```text
Authorization: Bearer <token>
```

也可以显式传 `--token`。不要把 token 写进命令历史或仓库。

## 5. 行情/交易接口

Python `NodeClient` 使用：

```text
GET  /readyz
GET  /quote/{symbol}
GET  /quotes?symbols=...
GET  /kline/{symbol}/{duration_ns}?width=200
GET  /account?account=...
GET  /positions?account=...
GET  /orders?account=...
GET  /trades?account=...
GET  /tasks?account=...
POST /task/target?account=...
POST /order?account=...
DELETE /order/{id}
DELETE /task/target/{id}
```

仓位目标：

```json
{
  "symbol": "SHFE.rb2701",
  "target_volume": 1,
  "mode": "ACTIVE",
  "max_volume": 1
}
```

`target_volume`：

- `+N`：目标净多 N 手；
- `-N`：目标净空 N 手；
- `0`：平到 0。

策略只发**目标净持仓变化**，重复轮询不会重复发同一个 target。

## 6. 实时循环行为

`LiveRunner.step()`：

1. 检查 `/readyz`；
2. 从 `/kline` 拉最近 bars；
3. 去掉可能未收盘的最后一根；
4. 从 `/positions` 读取当前净持仓和开仓价；
5. 从 `/account` 读取 balance；
6. 从 `/quote` 读取最新价，用于 ATR 止损监控；
7. 调用注册策略的 `Strategy.on_bar(context)`；
8. 把策略产出的通用 `Signal` 交给 `SignalDispatcher`：
   - notifier/组合 consumer（日志、企业微信、webhook）先通知；
   - 可选 `TargetExecutor` 再提交 `/task/target`；
9. 仅当目标净持仓与当前持仓不同才执行/通知。

目标任务提交后，实际成交/持仓仍由 node 与 CTP 处理；Python 不假设立即成交，
下一轮重新读取账户/持仓。

## 7. 为什么这里用 REST 轮询

EMA60/Donchian22 是日线/4h 策略，信号在 closed bar 上产生，控制延迟不需要
逐 tick 处理。REST 轮询有几个好处：

- 不需要维护 WS 增量重连状态；
- 每次决策都从 node 重新取快照，天然对账；
- 日线/4h 策略每 5–15 秒轮询足够。

如果需要 tick/盘口级策略，再使用 node `/stream`：

```json
{"op":"subscribe","symbols":["SHFE.rb2701"],
 "klines":[{"symbol":"SHFE.rb2701","duration_ns":14400000000000,"view_width":200}],
 "topics":["account","orders","trades","positions"]}
```

## 8. 安全与边界

- 当前只支持单 SimNow/CTP 账户；不能声称双账户隔离通过；
- 策略进程启动时，如果已有持仓，会从 `/positions` 读取并把开仓价作为
  stop 初始化参考；人工必须确认这个已有持仓是否属于本策略；
- 目标任务是异步的，Python 的下一轮状态以 `/positions` 为准；
- 不建议多个 Python 进程同时控制同一 account/symbol；
- 需要额外风控时，先配置 node RiskManager，再加 Python 侧资金/日内损失限制；
- CTP 不可交易（结算/断线）时，`LiveRunner` 只记录 not-ready，不发单。

## 9. 通知与下单组合

具体可复用接口见 [PYTHON_LIBRARY.md](PYTHON_LIBRARY.md)。

```python
from tqsdk_py import (
    NodeTargetExecutor, SignalDispatcher, WeComConsumer, LoggingConsumer
)

dispatcher = SignalDispatcher(
    notifiers=[
        LoggingConsumer(),
        WeComConsumer(os.environ["TQSDK_WECOM_WEBHOOK"]),
    ],
    executor=NodeTargetExecutor(node, max_volume=1),
)
```

CLI 在环境变量 `TQSDK_WECOM_WEBHOOK` 存在时自动加入企业微信通知：

```bash
export TQSDK_WECOM_WEBHOOK='https://qyapi.weixin.qq.com/cgi-bin/webhook/send?key=...'
PYTHONPATH=python python3 python/tqsdk_live.py   --strategy ema_donchian   --node-url http://127.0.0.1:18081   --symbol SHFE.rb2701   --execute --max-volume 1
```

通知失败默认不阻断下单；执行器失败不会被吞掉。组合 consumer 使用
`CompositeConsumer`，可以继续追加邮件、HTTP webhook 等。

## 10. 测试

```bash
PYTHONPATH=python python3 -m unittest discover -s python/tests -v
```

覆盖：

- 策略突破/止损/持仓；
- closed bar 过滤；
- `/positions`/开仓价解析；
- dry-run 不提交；
- `--execute` 路径提交 target；
- 实际合约到 `KQ.i@` 产品规格的映射。

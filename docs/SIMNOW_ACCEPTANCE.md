# SimNow / CTP 验收检查

`scripts/simnow_acceptance.py` 用于在 SimNow 上做可重复的端到端验收，避免
晚上手工点单后无法回溯“哪一步过了、哪一步失败”。

## 1. 启动环境

先准备 CTP/SimNow 配置：

```bash
cp config/live.local.env.example config/live.local.env
chmod 600 config/live.local.env
$EDITOR config/live.local.env
```

启动 gw/node，不启动 UI：

```bash
./scripts/run_live.sh --no-ui
```

默认 node：

```text
http://127.0.0.1:18081
```

也可以连接其他 node：

```bash
python3 scripts/simnow_acceptance.py --node-url http://127.0.0.1:18081
```

## 2. 只读检查

默认不会下单：

```bash
python3 scripts/simnow_acceptance.py \
  --symbol SHFE.rb2701 \
  --metrics
```

覆盖：

- `/healthz`；
- `/readyz`（最长等待 60 秒）；
- `/account`；
- `/positions`、`/orders`、`/trades`；
- `/quote/{symbol}`；
- 五档/一档盘口识别；
- `/metrics`（可选）；
- WebSocket `/stream` 订阅欢迎、订阅确认和快照（默认启用，可用 `--no-ws` 关闭）。

## 3. 限价/撤单/错误单验收

写操作必须显式确认：

```bash
python3 scripts/simnow_acceptance.py \
  --symbol SHFE.rb2701 \
  --orders --yes
```

默认行为：

1. 读取 quote 和 `price_tick`；
2. 计算远离盘口的限价（BUY 低于买一，SELL 高于卖一），尽量避免成交；
3. 提交 `BUY OPEN` 小单；
4. 查询 `/orders` 确认订单可见；
5. 立即撤单并等待终态；
6. 提交 `volume=0` 的错误单，验证拒绝链路。

如果限价单被立即成交或进入终态，报告会标记 `WARN`，因为此时无法验证撤单。

只预览订单，不提交：

```bash
python3 scripts/simnow_acceptance.py --orders --dry-run
```

`--orders` 不带 `--yes` 时会直接拒绝执行，避免误下真实 SimNow 单。

## 4. 市价/对价检查

市价单和对价单可能立即成交并产生持仓，默认 **不执行**。确认可以接受后显式开启：

```bash
python3 scripts/simnow_acceptance.py \
  --symbol SHFE.rb2701 \
  --orders --yes \
  --include-market \
  --include-counterparty
```

- 市价：`price_type=ANY`；
- 对价：`price_type=BEST`；
- 使用 `time_cond=IOC`；
- 脚本不会自动平掉产生的仓位，需要人工确认；
- 被柜台拒绝会记录 `WARN`，而不是误报成“下单成功”。

## 5. 报告

默认写入：

```text
artifacts/simnow_acceptance_YYYYmmdd_HHMMSS.json
artifacts/simnow_acceptance_YYYYmmdd_HHMMSS.md
```

可以指定路径：

```bash
--json-out artifacts/my_acceptance.json
--md-out artifacts/my_acceptance.md
```

退出码：

```text
0  没有 FAIL（WARN/SKIP 允许）
1  存在 FAIL
2  参数/安全确认失败
```

报告中的每条检查都有 `PASS/FAIL/WARN/SKIP`，并附带原响应证据，便于晚上复盘。

## 6. 自测

验收脚本本身有一个本地 fake node 测试，不会连接 SimNow：

```bash
python3 scripts/test_simnow_acceptance.py
```

它覆盖：

- health/ready/account/positions/orders/trades/quote；
- 限价提交和撤单；
- volume=0 错误拒绝；
- JSON/Markdown 报告生成。

## 7. 当前边界

- 脚本不启动 gw/node，复用 `run_live.sh --no-ui`；
- 暂不自动验证 CTP 今仓/昨仓平仓策略，平仓验收需要按持仓情况手工执行；
- 暂不自动重启 gw 做 reconnection 断言，可通过人工重启后再次运行只读检查；
- 真实 SimNow 验收仍应以柜台回报、成交/持仓查询结果为准，脚本只做自动化探针。

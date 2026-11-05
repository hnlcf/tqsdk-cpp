# Soak 与性能基线

## 1. Soak 工具

`scripts/soak_two_process.py` 是可配置的两进程长稳工具，默认时长 24h：

```bash
python3 scripts/soak_two_process.py
# 短时验证
python3 scripts/soak_two_process.py \
  --duration 30 --reconnect-interval 8 --load-threads 16 --order-interval 1 \
  --gw-bin ./build-release/tqsdk-gw --node-bin ./build-release/tqsdk-node
```

它同时施加：

- 8/16 个 keep-alive HTTP 线程持续 GET `/healthz`、`/account`；
- WebSocket 客户端 subscribe + 应用层 ping，断线自动重连；
- 周期性限价报单 + 撤单；
- 周期性 kill/restart gw，驱动 node 的 Resume + TradeSnapshot 恢复；
- 每秒采集 node/gw 的 RSS、FD 数、线程数，输出 CSV + JSON summary。

产物：

```text
artifacts/soak_YYYYmmdd_HHMMSS.csv
artifacts/soak_YYYYmmdd_HHMMSS.json
```

CSV 列：`t,node_rss_kb,node_fds,node_threads,gw_rss_kb,gw_fds,gw_threads,
node_alive,gw_alive,http_ok,http_err,order_ok,order_err,ws_messages,ws_err,reconnects`。

验收建议：

- `fatal` 为空，node 全程存活；
- node/gw RSS 在预热后不持续单调增长；
- FD/线程数稳定，不随重连次数线性增长；
- `http_ok > 0`，交易/WS 错误只集中在 gw 重启窗口。

### 已完成的短时验证（Release）

2026-11-03，`build-release`（无 ASan），30s，16 HTTP 线程，1s 报单/撤单，
8s 重启 gw：

```json
{
  "duration_s": 30.0,
  "http_ok": 153198,
  "http_err": 0,
  "order_ok": 23,
  "order_err": 4,
  "ws_messages": 12780,
  "ws_err": 0,
  "reconnects": 3,
  "node_max_rss_kb": 10520,
  "node_max_fds": 27,
  "node_max_threads": 4,
  "gw_max_rss_kb": 8624,
  "gw_max_fds": 10,
  "gw_max_threads": 4,
  "fatal": ""
}
```

`order_err=4` 对应 gw 重启窗口内的命令失败，属于预期故障窗口。

### 2026-11-03 最终回归（shared core + CTP 持久化/readyz）

`build-package` Release，30s，8 HTTP 线程，1s 报单/撤单，10s 重启 gw：

```json
{
  "duration_s": 30.0,
  "http_ok": 227167,
  "http_err": 0,
  "order_ok": 27,
  "order_err": 1,
  "ws_messages": 29612,
  "ws_err": 0,
  "reconnects": 2,
  "node_max_rss_kb": 26864,
  "node_max_fds": 19,
  "node_max_threads": 4,
  "gw_max_rss_kb": 26092,
  "gw_max_fds": 10,
  "gw_max_threads": 4,
  "fatal": ""
}
```

同版本 perf（8s，16 HTTP，4 WS）：

```text
| workload | qps | p50 ms | p90 ms | p99 ms | max ms | errors |
|---|---:|---:|---:|---:|---:|---:|
| HTTP x16 | 7515.4 | 1.355 | 4.769 | 10.596 | 39.378 | 0 |
| WS ping x4 | 14517.4 | 0.227 | 0.503 | 0.880 | 2.882 | 0 |
```

24h 运行由同一脚本直接执行：

```bash
python3 scripts/soak_two_process.py --out artifacts/soak_24h.csv
```

建议作为夜间/发布前任务运行，并把 JSON summary 与 CSV 归档。

## 2. 性能基线

`scripts/perf_baseline.py` 对运行中的 node 做客户端视角基线：

```bash
python3 scripts/perf_baseline.py --port 8080 \
  --clients 16 --duration 10 --ws-clients 4
```

- HTTP：16 条 keep-alive 连接顺序 GET `/healthz`；
- WebSocket：4 条连接测 `{"op":"ping"}` -> `{"type":"pong"}` RTT；
- 输出 QPS、p50/p90/p99/max（毫秒）与错误数，并写 JSON/Markdown。

### Release 基线（本机回环，2026-11-03）

```text
# tqsdk-node perf baseline
- target: 127.0.0.1:18080/healthz / /stream
- duration: 10.0s

| workload | qps | p50 ms | p90 ms | p99 ms | max ms | errors |
|---|---:|---:|---:|---:|---:|---:|
| HTTP x16 | 5875.9 | 1.740 | 6.095 | 13.870 | 86.149 | 0 |
| WS ping x4 | 11062.2 | 0.288 | 0.689 | 1.286 | 3.975 | 0 |
```

说明：

- 这是 Python 客户端 + 本机回环的**相对基线**，不是网络容量上限；
- 用于提交前后对比，建议固定机器、客户端线程数、duration 与目标 endpoint；
- 延迟包含 Python 客户端调度开销，绝对值仅作趋势参考；
- 不应在 ASan/TSan 构建上比较性能数字。

## 3. 推荐回归流程

```bash
# 1) 功能/单测
ctest --test-dir build --output-on-failure
ctest --test-dir build-ctp --output-on-failure   # CTP 可选

# 2) TSan 并发子集
TSAN_OPTIONS=halt_on_error=1 ./build-tsan/tqsdk_tests \
  --gtest_filter='Threading.*:WsStream.*:GatewayIpcServerTest.*:IpcClientTest.*:ChannelBasic.*'

# 3) Release 短 soak + perf
python3 scripts/soak_two_process.py --duration 30 \
  --gw-bin ./build-release/tqsdk-gw --node-bin ./build-release/tqsdk-node
# 4) 发布前 24h soak
python3 scripts/soak_two_process.py --duration 86400 \
  --gw-bin ./build-release/tqsdk-gw --node-bin ./build-release/tqsdk-node
```


### 2026-11-04 P1/P2 回归（debug build）

15s，8 HTTP 线程，1s 报单/撤单，5s 重启 gw，使用 `build-ui`：

```json
{
  "duration_s": 15.0,
  "http_ok": 124500,
  "http_err": 0,
  "order_ok": 12,
  "order_err": 2,
  "ws_messages": 17938,
  "ws_err": 0,
  "reconnects": 2,
  "node_max_rss_kb": 26896,
  "node_max_fds": 19,
  "node_max_threads": 4,
  "gw_max_rss_kb": 26048,
  "gw_max_fds": 10,
  "gw_max_threads": 4,
  "fatal": ""
}
```

`order_err=2` 集中在 gw 重启窗口，进程、HTTP、WS 均无 fatal，FD/线程数稳定。
该次为 P1/P2 功能回归短时验证；24h soak 仍按本文件上面的命令作为发布前任务。

### 2026-11-04 P4 短时验证与 24h 长稳启动

P4 通信/quote 元数据改动后的 20s Release 验证：

```json
{
  "duration_s": 20.0,
  "http_ok": 274530,
  "http_err": 0,
  "order_ok": 15,
  "order_err": 4,
  "ws_messages": 52097,
  "ws_err": 0,
  "reconnects": 3,
  "node_max_rss_kb": 27612,
  "node_max_fds": 15,
  "node_max_threads": 4,
  "gw_max_rss_kb": 26704,
  "gw_max_fds": 10,
  "gw_max_threads": 4,
  "fatal": ""
}
```

24h soak 已由 `scripts/soak_two_process.py --duration 86400` 启动，产物位于
`artifacts/soak_24h_*.csv/json`；完成后按本节验收标准归档。Windows CTP 验证
按计划推迟到后续阶段。

### 2026-11-04 最终 24h soak（进行中）

在 P4 通信/quote/WS 改动完成并 275/275 测试通过后，启动最终长稳：

```text
started_at: 2026-11-04 23:49:28
csv:        artifacts/soak_24h_20261104_234928.csv
log:        artifacts/soak_24h_20261104_234928.log
pid:        881836
```

配置：8 HTTP load 线程、每 1s 报单/撤单、每 10min 重启 gw。24h 结束后
按上面的验收标准检查 fatal、RSS/FD/线程趋势和重连计数。

### 2026-11-05 10:26 观察

当前 24h soak 已运行约 38,220s（10.6h），未发现异常：

```text
node_rss_kb: 28080–28336
node_fds:    19
http_ok:     541,576,101
http_err:    0
ws_messages: 58,161,900
ws_err:      0
reconnects:  63（测试脚本主动重启 gw）
order_ok:    7,537
order_err:   14（重启窗口内的预期错误）
```

进程仍在运行，计划运行到约 24h 后生成 CSV/JSON 总结。CTP 账户的每日结算
断线不会用这个 SimTd soak 掩盖；CTP 恢复/结算测试见
`docs/SIMNOW_ACCEPTANCE.md` §8。

### 2026-11-05 CTP 交易时段监控

已增加 `scripts/ctp_session_soak.py`，它按交易阶段采样 `/readyz`、账户、持仓和
委托，不把每日结算断线算作系统故障。当前 CTP 还预留着一个跨结算的
`SHFE.rb2701 long=1` 平昨测试仓，相关状态记录在
`artifacts/ctp_close_yesterday_pending.json`。

### CTP 交易时段 monitor

除了 SimTd 24h soak，当前还启动了 CTP 交易时段 monitor：

```text
out:  artifacts/ctp_session_20261105_live.jsonl
log:  artifacts/ctp_session_20261105_live.log
```

它每 30s 采样 `/readyz`、account、positions、orders，按 `phase` 区分
`open/break/pre_open` 与 `settlement/closed`。结算断线会记录为 expected，
不会伪装成 CTP 连续 24h 在线。

### 2026-11-05 后续自动任务

已通过 `at` 安排：

```text
09:30 2026-11-06  平昨自动 runner（隔离端口 24332/28083）
10:30 2026-11-06  平昨重试
10:35 2026-11-06  下一交易时段 CTP session monitor（14400s）
13:35 2026-11-05  小额期权部分成交 probe
```

所有任务均使用隔离端口，避免与正在运行的 session monitor 冲突。跨结算的
CTP 任务只对 `readyz.phase`/expected disconnect 做记录，不把预期断线当成故障。

### CTP monitor 重启（WS 修复后）

为验证 WebSocket 修复，CTP 交易时段 monitor 已重启到最新 node/gw：

```text
out: artifacts/ctp_session_20261105_final.jsonl
log: artifacts/ctp_session_20261105_final.log
```

现场验证结果：

```text
quote-only subscribe -> topics=[]，snapshot 只有 quotes
orders 显式订阅     -> 首帧 snapshot 带 orders，后续不再重复相同订单
```

CTP 仍按交易时段/结算阶段采样。

# Soak 与性能基线

本文记录长稳/交易时段 soak 的入口、判定标准和当前状态。架构见
[ARCHITECTURE.md](ARCHITECTURE.md)，性能细节见
[PERFORMANCE_BASELINE.md](PERFORMANCE_BASELINE.md)。

## 1. 两进程 soak

```bash
# 短时验证
./scripts/soak_long_job.sh --duration 60 --reconnect-interval 10

# 24h / 72h
./scripts/soak_long_job.sh --duration 86400
./scripts/soak_long_job.sh --duration 259200
```

底层是 `scripts/soak_two_process.py`，会同时施加：

- keep-alive HTTP `/healthz`、`/account` 负载；
- WebSocket subscribe + ping，断线重连；
- 周期限价报单/撤单；
- 周期 kill/restart gateway，驱动 Resume 和交易快照恢复；
- 采集 RSS/FD/线程数，输出 CSV + JSON summary。

产物默认在 `artifacts/soak_<timestamp>/`。通过标准：

- `fatal` 为空，node 全程存活；
- 预热后 RSS 不持续单调增长；
- FD/线程数不随重连次数线性增长；
- 交易错误只集中在 gateway 重启窗口。

短时结果只用于回归，不代表 24h/72h 长稳通过。

## 2. CTP 交易时段 soak

CTP 账户在每日结算期间会预期断线，不能用连续 24h 在线判断可靠性：

```bash
./scripts/ctp_week_preflight.sh
python3 scripts/ctp_week_soak.py --days 5 --execute
python3 scripts/ctp_daily_reconcile.py --out-dir artifacts/ctp_reconcile
```

`ctp_session_soak.py` 按交易时段采样 `/readyz`、account、positions、orders，
把结算/闭市断线标记为 `expected_disconnect`，只把交易时段内断线计为 unexpected。

## 3. GitHub Actions

`.github/workflows/soak.yml` 使用 self-hosted runner（标签
`self-hosted, linux, tqsdk-soak`），因为托管 runner 单 job 最长 6 小时，且长稳需要
固定机器、配置和网络。支持 `workflow_dispatch` 和每周五 18:00 Asia/Shanghai 的
72h campaign；产物写入 `artifacts/`。

## 4. 性能基线

```bash
python3 scripts/bench_http_ws.py \
  --base http://127.0.0.1:18081 --requests 200 --ws-seconds 5

node scripts/bench_wasm_plugins.mjs
```

HTTP/WebSocket/WASM 的可重复数据和环境说明见
[PERFORMANCE_BASELINE.md](PERFORMANCE_BASELINE.md)。不要把本机短测数字当成跨机器
SLA。

## 5. 当前状态

- SimTd 两进程短测/24h 历史回归已完成，产品代码和测试仍在持续更新；
- CTP session monitor 支持结算预期断线；
- 当前 7 天 CTP soak 仍在运行，输出在 `artifacts/ctp_week_040/`；
- 真实 5 个交易日完成前，不宣称 CTP 连续周验证通过；
- 长稳失败时必须保留 CSV/JSON、gw/node 日志和配置快照，先定位再重启。

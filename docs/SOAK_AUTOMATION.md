# 长稳自动化

## 1. 本地/专用环境

```bash
./scripts/soak_long_job.sh --duration 86400  # 24h
./scripts/soak_long_job.sh --duration 259200 # 72h
```

产物包括：

- `soak_*.csv`：每秒 RSS/FD/线程/吞吐；
- `soak_*.json`：汇总；
- gw/node 日志。

## 2. GitHub Actions

`.github/workflows/soak.yml` 使用 **self-hosted Linux runner**，因为：

- GitHub 托管 runner 单 job 最长 6 小时；
- 24h/72h soak 需要持久机器、CTP/Sim 配置和固定网络。

触发方式：

- `workflow_dispatch`：输入 `duration_seconds`、`reconnect_interval`；
- 定时：每周五 18:00 Asia/Shanghai 启动 72h campaign（cron UTC = 周五 10:00）。

runner 标签：

```text
self-hosted, linux, tqsdk-soak
```

runner 需要：

- CMake、Ninja、clang、vcpkg（`/opt/vcpkg`）；
- 已配置的 CTP/Sim 环境；
- 可写 `artifacts/`。

## 3. 通过标准

- `fatal == ""`；
- gw/node 进程未非预期退出；
- 24h/72h 内 RSS/FD/线程无持续增长；
- CTP 结算断线按 `expected_disconnect` 分类；
- 每个交易日有成功采样和 daily reconcile 产物。

## 4. 当前状态

本工作区已完成：

- 60s soak（含 5 次网关重启）；
- 双 Sim gw 进程级验收；
- ASan 双账户验收。

**未在当前机器实际跑满 24h/72h**；workflow 和脚本是执行入口，不应把“脚本存在”
写成“长稳通过”。真实 24h/72h 结果应在专用 runner 上归档。

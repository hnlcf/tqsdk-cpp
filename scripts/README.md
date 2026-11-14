# Scripts

本目录只保留当前维护的入口。历史 P0/P6 campaign、一次性 probe 和重复脚本已移除；
需要追溯请使用 Git 历史。

## 构建、测试与 CI

| 脚本 | 用途 |
|---|---|
| `ci_local.sh` | 本地完整回归：构建、CTest、Web、WASM ABI、部署检查 |
| `asan_job.sh` / `tsan_job.sh` / `ubsan_job.sh` | 独立 sanitizer 构建与测试 |
| `check_secrets.py` | 检查 tracked 文件中的疑似凭据 |
| `verify_deploy.sh` | systemd/部署模板静态检查 |
| `build_wasm_plugins.sh` | 构建 WASM 指标插件 |
| `check_wasm_plugins.mjs` | 校验 WASM 插件 ABI |
| `bench_wasm_plugins.mjs` | WASM 指标基准 |

## Live 运行与诊断

| 脚本 | 用途 |
|---|---|
| `run_live.sh` | 启动 gw/node；默认读取 `config/live.local.env` |
| `run_live_supervisor.sh` | 最小 live supervisor，按退避重启 |
| `run_web.sh` | 启动 gw/node + Vite Web |
| `live_healthcheck.sh` | `/healthz`、`/readyz`、交易阶段检查 |
| `ctp_week_preflight.sh` | 连续交易周前的只读检查 |

TQ OTG 交易网关的配置见 `config/shared/tq.toml`，由 `config/gw.toml`
的 `[include]` 引入；其中
`broker_name`（天勤元数据键 / `req_login.bid`）与可选的 `broker_id`
（前置柜台 id）不是同一个字段。

## Soak 与性能

| 脚本 | 用途 |
|---|---|
| `soak_long_job.sh` | 24h/72h 两进程 soak 包装 |
| `soak_two_process.py` | 两进程并发 REST/WS/下单/重连 soak |
| `ctp_session_soak.py` | CTP 交易时段采样，区分预期结算断线 |
| `ctp_week_soak.py` | 连续交易周编排器 |
| `ctp_daily_reconcile.py` | 每日账户/持仓/订单对账快照 |
| `bench_http_ws.py` | HTTP/WS 延迟与吞吐基线 |

## 验收

| 脚本 | 用途 |
|---|---|
| `simnow_acceptance.py` | 单 SimNow/CTP 账户只读/限价/撤单验收 |
| `test_simnow_acceptance.py` | `simnow_acceptance.py` 的 hermetic smoke test |
| `simnow_dual_acceptance.py` | 双 SimNow 账户进程级验收 |
| `multi_account_acceptance.py` | 双 Sim gw 多账户路由验收 |
| `ctp_recovery_test.sh` | gateway/node 重启恢复与快照一致性 |

写操作脚本默认要求显式确认，且只在 SimNow/测试账户使用：

```bash
python3 scripts/simnow_acceptance.py --orders --yes
python3 scripts/simnow_dual_acceptance.py --orders --yes --symbol DCE.v2701
```

详细流程见 [docs/SIMNOW_ACCEPTANCE.md](../docs/SIMNOW_ACCEPTANCE.md)、
[docs/SIMNOW_DUAL_ACCOUNT.md](../docs/SIMNOW_DUAL_ACCOUNT.md)。

## 策略与回测

| 脚本 | 用途 |
|---|---|
| `fetch_tq_daily.py` | 下载 KQ.i@ 十年日线数据 |
| `backtest_daily.py` | EMA60/Donchian22/ATR 正式日线回测和报告 |
| `test_backtest_daily.py` | 回测 primitives 单测 |
| `strategy_scan.py` | 4h 策略扫描 |
| `strategy_execute.py` | 评估真实合约并可选提交 TargetPosTask |

## 打包与容器

| 脚本 | 用途 |
|---|---|
| `package_release.sh` | 生成 DEB/RPM/TGZ release 产物 |
| `package_deb.py` | 不依赖 dpkg-deb 的 DEB 构建 |
| `package_rpm.sh` | 从 staging tree 构建 RPM |
| `build_container.sh` | Docker/Podman 镜像构建和 smoke |

## 约定

- 不在脚本提交真实账号、密码、token；
- 写操作必须 `--yes`，优先使用隔离端口/flow 路径；
- 长稳和真实 CTP 的结论写入 `artifacts/`，不写回脚本注释；
- 删除或替换脚本时同步更新本文件和 `docs/README.md`。

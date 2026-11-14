# 文档索引

本目录只保留**当前实现和维护入口**。历史设计、一次性 campaign 记录不再维护；
需要追溯时使用 Git 历史，不再堆叠 `archive/` 副本。

根目录 [README.md](../README.md) 是项目入口。

## 1. 架构与协议

| 文档 | 内容 |
|---|---|
| [ARCHITECTURE.md](ARCHITECTURE.md) | 进程拓扑、分层、数据所有权、线程模型 |
| [MODULES.md](MODULES.md) | 模块职责、关键文件和依赖方向 |
| [FLOWS.md](FLOWS.md) | 行情/交易/订阅/重连信息流 |
| [STATE_MACHINES.md](STATE_MACHINES.md) | 进程、连接、订单、交易日状态机 |
| [PROTOCOLS.md](PROTOCOLS.md) | IPC 帧、REST、WebSocket 协议 |
| [ERROR_HANDLING.md](ERROR_HANDLING.md) | `std::expected`、错误边界、异常隔离和日志 |
| [TQ_PROTOCOL.md](TQ_PROTOCOL.md) | TQ OTG 全部 aid、消息结构、TqTdGateway |
| [TQ_AUTH.md](TQ_AUTH.md) | 快期账户认证、token 和动态 URL |

## 2. 构建、部署与运维

| 文档 | 内容 |
|---|---|
| [OPERATIONS.md](OPERATIONS.md) | 依赖、构建、配置、启动、健康检查、诊断 |
| [DEPLOYMENT.md](DEPLOYMENT.md) | TLS、token 认证、systemd、日志脱敏和审计 |
| [PACKAGING.md](PACKAGING.md) | DEB/RPM/TGZ 打包和发布 |
| [CONTAINERS.md](CONTAINERS.md) | Docker/Podman 镜像 |
| [CI.md](CI.md) | CI、sanitizer、短 soak 和回归门禁 |
| [SOAK_PERF.md](SOAK_PERF.md) | 长稳工具、通过标准、CTP session soak |
| [PERFORMANCE_BASELINE.md](PERFORMANCE_BASELINE.md) | HTTP/WS/WASM 可重复性能基线 |
| [RELEASE_0.4.0.md](RELEASE_0.4.0.md) | 当前 0.4.0 release 说明 |

## 3. 交易、CTP 与验收

| 文档 | 内容 |
|---|---|
| [CTP.md](CTP.md) | CTP 接入、报单映射、SHFE 规则、查询和节假日 |
| [SIMNOW_ACCEPTANCE.md](SIMNOW_ACCEPTANCE.md) | 单账户 SimNow/CTP 验收 |
| [SIMNOW_DUAL_ACCOUNT.md](SIMNOW_DUAL_ACCOUNT.md) | 双 SimNow 账户进程级验收 |
| [MULTI_ACCOUNT_ACCEPTANCE.md](MULTI_ACCOUNT_ACCEPTANCE.md) | 双 Sim 网关多账户验收 |
| [MULTI_ACCOUNT_BACKTEST.md](MULTI_ACCOUNT_BACKTEST.md) | 多账户门面与 bar 回测 |
| [ORDER_TASKS.md](ORDER_TASKS.md) | `TargetPosTask` / `InsertOrderTask` |
| [RISK_CONTROL.md](RISK_CONTROL.md) | `RiskManager` 和风控规则 |
| [TRADING_STATUS.md](TRADING_STATUS.md) | 交易状态与动态 MD/TD URL |

## 4. 数据、合约与期权

| 文档 | 内容 |
|---|---|
| [CONSTANTS.md](CONSTANTS.md) | 跨模块默认值、容量和协议上限 |
| [INSTRUMENTS.md](INSTRUMENTS.md) | 标准合约目录 |
| [KLINE_CACHE.md](KLINE_CACHE.md) | SQLite K 线缓存 |
| [CONTINUOUS_CONTRACTS.md](CONTINUOUS_CONTRACTS.md) | KQ 主连/指数与 1d 复权 |
| [TICK_DATA.md](TICK_DATA.md) | Tick / DataSeries / DataDownloader |
| [OPTION_ANALYTICS.md](OPTION_ANALYTICS.md) | 期权 ATM、Black-Scholes、Greeks |

## 5. Web 前端

| 文档 | 内容 |
|---|---|
| [WEB_FRONTEND.md](WEB_FRONTEND.md) | TQSDK Board 架构、功能、构建 |
| [WEB_CUSTOM_INDICATORS.md](WEB_CUSTOM_INDICATORS.md) | WASM 指标 ABI、Manifest、SDK 和 Worker |

## 6. 策略、回测与 Python

| 文档 | 内容 |
|---|---|
| [STRATEGY_EMA60_DONCHIAN.md](STRATEGY_EMA60_DONCHIAN.md) | EMA60 + Donchian22 + ATR 策略 |
| [DAILY_STRATEGY_BACKTEST.md](DAILY_STRATEGY_BACKTEST.md) | 2016-2026 日线正式回测报告 |
| [DAILY_BACKTEST_DATA.md](DAILY_BACKTEST_DATA.md) | 回测数据源、过滤和方法 |
| [PYTHON_BACKTEST.md](PYTHON_BACKTEST.md) | Python 调用 C++ BacktestEngine |
| [PYTHON_LIVE_TRADING.md](PYTHON_LIVE_TRADING.md) | Python 实时策略 -> node REST |
| [PYTHON_LIBRARY.md](PYTHON_LIBRARY.md) | `tqsdk_py` 策略库 |
| [PYTHON_PACKAGING.md](PYTHON_PACKAGING.md) | uv、wheel 和 native 打包 |

## 7. 未来计划

- [ROADMAP.md](ROADMAP.md)

文档更新原则：协议或行为变更先改 canonical doc，再改专题文档；不再新增按日期堆积的
一次性验收记录，结果写入 `artifacts/` 或对应维护页的“当前状态”小节。

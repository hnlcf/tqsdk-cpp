# 文档索引

本目录按“总体 → 模块 → 交互 → 状态 → 协议 → 运维 → 专题”分层。根目录
[README.md](../README.md) 是入口，canonical docs 如下。

## 1. 总体设计

| 文档 | 内容 |
|---|---|
| [ARCHITECTURE.md](ARCHITECTURE.md) | 进程拓扑、分层、数据所有权、线程模型、性能边界 |
| [MODULES.md](MODULES.md) | 每个模块的职责、关键文件、公开接口和依赖方向 |
| [FLOWS.md](FLOWS.md) | 行情/交易/订阅/重连/结算/UI 信息流 |
| [STATE_MACHINES.md](STATE_MACHINES.md) | 进程、连接、CTP、订单、WS 客户端、交易日状态机 |
| [PROTOCOLS.md](PROTOCOLS.md) | IPC 帧、REST、WebSocket、序列化和错误语义 |

## 2. 运行与运维

| 文档 | 内容 |
|---|---|
| [OPERATIONS.md](OPERATIONS.md) | 依赖、构建、配置、启动、测试、长稳、诊断 |
| [SIMNOW_ACCEPTANCE.md](SIMNOW_ACCEPTANCE.md) | SimNow 真实 CTP 验收流程和报告 |
| [SOAK_PERF.md](SOAK_PERF.md) | 24h SimTd soak、CTP session monitor、性能基线 |
| [CTP_WEEKLY_TEST_PLAN.md](CTP_WEEKLY_TEST_PLAN.md) | 连续交易日 CTP soak、每日对账、执行清单 |
| [P6_AUTOMATION.md](P6_AUTOMATION.md) | 2026-11-09 09:00 P6.1/P6.2/P6.3 自动排期与产物 |
| [CTP.md](CTP.md) | CTP 接入、SHFE 价格转换、平今平昨、节假日 |
| [HA（归档）](archive/HA_LEGACY.md) | 旧版高可用草案，待重写 |

## 3. UI 与扩展

| 文档 | 内容 |
|---|---|
| [UI_QT.md](UI_QT.md) | Qt UI 架构、NodeClient、面板、指标、4K/高 DPI |
| [INDICATOR_PLUGINS.md](INDICATOR_PLUGINS.md) | Python sidecar、插件 MANIFEST、共享指标库 |
| [INSTRUMENTS.md](INSTRUMENTS.md) | 合约目录格式、加载与刷新 |
| [KLINE_CACHE.md](KLINE_CACHE.md) | SQLite K 线缓存、恢复与容量 |

Web 前端当前不推进；旧方案见
[archive/WEB_FRONTEND_DEFERRED.md](archive/WEB_FRONTEND_DEFERRED.md)。

## 4. 未来计划

- [ROADMAP.md](ROADMAP.md)

## 5. 历史归档

[archive/](archive/) 中的文档只用于追溯旧设计，不代表当前实现：

- `README_LEGACY.md`
- `ARCHITECTURE_REVIEW_2026_11.md`
- `THREADING_LEGACY.md`
- `IPC_RECOVERY_LEGACY.md`
- `HA_LEGACY.md`
- `UI_ARCHITECTURE_LEGACY.md`
- `WEB_FRONTEND_DEFERRED.md`
- `MODULES_LEGACY.md`
- `ROADMAP_LEGACY.md`

新代码/协议变更应先更新 canonical docs，再更新专题文档；归档文档不再维护。

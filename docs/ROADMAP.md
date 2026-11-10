# Roadmap

本文只列当前仍然有意义的工作；历史路线已归档到
[archive/ROADMAP_LEGACY.md](archive/ROADMAP_LEGACY.md)。

## 1. 当前基线

已具备：

- Tq 行情 / CTP 交易 / Sim 交易；
- gw/node 两进程 + IPC seq/replay；
- REST/WS 数据面；
- TQSDK Board Web：SolidJS + pnpm + Lightweight Charts；
- 合约目录、节假日、K 线缓存；
- SimNow CTP 验收、24h SimTd soak、CTP session monitor；
- 订单价格归一化、平今/平昨、命令幂等、故障恢复；
- 本轮模块重构：WsEntityFilter、SnapshotService、GatewaySubscriptionBook、
  CommandAckCache、DiffMerge/CTP/gateway/http 分文件。

## 1.1 当前剩余优先级

1. **TqTdGateway**：消费动态 TD URL，实现 TQ 交易网关登录、下单/撤单、成交/持仓 diff、重连对账；需要官方协议/可用测试端点。
2. ✅ **WebSocket 多账户过滤**：`/stream?account=` 选择每账户独立 hub；Web 账户切换会重连并带账户订阅。
3. ✅ **账户级 task/risk**：`/task/*`、`/risk/*` 支持 `?account=`，任务按账户过滤，风控使用对应 TqApi。
4. ✅ **进程级双账户验收**：两个独立 Sim gw + node 的 `/accounts`、下单、持仓、task/risk、`/stream?account=` 已通过；真实 CTP/SimNow 双账户仍待执行，见 [MULTI_ACCOUNT_ACCEPTANCE.md](MULTI_ACCOUNT_ACCEPTANCE.md)。
5. **前端 E2E/视觉回归**：Playwright 覆盖搜索选合约、切换合约/周期、K 线缩放、账户切换、断线重连。
6. ✅ **IPC/WS fuzz 冒烟 + 24h-72h soak 脚本 + GitHub Actions workflow**；远程 runner 尚未实际执行，真实 72h 尚未跑。
7. **CTP 连续 5 个交易日 soak 与交易日矩阵**；真实交易日不可压缩。
8. **生产化**：TLS/认证（node 目前 loopback）、systemd/supervisor、日志脱敏、审计。
9. **性能/协议**：基线见 [PERFORMANCE_BASELINE.md](PERFORMANCE_BASELINE.md)；WS binary/MessagePack/虚拟列表按 profile 决策。

## 2. P0 执行/风控能力（已实现）

- ✅ 高级报单：FAK/FOK、市价、BEST、FIVELEVEL、MIN/ALL、offset_leg2；
  - `InsertOrderReq/Order`、REST/WS/IPC/CTP 映射、Web 三键下单 UI；
  - `apply_advanced_order()` 统一转 CTP `IOC+ANY/ALL`；
- ✅ `TargetPosTask` / `InsertOrderTask` / `InsertOrderUntilAllTradedTask`；
  - update hook 驱动，兼容 node 无 user wait_update 的场景；
  - 单任务单在途单、ACTIVE/PASSIVE/CUSTOM、拆单；
  - REST：`/task/target`、`/task/order`、`/tasks`；
- ✅ `RiskManager` + OpenCounts/OpenVolumes/OrderRate 规则；
  - 在网关前拒绝并生成可见的本地错误订单；
  - REST：`/risk/status`、`/risk/rules`；
- ✅ 单测/Sim 集成测试和 P0 SimNow 验收脚本：
  - [P0_SIMNOW_TEST_PLAN.md](P0_SIMNOW_TEST_PLAN.md)
  - [ORDER_TASKS.md](ORDER_TASKS.md)
  - [RISK_CONTROL.md](RISK_CONTROL.md)

**下一轮增强**：TargetPosTask 追价节流/任务统计、交易日历日切。多账户风控已接入
`/risk/*?account=`；策略任务按账户隔离已接入。

## 2. 下一优先级（P6）

### P6.1 CTP 交易会话可靠性

- ✅ `CtpQueryPipeline` 已从 `CtpTdGateway` 独立：
  - query generation/watchdog；
  - 查询队列；
  - timeout/retry；
  - callback -> pipeline -> snapshot；
  - 7 项独立单测 + CTP 故障注入回归通过。
- 连续 5 个交易日 CTP session soak（真实交易日，不压缩）：
  - 已准备 `scripts/ctp_week_preflight.sh`；
  - 已准备 `scripts/ctp_week_soak.py`；
  - 已准备 `scripts/ctp_daily_reconcile.py`；
  - 执行清单见 [CTP_WEEKLY_TEST_PLAN.md](CTP_WEEKLY_TEST_PLAN.md)。
- 跨结算重启对账、盘中 CTP front 断线、查询延迟分布。

### P6.2 交易验收矩阵

- SHFE/CZCE/DCE 平今/平昨交叉矩阵；
- 部分成交/部分撤单（需低流动性窗口）；
- 开盘/收盘/午休边界；
- 订单在途断线、ack 丢失、重复 rid；
- 多账户/多 node 共享 gw。

### P6.3 长稳与故障

- ✅ 24h SimTd soak 已归档；✅ 72h 包装脚本 `scripts/soak_long_job.sh` 已就绪，真实 72h 待专用环境执行；
- ✅ ASan/TSan/UBSan 本地全量通过；✅ GitHub Actions workflow 已添加，远程 runner 待实际触发；
- ✅ IPC/WS fuzz 冒烟测试已加入 ctest；
- 待补充：帧长度半包/超大长度网络级 fuzz、重复 seq/epoch 回绕；
- CTP 查询 watchdog 抖动注入；
- 内存/FD/线程基线告警。

## 2.1 P1/P2 能力（本轮已实现）

- ✅ P1-1：Tick/DataSeries/DataDownloader、`/ticks/{symbol}`；
- ✅ P1-2：Black-Scholes/Greeks、ATM 期权链、`/option/analytics/{underlying}`、Web Greeks；
- ✅ P1-3：TradingStatus、`/trading-status`、Tq MD 动态 URL、TD URL 解析诊断；
- ✅ P2：`MultiAccountManager`（每账户独立 TqApi）、独立 bar `BacktestEngine`。
- ✅ node REST 多账户路由、TickStore 统一行情 SQLite、tick 级回测；
- ✅ WebSocket 多账户过滤、账户级 task/risk 已接入；
- 待集成：TqTdGateway。

## 3. UI 与可观测性

- TQSDK Board：多自选模糊搜索/K 线/分时/盘口/快捷下单/合并持仓/委托/成交/权益已落地；
- 指标参数/颜色设置、动态均线、输出样式和启用状态本地持久化已落地；
- ✅ WASM 指标插件：自动扫描注册、浮动选择器、逐输出颜色/线型/显隐、EMA/MA 动态均线；MA/BOLL/MACD/RSI/ATR/Donchian/KDJ/CCI/OBV/VWAP；
- ✅ 前端 Playwright E2E（搜索/大小写/WASM/下单板按钮）；
- TqAuth 快期认证与目录 Bearer header 已接入；
- ✅ 顶栏账户选择器、运维页、持仓固定列宽、K 线切换缩放/本地时间；
- ✅ 指标 Worker 并行、全隐藏跳过、输出缓存、自选/侧栏/持仓表尺寸持久化；
- 图表画线 primitive/overlay、指标增量计算（按 profile）；
- 前端 Playwright 端到端扩展（登录/订阅/真实下单/重连/视觉回归）；
- 虚拟列表、列宽/排序持久化；
- 期权希腊字母、波动率与更完整期权分析；

## 4. 架构与性能

- ✅ `CtpQueryPipeline`、`MarketSnapshotService` 服务边界已收敛；
- ✅ HttpServer 路由 handler 已拆为独立方法，setup_routes 只做注册；
- ✅ 路由已按 catalog/market/trading/task-risk 拆分；
- IPC 可选 MessagePack/共享内存快路径；
- WS 二进制行情帧评估；
- KlineStore 仅 node 构建依赖；
- core pch 与 UI pch 分离；
- multi-account/路由策略。

## 5. 平台

- Windows CTP 编译、SDK 打包、SimNow 实盘验证；
- Linux 打包/安装目录；
- 服务化部署（systemd/supervisor）；
- TLS + 认证（当前 node 只能 loopback）；
- 生产日志脱敏和审计。

## 6. 暂缓/不做

- 完整交易所官方交易日历服务：当前使用 shinny 节假日 + 可配置 session；
- 共享内存 IPC：先完成稳定性和多账户路由再评估。

## 7. 验收定义

每项完成必须同时满足：

1. 代码边界清晰，依赖方向不逆转；
2. 单元/集成/e2e 回归通过；
3. 关键失败路径有测试或可重复脚本；
4. canonical docs 同步更新；
5. 不把预期断线/撮合限制伪装成 PASS。

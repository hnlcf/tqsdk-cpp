# 前后端重构评估（2026-11-10）

## 1. 结论

**不需要推倒重写。** 当前 C++/SolidJS/CTP 分层的方向是正确的，单元/集成/soak
证据也集中在现有边界上；大规模重写会造成协议、账户、订单恢复和指标 ABI 同时
漂移，风险高于收益。

需要的是**前端按控制器拆分、后端按账户上下文收敛**的增量重构：

- 先做不改协议的重构（低风险、可逐步测试）；
- 账户、任务、风控统一使用一个 `AccountScope`，不再在每个 handler 里重复
  `?account=` 判空；
- 性能优化以基线数据驱动，优先图表/Worker/WS 序列化，不先改 IPC 协议。

## 2. 现状规模

```text
web/src/App.tsx                         526 行（组合/页面状态）
web/src/hooks/useWorkspaceLayout.ts     115 行
web/src/hooks/useWasmIndicators.ts      210 行
web/src/hooks/useMarketStream.ts         70 行
web/src/hooks/useChartData.ts            71 行
web/src/hooks/useTradingSession.ts      200 行
web/src/chart/KLineChart.tsx            444 行（WASM-only）
web/src/chart/data.ts                    25 行
web/src/chart/seriesMapping.ts           56 行
web/src/components/IndicatorSettings.tsx 408 行
web/src/api/client.ts                   666 行
src/serve/http_routes.cpp               199 行
src/serve/http_routes_catalog.cpp       197 行
src/serve/http_routes_market.cpp         73 行
src/serve/http_routes_trading.cpp        55 行
src/serve/http_routes_task_risk.cpp     135 行
src/serve/account_router.{h,cpp}        105 行
src/serve/http_server.cpp               270 行
src/gateway/ctp/ctp_td_gateway.cpp      326 行
src/gateway/ctp/ctp_query_pipeline.cpp  234 行
```

规模本身不是问题；问题是单个文件同时承担了太多生命周期。

## 3. 前端发现

### 3.1 App.tsx 是主要复杂度热点

`App` 现在负责页面组合、自选/URL/主题/Toast 等页面状态；行情流、交易会话、
图表数据、WASM 指标、布局拖动已分别抽到独立 hook。

建议按**可测试控制器**拆分（不是简单拆组件）：

1. `useMarketStream(account)`：WebSocket 连接、订阅 diff、重连、状态（✅）；
2. `useTradingSession(account)`：REST 快照、WS diff 合并、下单/撤单（✅）；
3. `useWasmIndicators()`：WASM worker、参数、输出缓存、计算结果（✅）；
4. `useWorkspaceLayout()`：尺寸/拖动/localStorage（✅）；
5. `useChartData(symbol, period)`：K 线加载、live bar 合并、周期切换（✅）。

目标是 `App.tsx` 回到 250–350 行，只负责组合和路由。

### 3.2 KLineChart 需要适配层

`KLineChart.tsx` 同时处理 Lightweight Charts 生命周期、主图/副图 pane、WASM
输出类型、markers、legend、主题和尺寸。建议拆为：

- `chart/createChart.ts`：创建/销毁、resize；
- `chart/seriesBinding.ts`：把 `WasmSeries`/K 线映射成 pane/series；
- `chart/legend.ts`：只读 DOM 展示；
- `KLineChart.tsx`：输入输出适配。

这样测试可以不依赖真实浏览器布局，用纯映射函数覆盖 line/histogram/area/band/
markers 与颜色/线型。

### 3.3 指标设置/客户端也偏大

- `IndicatorSettings` 拆成 picker、detail、output row、dynamic line editor；
- `api/client.ts` 拆 `rest.ts` / `stream.ts` / `types.ts`，避免任何页面 import
  整个 HTTP+WS 客户端；
- 迁移完成后删除 `indicators.ts` 里的旧本地指标实现和旧 `IndicatorConfig`
  storage API，减少“两套指标”的认知负担；
- `WasmPluginView` 可引入 `ready/loading/error` 的 discriminated union，减少
  `status === "ready"` 散布。

## 4. 后端发现

### 4.1 账户上下文已收敛（2026-11-10 已完成第一步）

`AccountRouter`/`AccountScope` 已落地，HTTP REST/task/risk 统一通过
`HttpServer::account_scope()` 解析账户，未知账户抛 404；任务/风控不再各自判空。
新增 `tests/serve/account_router_test.cpp` 覆盖无 manager、默认账户、未知账户。

`TaskRegistry` 已纳入 `AccountScope`：

```text
AccountScope {
  account_id;       // resolved default if empty
  TqApi* api;
  TaskRegistry* tasks;   // per-account view
  RiskManager* risk;
}
```

handler 通过 `scope.tasks` 直接访问本账户任务，task id 在账户内独立；新增
`TaskRegistryIdsArePerAccount` 集成测试验证 A/B 任务隔离和同 id 不串账户。

### 4.2 WebSocket hub 生命周期（懒创建 + 自动移除）

`HttpServer` 不再在构造时预先创建所有账户 hub；收到 `/stream?account=` 时在
`data_ex_` 上解析账户并按需创建 hub，避免启动期静态枚举。

已提供 `HttpServer::drop_account_hub()`；`MultiAccountManager` 通过
`set_account_removed_handler()` 在 `remove_account()/close()` 时通知 HttpServer，
先停止该账户 hub 再关闭 TqApi。`RemovedAccountDropsHubAndRejectsStream` 覆盖
“server 构造后新增账户 → 建 hub → 删除账户 → `/stream` 返回 404”。

### 4.3 路由文件已按资源拆分（2026-11-10）

`http_routes.cpp` 只保留 health/ready/metrics/accounts/holiday/trading-status/
websocket 与路由表；catalog/option、market、trading、task/risk 分别拆到独立文件。
公共 `RouteHandlers` 声明在 `src/serve/http_route_handlers.h`，query helper 在
`src/serve/http_route_helpers.h`。

### 4.4 CTP/数据面不建议现在重写

`CtpQueryPipeline`、`CtpTdGateway`、`GatewaySubscriptionBook`、`DiffMergeEngine`
都有专项测试和真实 SimNow 验收；当前拆分粒度合理。只有当 5 日 soak 暴露明确的
生命周期/状态耦合时再做，不应为了“文件更短”重写订单恢复逻辑。

## 5. 协议/性能判断

- WASM 全量计算 7 插件约 0.48ms/5000 根，优先优化 Worker 消息与图表 setData；
- HTTP quote/health 已在亚毫秒级，kline p50 约 2.6ms（本机）；
- WS 首帧 snapshot 约 15ms，静默行情约 5 条/s；
- 因此 **MessagePack/共享内存/WS binary 不应作为第一优先级**，先做 profile
  和 24h/72h soak 的 CPU/延迟分布；
- 前端虚拟列表只在行数达到数千行时才有收益，当前自选/持仓规模不需要。

## 6. 建议顺序

### P0（低风险，先做）

1. `App` 控制器拆分 + 单元测试（stream/trading/indicators/layout）；
   - ✅ `useWorkspaceLayout`、`useMarketStream`、`useChartData`、
     `useTradingSession`、`useWasmIndicators` 已抽出；
   - ✅ 纯函数测试覆盖 layout/chart data/series mapping/trade diff；
2. `KLineChart` 纯映射层拆分；
   - ✅ `seriesMapping.ts` 已抽出 candle/line/volume 映射 + 3 项测试；
   - ✅ KLineChart 删除旧本地指标计算路径，只保留 WASM 指标；
3. `AccountRouter/AccountScope`；
   - ✅ 已落地，REST/task/risk 使用，2 项单测；
4. ✅ 删除旧本地指标计算/旧 `tqsdk.indicators.v1` storage API，统一 WASM 入口。

### P1（需要验收）

5. ✅ WS hub 动态账户生命周期：懒创建、`drop_account_hub()`、manager 自动通知和增删验收完成；
6. ✅ 路由按资源拆分；
7. ✅ 增加 KDJ/CCI/OBV/VWAP；指标增量计算/Worker 池仍按 profile 决定；
8. 前端虚拟列表按实际行数决定（待实测）。

### P2（协议/平台）

9. MessagePack/shm/binary WS 只在基线证明收益后做；
10. TLS/auth、systemd、日志审计仍属于生产化前置项，不是重构。

## 7. 验收标准

任何重构都必须同时满足：

- 原有 CTest 全量、Web typecheck/test/build、E2E、WASM ABI 脚本通过；
- 双账户 REST/WS/task/risk 隔离测试不退化；
- 24h soak 指标不出现新的 RSS/FD 增长；
- docs 同步，且不把“未执行的真实交易日/外部 CI”写成已验证。

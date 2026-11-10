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
web/src/App.tsx                         935 行
web/src/chart/KLineChart.tsx            686 行
web/src/components/IndicatorSettings.tsx  408 行
web/src/api/client.ts                   666 行
src/serve/http_routes.cpp               709 行
src/serve/http_server.cpp               271 行
src/gateway/ctp/ctp_td_gateway.cpp      326 行
src/gateway/ctp/ctp_query_pipeline.cpp  234 行
```

规模本身不是问题；问题是单个文件同时承担了太多生命周期。

## 3. 前端发现

### 3.1 App.tsx 是主要复杂度热点

`App` 同时负责：行情流创建/重连、账户切换、REST 快照、下单/撤单、WASM 插件
生命周期、指标缓存、布局持久化、图表状态、URL/主题、Toast。

建议按**可测试控制器**拆分（不是简单拆组件）：

1. `useMarketStream(account)`：WebSocket 连接、订阅 diff、重连、状态；
2. `useTradingSession(account)`：REST 快照、WS diff 合并、下单/撤单；
3. `useIndicatorRuntime()`：WASM worker、参数、输出缓存、计算结果；
4. `useWorkspaceLayout()`：尺寸/拖动/localStorage；
5. `useChartData(symbol, period)`：K 线加载、live bar 合并、周期切换。

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

### 4.1 账户上下文已经基本收敛，但仍是手写

`account_api()` 已统一解析 `?account=`，HTTP REST/WS/task/risk 都能按账户路由。
下一步建议引入：

```text
AccountScope {
  account_id;       // resolved default if empty
  TqApi* api;
  TaskRegistry* tasks;   // per-account view
  RiskManager* risk;
}
```

由 `AccountRouter` 从 query 解析一次，handler 接收 `AccountScope`，而不是每个
handler 自己处理空账户、错误 404 和 task owner map。这样后续动态增删账户、账户
级指标/限流不会重复写。

### 4.2 WebSocket hub 映射是启动期静态的

现在 `HttpServer` 构造时按 `MultiAccountManager::account_ids()` 建 hub；node 当前
在构造前完成账户加载，所以可用。若未来运行中增删账户，需要：

- `AccountRouter` 提供 `add/remove_account` 回调；
- hub 生命周期跟随账户，而不是 HttpServer 构造；
- 关闭连接时按账户引用计数释放订阅。

这是**小规模重构**，不需要改协议。

### 4.3 路由文件仍可继续切分

`http_routes.cpp` 已经从 `http_server.cpp` 拆出，但 709 行内仍包含 catalog/
market/trading/option/task/risk 全部 handler。建议按资源拆成：

```text
src/serve/routes/catalog_routes.cpp
src/serve/routes/market_routes.cpp
src/serve/routes/trading_routes.cpp
src/serve/routes/task_risk_routes.cpp
```

`HttpServer::setup_routes()` 仍是声明表，handler 实现分文件；测试和编译边界更清楚。

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
2. `KLineChart` 纯映射层拆分；
3. `AccountRouter/AccountScope`；
4. 删除旧本地指标死代码，统一 WASM 入口。

### P1（需要验收）

5. WS hub 动态账户生命周期；
6. 路由按资源拆分；
7. 指标增量计算/Worker 池按 profile 决定；
8. 前端虚拟列表按实际行数决定。

### P2（协议/平台）

9. MessagePack/shm/binary WS 只在基线证明收益后做；
10. TLS/auth、systemd、日志审计仍属于生产化前置项，不是重构。

## 7. 验收标准

任何重构都必须同时满足：

- 原有 CTest 全量、Web typecheck/test/build、E2E、WASM ABI 脚本通过；
- 双账户 REST/WS/task/risk 隔离测试不退化；
- 24h soak 指标不出现新的 RSS/FD 增长；
- docs 同步，且不把“未执行的真实交易日/外部 CI”写成已验证。

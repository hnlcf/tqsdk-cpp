# TQSDK Board Web 前端

> 状态：当前唯一前端；Qt UI 代码已删除。
> 目录：`web/`；包名：`tqsdk-board`。

## 1. 技术栈

```text
SolidJS + TypeScript + Vite
pnpm（package manager）
TradingView Lightweight Charts v5（Apache-2.0）
node REST + WebSocket（/stream）
```

Node 使用 `nvm.fish`：

```fish
cd web
nvm use lts
pnpm install
pnpm dev
```

默认 Vite 代理 `/api` 到 `http://127.0.0.1:18081`；连接其他 node：

```fish
env VITE_PROXY_TARGET=http://127.0.0.1:18082 pnpm dev
```

## 2. 页面与功能

### 下单板

- 买开/卖开/平多（SELL CLOSE）/平空（BUY CLOSE）；
- 平仓按钮显示可平数量，无对应仓位时禁用；
- SHFE/INE/GFEX 的平今/平昨由 CTP 网关按当前持仓自动选择。

### 自定义指标

- 指标统一为 WASM 插件 + Worker + Manifest；MA/BOLL/MACD/RSI 已迁移；
- Wasm 只读历史 OHLCV，不提供 host imports/网络/DOM；
- 支持 line/histogram/area/band/markers 和副图；
- 默认启用 EMA 多均线（默认 6 条）和 MACD；其他插件在浮动列表中启用；
- 指标设置中可修改 manifest 参数，并显示插件版本；
- 指标启用状态、参数、输出颜色/线型/显隐、动态均线增删自动保存在浏览器 localStorage；
- 自选列表宽度、右侧栏宽度和持仓表高度可拖动，并保存到 localStorage；
- 详细方案见 [WEB_CUSTOM_INDICATORS.md](WEB_CUSTOM_INDICATORS.md)。

### 交易终端

- 自选列表：多列表、新建/重命名/删除、代码/名称模糊搜索、上下移动、localStorage；
- K 线：1m/5m/15m/30m/1h/2h/4h/1d/3d/1w；
- 分时：当前 China futures session、面积图、均价线、成交量、昨收；
- 指标：MA/BOLL/MACD/RSI；
- 盘口：5 档深度，点击价格填入下单板；
- 分笔成交：由 quote volume 增量生成；
- 三键下单：买开/卖开/平仓直接提交；价格类型支持限价/对价/市价/五档价；
  指令支持普通/FAK/FOK，数量条件支持 ANY/MIN 与最小成交手数；
  后端映射 IOC+ANY/ALL/MIN 并保留 advanced 审计字段；
- 持仓/委托/成交：REST + WS 合并；持仓多空合并为一行，
  显示合约名、最新价、开仓均价、保证金、浮动盈亏、浮盈比例、持仓市值、资金占比；
- 持仓实时性：quote 更新时前端重算浮动盈亏/市值，交易快照 5s 兜底对账；
- 账户权益：权益/可用/保证金/浮盈/风险度；权益按实时持仓浮盈修正，
  WS 快照与 5s REST 对账同步；
- `/readyz` 或 WS 未就绪时禁用报单。

### 期货列表

目录搜索、交易所筛选、可见行 quote 订阅、点击进入终端。
目录不可用时显示明确 MOCK/错误状态，不伪装成实盘。

### 期权链

产品/合约/到期选择、T 型报价、组合到期损益、期权与标的 K 线；
ATM 行权价由后端 Black-Scholes 计算结果驱动，选中合约显示 Greeks/IV。

### 多账户与图表稳定性

- 顶栏账户选择器从 `/accounts` 加载；切换账户时行情流重连到
  `/stream?account=<id>`，每个账户使用独立 WebSocket hub；
- 持仓/委托/成交同时保留 REST `?account=` 首次快照，WS snapshot/diff 负责后续
  增量；交易 topics 总是按当前账户订阅；
- WASM 输出按 chart+bars+参数缓存，颜色/线型/显隐变化不重跑数值计算；
- 持仓表使用稳定列宽和 tabular numbers，实时价格变化不会推动列宽；
- K 线切换合约/周期时只做一次适配缩放，实时更新保留用户缩放；
- K 线时间轴/十字光标按 China wall-clock 显示（UTC+8），不依赖浏览器时区。

### 运维

顶部「运维」入口（`?view=ops`）每 3 秒刷新：

- `/readyz` 的 md/td/phase/expected_disconnect；
- `/accounts` 多账户列表、默认账户、余额/可用/保证金/风险度；
- `/tick-cache/status` 的 accepted/written/pending/dropped；
- `/kline-cache/status` 的写入状态和文件路径；
- 常用 REST 端点速查。

## 3. 数据流

```text
node REST /quote /kline /trading-snapshot /catalog
        │
        ├─ 初始快照
        ▼
SolidJS App state
        ▲
        │
node WS /stream snapshot/diff
        │  topics: account/orders/trades/positions
        │  klines: symbol + duration_ns
        │
        ▼
自选/图表/盘口/买卖/持仓/委托/成交/权益
```

关键约束：

- WebSocket 订阅必须显式发送交易 topics；缺省只推行情；
- 断线指数退避重连，重连后重新订阅并依赖服务端 snapshot 恢复；
- K 线历史 REST + WS 增量合并，图表使用命令式 `series.update()`；
- 行情进入 Solid signals，但图表实例不随每个 tick 重建；
- REST 快照与 WS snapshot 合并，交易数据不使用 mock 冒充；
- 分时 session 固定使用 China Standard Time，不依赖浏览器时区。

## 4. 指标配置

- 指标设置弹层支持 MA 周期/颜色、BOLL 周期/倍数/颜色、
  MACD 快慢信号/颜色、RSI 周期/颜色；
- 配置写入 `localStorage`，并保留旧 v1 配置迁移；
- WASM 自定义指标路线和兼容边界见 [WEB_CUSTOM_INDICATORS.md](WEB_CUSTOM_INDICATORS.md)。

### 本地持久化

WebUI 使用 `localStorage`，不是 cookie，也不会把指标配置发往后端：

```text
tqsdk.wasm.indicators.v1   指标启用/参数/输出样式/动态均线
tqsdk.workspace.layout.v1  左侧宽度/右侧宽度/持仓表高度
tqsdk.theme                主题
tqsdk.watchlists.v2        自选列表
```

这些数据按浏览器 + origin 隔离；清理浏览器站点数据会恢复默认。指标参数不包含
账户、密码或 token。

## 5. K 线与 TradingView

当前使用 **Lightweight Charts**：

- Apache-2.0，自托管，TradingView 风格；
- 蜡烛/面积/柱状/线、十字光标、缩放、panes、primitive；
- 指标由 WASM 插件计算，再添加为 series；
- 已内置 EMA/MA/BOLL/MACD/RSI/ATR/Donchian/KDJ/CCI/OBV/VWAP；
- 已实现基础水平线/趋势线（localStorage 按 symbol|period 持久化）；
- 斐波那契/自由画线/编辑手柄仍需要 primitive/overlay；

TradingView **Advanced Charts / Charting Library** 提供完整指标和画线，但
是专有许可，不是开源依赖。若需要这些能力，应单独评估许可，不应默认打包。

## 6. 代码结构

```text
web/src/
  api/client.ts            REST/WS、重连、订阅 release
  hooks/useWorkspaceLayout.ts  布局拖动 + localStorage
  hooks/useWasmIndicators.ts   WASM 生命周期 + 计算缓存
  hooks/useMarketStream.ts     WS 账户连接/重连
  hooks/useChartData.ts        K 线 REST + live bar 合并
  hooks/useTradingSession.ts   账户/持仓/委托/成交/下单
  chart/data.ts                OHLCV 转换 + bar 合并纯函数
  chart/seriesMapping.ts       纯 candle/line/volume 映射
  chart/drawings.ts            画线类型/校验/序列点
  chart/KLineChart.tsx         K线 + WASM 指标 + panes + 画线
  chart/IntradayChart.tsx      分时
  chart/theme.ts               图表主题
  indicators/indicators.ts     分时/session 工具（旧指标计算已删除）
  components/                  自选/下单/盘口/分笔/交易表/顶栏
  pages/                       期货/期权
  lib/                         storage/format/trade merge
  styles/global.css
```

## 7. 测试

```fish
pnpm test
pnpm typecheck
pnpm build
```

当前 Vitest 覆盖（28 项）；Playwright E2E：

- 自选搜索 `SHFE.ao2701` 后大小写不变；
- 通过 1m/5m 周期切换、分时切换、图表缩放；
- 水平线/趋势线绘制、清除和刷新持久化；
- 账户选择器可见；
- 指标设置是浮动列表，点击插件进入详情；输出可单独配置显示、颜色、线型和线宽；
- EMA/MA 支持最多 8 个周期槽位的添加/移除。
- 默认输出图例包含 EMA5/10/20/60/120/200 和 MACD DIF/DEA/HIST；
- 启用 MA 后出现 MA5；关闭 MACD DIF 后对应输出消失；
- 刷新后指标参数/输出配置/启用状态保留；
- 拖动左侧 panel 后刷新，宽度保持；
- 分时 session、bar 合并、series 映射、layout、trade diff、画线序列有纯函数单测；
- WS 账户作用域、重连退避和重新订阅有 fake WebSocket 单测；
- KQ 主连 1d 原始/前复权/后复权 API 参数和 UI 选择已覆盖。

Vitest 覆盖：

- SMA/EMA/BOLL/MACD/RSI；
- China futures session 分时选择；
- China time 标签与 K 线 tick mark；
- 合约名/乘数/本地模糊搜索 fallback。

真实行情/下单验证需要 node 和网关；浏览器截图只能证明界面/渲染，不等于
CTP 验收。

## 8. 后续

- 高级画线（Fibonacci/自由画线/编辑手柄）；
- 指标 Worker/增量计算（已有全量 profile，0.65ms/11 插件）；
- 虚拟列表/列宽持久化；
- 期权希腊字母/波动率；
- WebSocket 压缩或二进制行情；
- 前端端到端 Playwright 测试。

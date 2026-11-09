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

### 交易终端

- 自选列表：多列表、新建/重命名/删除、代码/名称模糊搜索、上下移动、localStorage；
- K 线：1m/5m/15m/30m/1h/2h/4h/1d/3d/1w；
- 分时：当前 China futures session、面积图、均价线、成交量、昨收；
- 指标：MA/BOLL/MACD/RSI；
- 盘口：5 档深度，点击价格填入下单板；
- 分笔成交：由 quote volume 增量生成；
- 三键下单：买开/卖开/平仓直接提交；价格类型支持限价/对价/市价；
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

产品/合约/到期选择、T 型报价、组合到期损益、期权与标的 K 线。

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

## 4. K 线与 TradingView

当前使用 **Lightweight Charts**：

- Apache-2.0，自托管，TradingView 风格；
- 蜡烛/面积/柱状/线、十字光标、缩放、panes、primitive；
- 不内置指标和画线工具；
- 指标由 `src/indicators` 计算，再添加为 series；
- 当前实现 MA/BOLL/MACD/RSI 及当前价/昨收线；
- 完整趋势线/斐波那契/自由画线需要 primitive/overlay，或改用
  KLineChart（Apache-2.0，内置指标和 overlay）。

TradingView **Advanced Charts / Charting Library** 提供完整指标和画线，但
是专有许可，不是开源依赖。若需要这些能力，应单独评估许可，不应默认打包。

## 5. 代码结构

```text
web/src/
  api/client.ts            REST/WS、重连、订阅 release
  chart/KLineChart.tsx      K线 + 指标 + panes
  chart/IntradayChart.tsx  分时
  chart/theme.ts           图表主题
  indicators/indicators.ts MA/EMA/BOLL/MACD/RSI/session
  components/              自选/下单/盘口/分笔/交易表/顶栏
  pages/                   期货/期权
  lib/                     storage/format
  styles/global.css
```

## 6. 测试

```fish
pnpm test
pnpm typecheck
pnpm build
```

当前 Vitest 覆盖（8 项）：

- SMA/EMA/BOLL/MACD/RSI；
- China futures session 分时选择；
- China time 标签；
- 合约名/乘数/本地模糊搜索 fallback。

真实行情/下单验证需要 node 和网关；浏览器截图只能证明界面/渲染，不等于
CTP 验收。

## 7. 后续

- 画线 primitive/overlay；
- 指标 Worker（大数据量）；
- 虚拟列表/列宽持久化；
- 期权希腊字母/波动率；
- WebSocket 压缩或二进制行情；
- 前端端到端 Playwright 测试。

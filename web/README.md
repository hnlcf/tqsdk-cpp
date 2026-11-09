# TQSDK Board（tqsdk-board）

独立 Web 前端，SolidJS + TypeScript + Vite + pnpm，只通过 node 的 REST/WebSocket
访问数据，不链接 C++ 核心库。

## 运行

Node 使用 `nvm.fish` 管理：

```fish
cd web
nvm use lts
pnpm install
pnpm dev
```

默认 Vite 监听 `127.0.0.1:5173`，并把 `/api` 代理到
`http://127.0.0.1:18081`。连接其他 node：

```fish
env VITE_PROXY_TARGET=http://127.0.0.1:18082 pnpm dev
```

打开 <http://127.0.0.1:5173>。

生产构建和检查：

```fish
pnpm typecheck
pnpm test
pnpm build
pnpm preview
```

## 已实现功能

- 自选列表：多列表、新建/重命名/删除、代码/名称模糊搜索、上移/下移、localStorage 持久化、实时 quote；
- K 线：1m/5m/15m/30m/1h/2h/4h/1d/3d/1w，成交量、十字光标、当前价线；
- 分时：当前交易日 session，价格面积图、均价线、成交量、昨收线；
- 指标：MA5/10/20、BOLL、MACD、RSI，可用工具栏切换；
- 交易表：持仓/委托/成交，持仓由实时 quote 计算浮动盈亏；
- 盘口：5 档买/卖，点击价格填入下单板；
- 分笔成交：由 quote volume 增量生成，有界列表；
- 三键下单：买开/卖开/平仓直接提交，价格类型支持限价/对价/市价；
- 合并持仓：多空合并为持仓数/开仓均价，显示合约名、最新价、
  浮盈比例、持仓市值、资金占比；WS quote 实时重算；
- 委托、成交：REST/WS 合并，委托撤单；
- 账户权益：权益、可用、保证金、浮盈、风险度；5s REST 对账与 WS 快照同步；
- `/readyz` 门禁：未 ready 或 WS 断开时禁止报单；
- 深链：`?view=futures|options`、`?mode=time|kline`、`?theme=dark|light`、
  `?indicators=ma,boll,macd,rsi`。

## 目录

```text
web/
  src/
    api/client.ts          REST + WebSocket + 重连/订阅
    chart/KLineChart.tsx   K 线 + 指标
    chart/IntradayChart.tsx 分时
    indicators/            MA/EMA/BOLL/MACD/RSI
    components/            自选、下单、盘口、分笔、交易表、顶栏
    pages/                 期货列表、期权链
    lib/                   storage/format
    styles/global.css
  package.json
  pnpm-lock.yaml
  pnpm-workspace.yaml
  vitest.config.ts
```

## K 线与 TradingView

当前使用 **TradingView Lightweight Charts**：

- Apache-2.0，可自托管，视觉和交互接近 TradingView；
- 自带蜡烛/面积/柱状等 series、十字光标、缩放、panes、primitive 扩展；
- **不内置指标和画线工具**，所以 MA/BOLL/MACD/RSI 由本项目的
  `src/indicators` 计算后以 series 添加；
- 当前补齐了指标和当前价/昨收线；完整趋势线、斐波那契、自由画线需要
  primitive/overlay 扩展。

如果必须开箱即用大量指标和画线，可评估 **KLineChart**（Apache-2.0，
内置指标和 overlay）。TradingView **Advanced Charts / Charting Library**
虽然有 100+ 指标和 110+ 画线工具，但属于专有许可，不是开源库，不适合
直接作为本仓库的默认依赖。

参考：

- <https://github.com/tradingview/lightweight-charts>
- <https://www.tradingview.com/tradingview-for-the-web/>
- <https://github.com/klinecharts/KLineChart>

## 后端约定

`/stream` 订阅必须显式带上交易 topics：

```json
{
  "op": "subscribe",
  "symbols": ["SHFE.rb2701"],
  "klines": [{"symbol": "SHFE.rb2701", "duration_ns": 60000000000}],
  "topics": ["account", "orders", "trades", "positions"]
}
```

下单/撤单：

```text
POST   /order
DELETE /order/{order_id}
```

## 测试

```fish
pnpm test
pnpm typecheck
pnpm build
```

`pnpm test` 当前覆盖指标计算和分时 session 选择；真实行情/下单需要通过
node 运行验证，不能把浏览器 mock 当作实盘验收。

# TQSDK Web Terminal

SolidJS + Vite + TypeScript + TradingView Lightweight Charts 的期货终端原型。

行情/合约优先连接 node（`/api/catalog`、`/quote`、`/kline`、`/stream`）；
后端不可用时自动回退到本地 mock。交易账户连接后，下单/撤单走 node 的
`/order` API；没有交易网关时只显示提示，不会静默发送。

## 运行

```fish
cd web
nvm use lts
npm install
npm run dev
```

默认 Vite 把 `/api` 代理到 `http://127.0.0.1:18081`。连接其他 node：

```fish
env VITE_PROXY_TARGET=http://127.0.0.1:18082 npm run dev
```

打开 <http://127.0.0.1:5173>。

可直接深链到页面/主题：

```text
http://127.0.0.1:5173/?view=futures&theme=light
http://127.0.0.1:5173/?view=options&theme=dark
```

生产构建：

```fish
npm run build
npm run preview
```

## 目录

```text
web/
  apps/web/                 SolidJS 应用
    src/App.tsx             页面布局、实时账户/持仓/委托/下单面板
    src/chart/KLineChart.tsx Lightweight Charts 封装（跟随主题）
    src/api/                REST/WS client + catalog
    src/pages/              期货列表 / 期权链页面
    src/data/mock.ts        mock K 线/报价/持仓/委托/期货/期权
  apps/desktop/             Tauri 2 桌面壳（见 apps/desktop/README.md）
  package.json              npm workspace 根
```

## 后续接入

1. 将账户/持仓/委托/成交的 REST 初始快照接入为 WS 不可用时的兜底。
2. `/readyz` 未就绪时禁用下单。
3. 指标计算移到 Web Worker。
4. 增加 KLineChart 画线 primitive / 指标包。
5. 将 `tqsdk-node` 作为 Tauri 2 sidecar 打包，处理动态端口/本地 token。

完整路线见 [`../docs/WEB_FRONTEND.md`](../docs/WEB_FRONTEND.md)。

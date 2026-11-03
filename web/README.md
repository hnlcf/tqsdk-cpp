# TQSDK Web Terminal

Phase 0 静态原型：SolidJS + Vite + TypeScript + TradingView Lightweight Charts。

当前数据全部是本地 mock，不会连接 `/order`，也不会发送任何真实报单。

## 运行

```fish
cd web
nvm use lts
npm install
npm run dev
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
    src/App.tsx             页面布局、mock 持仓/委托/下单面板
    src/chart/KLineChart.tsx Lightweight Charts 封装（跟随主题）
    src/pages/              期货列表 / 期权链页面
    src/data/mock.ts        mock K 线/报价/持仓/委托/期货/期权
  package.json              npm workspace 根
```

## 后续接入

1. 把 `src/data/mock.ts` 替换为 `api-client`：
   - REST `/account`、`/positions`、`/orders`、`/kline`
   - WebSocket `/stream` diff
2. `/readyz` 未就绪时禁用下单。
3. 指标计算移到 Web Worker。
4. 增加 KLineChart 画线 primitive / 指标包。
5. 最后用 Tauri 2 sidecar 启动 `tqsdk-node` 做桌面壳。

完整路线见 [`../docs/WEB_FRONTEND.md`](../docs/WEB_FRONTEND.md)。

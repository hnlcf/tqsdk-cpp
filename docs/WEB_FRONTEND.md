# Web 行情/交易客户端技术路线调研

> **已废弃（2026-11）**：项目已放弃 Web/Tauri 前端，改为独立 Qt 6 Widgets
> 桌面客户端。新的设计、目录和 node 交互方式见
> [`UI_QT.md`](./UI_QT.md)。本文仅作为历史调研保留。


> 调研日期：2026-11-03。目标是先做 Web 版（K 线、盘口、持仓、下单），
> 后续用 Tauri 或原生框架做本地桌面客户端。本文给出选型、目录、
> 通信协议、性能策略和分阶段路线，不把前端技术栈写死在后端。

## 1. 结论先行

### 推荐方案

```text
Web 第一版：
  SolidJS + TypeScript + Vite
  + TradingView Lightweight Charts v5
  + solid-js/store（本地数据）
  + REST 快照 + WebSocket 增量 diff / 重连

桌面版：
  Tauri 2 + 同一套 Web UI
  + sidecar 启动 tqsdk-node
  + 本地 HTTP/WebSocket（复用现有协议）

原生 Qt：
  暂不作为第一实现；仅在高频/超低延迟/深度自定义渲染成为硬需求时评估。
```

### 为什么不是一开始就 Qt

- Qt 的 C++/QML 在延迟和系统集成上有优势，但 UI 迭代、图表生态、
  Web 复用和跨平台打包成本明显更高；
- 本项目后端已经稳定提供 REST + WebSocket，Web/Tauri 可以直接复用；
- 期货零售/半专业客户端的瓶颈通常在行情渲染、指标计算和网络抖动，
  不在“UI 是否原生”；
- 先用 Web 验证交互和指标，再把 Tauri 当壳，能避免把产品逻辑绑死在
  Qt 的 signal/slot 和 QML 上。

### 为什么不直接调用 C++ 核心库

桌面版第一版建议：

```text
Tauri WebView -> localhost HTTP/WS -> tqsdk-node -> IPC -> tqsdk-gw
```

而不是：

```text
Tauri WebView -> Rust FFI -> libtqsdk_core.so
```

原因：

1. **协议已经存在**：node 的 `/quote`、`/kline`、`/order`、`/stream` 已可用；
2. **进程隔离**：行情/交易/崩溃恢复边界清晰，前端崩溃不影响 CTP 会话；
3. **多语言可替换**：未来换 Qt、Web、移动端都复用同一后端；
4. **线程安全**：Tauri WebView 线程与 asio core strand 之间不直接共享状态；
5. **调试方便**：curl、WebSocket 客户端、浏览器 DevTools 都能直接排查。

只有在实测发现 WebSocket/JSON 成为硬瓶颈时，才考虑增加 **Rust/C++ FFI
的“行情直通”通道**，且应保留 HTTP/WS 作为降级路径。

## 2. K 线图表选型

### 2.1 TradingView Lightweight Charts

- Apache-2.0，Canvas 2D，TypeScript，体积小；
- 支持 candlestick/bar/line/area/histogram、十字光标、缩放、价格线、
  markers、panes、custom series/primitives；
- **不内置技术指标**（MA/MACD/KDJ/RSI/BOLL 需要自己计算并添加 series）；
- **不内置画线工具**（趋势线、矩形、斐波那契需要基于 primitive/plugin
  或自建 overlay）；
- 需要保留 TradingView attribution（`attributionLogo` 或页面链接）；
- v5 API 与 v3/v4 差异较大，网上旧示例不能直接抄。

适合：

- 想要接近 TradingView 的视觉和交互；
- 愿意自己维护指标计算 / 画线工具；
- 以 K 线展示和交易为主，不需要完整 TradingView 商业终端。

参考：

- <https://github.com/tradingview/lightweight-charts>
- <https://stockmarketstack.com/tools/lightweight-charts>
- <https://www.articsledge.com/post/tradingview-lightweight-charts-guide>

### 2.2 KLineChart

- Apache-2.0，Canvas，TypeScript，零依赖；
- 内置较多技术指标和画线模型，中文文档友好；
- 更接近“国产看盘软件”的功能集合；
- 视觉定制自由度低于 Lightweight Charts，包体更大。

适合：

- 首版就必须有 MACD/KDJ/RSI/BOLL 和画线工具；
- 不想自己实现指标与 drawing primitive；
- 能接受非 TradingView 风格。

参考：

- <https://github.com/klinecharts/KLineChart>
- <https://www.klinecharts.com>

### 2.3 建议

**分两层：**

1. **首版 K 线渲染用 Lightweight Charts**，先把行情/持仓/下单闭环做出来；
2. 指标计算放在前端 `packages/indicators`（Web Worker），画线工具作为
   `packages/chart` 的 primitive 逐步补齐；
3. 如果首版就需要“开箱即用的指标 + 画线”，则把 KLineChart 作为
   `packages/chart-kline` 的备选实现，不要让业务组件直接依赖某个图表库。

不要使用 TradingView Advanced Charts / Charting Library 作为默认方案：
它是闭源、申请制、有 watermark/bar 上限等限制，不适合本地开源项目。

## 3. SolidJS 还是 React

| 维度 | SolidJS | React 19 |
|---|---|---|
| 响应式 | 细粒度 signal/effect，更新路径短 | 组件级 re-render，需要 memo/compiler |
| 生态 | 图表/表格/路由/测试库较少 | 最大，生态最成熟 |
| 学习曲线 | JSX 风格接近 React，但无 hooks 依赖数组 | hooks/闭包心智负担更重 |
| TradingView 集成 | 直接用 vanilla API 封装，反而简单 | 社区封装多，但封装层也可能引入重渲染 |
| 复杂交易 UI | 表格/虚拟列表需要更多自研 | 现成方案多 |
| 打包/运行时 | 更小、启动更轻 | 更大 |

本项目选择 **SolidJS + TypeScript**，原因是：

1. 你本人更熟悉 SolidJS，长期维护成本最低；
2. 交易客户端的高频更新主要是行情/持仓数据，Solid 的细粒度更新更自然；
3. K 线由 Lightweight Charts 自己用 Canvas 渲染，不需要依赖 React 的图表封装；
4. 交易 UI 的表格/表单规模可控，不需要为了生态引入 React。

需要接受的代价：

- Solid 的组件库、表格、虚拟列表、测试工具比 React 少；
- 复杂通用组件需要自己写或做少量适配；
- 图表必须按 vanilla API 封装，不要把每 tick 数据塞进 Solid store 触发全树更新。

实现原则：

- 图表实例放 `let chart`/普通变量，用命令式 `series.update()`；
- 行情/持仓使用 `createStore` + 细粒度 selector，避免整页重渲染；
- 指标计算移到 Web Worker；
- 只让“当前报价/持仓摘要”进入响应式 UI，K 线数据走图表实例。

## 4. Tauri 2 vs Qt

### 4.1 对比

| 维度 | Tauri 2 | Qt 6 |
|---|---|---|
| UI | HTML/CSS/TS，复用 Web | C++/QML，学习曲线陡 |
| 渲染 | 系统 WebView | Qt Quick/RHI 或 Widgets |
| 包体 | 通常几 MB～十几 MB | 几十 MB 起 |
| 内存 | 较低（WebView 共享） | 低，但 Qt 模块多时也不小 |
| 启动 | 快 | 快 |
| 图表 | Lightweight/KLineChart | QtCharts/自绘，TradingView 风格弱 |
| 开发效率 | 高，Web 生态 | 中低，C++ 调试成本高 |
| 系统集成 | Tauri plugin/sidecar | 最强，原生 API 直接 |
| 许可 | MIT/Apache-2.0 | LGPL（动态链接）/GPL/商业 |
| 适用 | 90% 桌面交易客户端 | 工业/嵌入式/极低延迟/深度自绘 |

参考：

- <https://v2.tauri.app/>
- <https://rustify.rs/articles/rust-tauri-vs-qt-2026>
- <https://forasoft.com/blog/article/electron-desktop-app-development-guide-for-business>

### 4.2 建议

- **默认 Tauri 2**：Web 先行，桌面壳后加；
- **Qt 作为备选**：如果出现以下硬需求再评估：
  - 报单/行情端到端 p99 需要稳定在个位数毫秒；
  - 需要直接操作 CTP API 或共享内存行情；
  - 需要复杂自绘（深度图、订单簿热力图、专业画线）且 WebView 性能不够；
  - 团队本身是 C++/Qt 团队。
- 如果选 Qt，UI 仍放在 `web/` 还是 `desktop/`？
  - 不建议把 Qt 和 Web 混在同一可执行文件里；
  - 建议 `desktop/` 独立 Qt 客户端，复用 `tqsdk-node` 的 HTTP/WS；
  - 只有确认需要直连核心库时，才让 Qt 链接 `libtqsdk_core.so`，
    并保持与 IPC 相同的命令/事件语义。

## 4.3 本地开发环境约定

- Node 使用 nvm 管理；进入 `web/` 后先执行 `nvm use lts`；
- 包管理器使用 **pnpm**（通过 corepack 固定版本），提交 `pnpm-lock.yaml`；
- 不把 `node_modules/`、`dist/` 提交到仓库；
- 前端构建与 C++ CMake 构建完全解耦，CI 可分两条流水线；
- 如果后续采用 Tauri，Rust 工具链单独固定版本，不要求 C++ 构建环境。

## 5. 目录结构建议

```text
tqsdk_cpp/
  web/
    package.json                 # npm workspace
    package-lock.json
    apps/
      web/                       # SolidJS + Vite Web 客户端
        src/
          api/                       REST/WS client
          pages/
          widgets/
          chart/                     Lightweight Charts 封装
          styles/
      desktop/                   # Tauri 2 桌面壳（脚手架）
        src-tauri/
          src/
          tauri.conf.json
    packages/
      api-client/                # REST/WS 类型、重连、seq/resume
      chart/                     # Lightweight Charts 封装
      indicators/                # MA/EMA/MACD/KDJ/RSI/BOLL...
      domain/                    # Quote/Order/Trade/Position 类型
      ui/                        # 设计系统/表格/表单
  # 现有 C++ 后端保持不动
```

要点：

- `web/` 不把 C++ 头文件或 CMake 引入前端构建；
- `api-client` 只依赖 JSON 协议，未来可被 Tauri/Qt/移动端复用；
- `chart` 与 `indicators` 解耦，指标可在 Worker 中跑；
- 桌面壳只做窗口、托盘、自动更新、sidecar 生命周期。

## 6. 前端与后端通信

### 6.1 当前可用接口

REST：

```text
GET  /healthz
GET  /readyz
GET  /metrics
GET  /quote/{symbol}
GET  /kline/{symbol}/{duration_ns}
GET  /kline-cache/status
GET  /account
GET  /orders
GET  /trades
GET  /positions
POST /order
DELETE /order/{order_id}

GET  /catalog/status
GET  /catalog/instruments?classes=FUTURE,FUTURE_CONT&exchange=SHFE&q=rb
GET  /catalog/instruments/{instrument_id}
GET  /catalog/options/products
GET  /catalog/option-chain/{underlying}
WS   /stream
```

合约目录来自 node 在空闲时下载的 `symbol.json` / `latest.json`，详见
[`docs/INSTRUMENTS.md`](./INSTRUMENTS.md)。

WebSocket 客户端消息：

```json
{"op":"subscribe","symbols":["SHFE.rb2701"],"klines":[{"symbol":"SHFE.rb2701","duration_ns":60000000000}],"topics":[]}
{"op":"ping"}
```

服务端 diff 形状与 C++ `DataManager` 一致；前端应把它当成**增量 patch**，
不要每次重建整个 store。

### 6.2 首屏流程

```text
1. GET /readyz（503 则显示“交易网关恢复中”，禁用下单）
2. GET /account /positions /orders /trades
3. WS connect
4. WS subscribe（自选合约 + 当前周期 K 线）
5. 收到 diff 后合并到本地 store
6. 用户切合约/周期时只发新的 subscribe（或 release+subscribe）
```

### 6.3 断线与重连

- WebSocket 断开后指数退避重连；
- 重连成功后重新拉 REST 快照，再恢复 subscribe；
- 不要只依赖 WS 补帧：交易状态以 REST/快照为准；
- 下单按钮在 `/readyz` 非 200 或 WS 未连接时禁用；
- 订单提交使用后端返回的 `order_id`，前端不要自行生成 rid；
- 后续如需“点击两次不重复下单”，增加 `client_order_id` 幂等键。

## 7. 性能策略

1. **K 线增量更新**：历史 `setData()` 一次，tick 只 `series.update()`；
2. **rAF 批处理**：同一帧内多个 tick 合并成一次 update；
3. **指标 Worker**：MA/MACD/KDJ 在大数据量下放 Web Worker，避免阻塞 UI；
4. **虚拟列表**：订单/成交/持仓表用虚拟滚动；
5. **图表实例命令式**：Solid 只管理容器和生命周期，不让每 tick 触发整页 render；
6. **WS 限流**：前端按 30～60Hz 合并行情；交易回报不合并；
7. **桌面端**：Tauri 只做壳，不让 IPC 进入每 tick 热路径；WebSocket 仍走本地回环。

## 8. 分阶段路线

### Phase 0：静态原型（已完成，2026-11-03）

- Vite + SolidJS + TS；
- 用 mock 数据画 K 线 + 成交量 + 持仓表；
- 验证 Lightweight Charts v5 的 API、主题、响应式；
- 不接真实交易；
- Header 导航：交易终端 / 期货列表 / 期权链 / 研究（预留）；
- dark/light 主题切换，偏好写入 localStorage；
- 期货列表支持搜索、交易所筛选、可见行实时 quote 订阅和点击进入终端；
- 期权链支持交易所/品种/合约/到期选择、T 型报价勾选、组合损益、
  选中期权和标的 K 线；
- 终端账户/持仓/委托/成交通过 `/stream` topics 订阅，WS 快照/增量合并；
- 下单/撤单走 `/order` REST，失败保留错误提示并回退 mock 展示。

落地位置：

```text
web/apps/web/src/App.tsx
web/apps/web/src/chart/KLineChart.tsx
web/apps/web/src/data/mock.ts
```

运行：

```fish
cd web
nvm use lts
npm install
npm run dev
```

`npm run build`（含 `tsc --noEmit`）已通过；当前行情/合约/账户/持仓/
委托/成交已接 node，账户或网关不可用时回退 mock。下单/撤单使用
`POST /order`、`DELETE /order/{id}`，委托状态由 WS 快照/增量刷新。

### Phase 1：只读行情客户端（3～5 天）

- 接 `/quote`、`/kline`、`/stream`；
- 合约搜索/自选、周期切换、十字光标、成交量；
- WS 重连 + REST 快照恢复；
- `/readyz` 状态条。

### Phase 2：交易面板（3～5 天）

- 账户/持仓/订单/成交；
- 下单/撤单表单，二次确认；
- 错误提示、拒单原因、订单状态机；
- 交易时段/只读模式。

### Phase 3：Tauri 桌面壳（3～5 天）

- sidecar 启动 `tqsdk-node`；
- 动态端口 + 本地 token；
- 托盘、窗口状态、自动更新；
- 打包 `libtqsdk_core.so` + CTP `.so` + node。

### Phase 4：专业图表（按需）

- 指标库、画线工具、布局保存；
- 多周期/多合约对比；
- 深度图/成交明细；
- 若 WebView 性能不足，再评估 Qt 或 Rust 直通行情。

## 8.1 Tauri 桌面壳状态

已添加 `web/apps/desktop` Tauri 2 脚手架：

- 复用 `@tqsdk/web` 的 Vite UI；
- dev URL `http://127.0.0.1:5173`；
- 生产 `frontendDist` 指向 `web/apps/web/dist`；
- 暂不打包 sidecar/图标，`bundle.active=false`。

本机验证结果：

```text
tauri info
  webkit2gtk-4.1: not installed
  rsvg2: not installed
  rustc/cargo: OK
  @tauri-apps/cli: 2.12.4
```

Fedora 需先安装：

```bash
sudo dnf install webkit2gtk4.1-devel gtk3-devel libsoup3-devel librsvg2-devel
```

然后：

```fish
cd web
nvm use lts
npm --workspace @tqsdk/desktop run dev
```

下一步再接 `tqsdk-node` sidecar、动态端口和本地 token。

## 9. 待验证问题

- [ ] Lightweight Charts v5 在 5 万根 1 分钟 K 线 + 5 个指标下的帧率；
- [ ] WebKitGTK（Linux Tauri）与 WebView2（Windows）的渲染差异；
- [ ] WebSocket JSON diff 在 1000 tick/s 下的 CPU/内存；
- [ ] 指标 Worker 的传输成本（Transferable/SharedArrayBuffer）；
- [ ] 画线工具 primitive 的实现工作量；
- [ ] Tauri sidecar 的启动/退出/端口冲突处理；
- [ ] 是否需要在 node 增加 `client_order_id` 幂等字段；
- [ ] 前端断线时未完成订单的恢复策略。

## 10. 参考链接

- TradingView Lightweight Charts: <https://github.com/tradingview/lightweight-charts>
- Lightweight Charts 能力/限制综述: <https://stockmarketstack.com/tools/lightweight-charts>
- Lightweight Charts 实践指南: <https://www.articsledge.com/post/tradingview-lightweight-charts-guide>
- KLineChart: <https://github.com/klinecharts/KLineChart>
- KLineChart 文档: <https://www.klinecharts.com>
- Tauri 2: <https://v2.tauri.app/>
- Tauri vs Qt（2026）: <https://rustify.rs/articles/rust-tauri-vs-qt-2026>
- Electron/Tauri/Qt 对比: <https://forasoft.com/blog/article/electron-desktop-app-development-guide-for-business>

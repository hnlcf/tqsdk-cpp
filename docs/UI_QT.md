# TQSDK Qt UI 设计方案

> 状态：设计/骨架阶段
>
> 结论：放弃 Web/Tauri 前端，采用 **独立进程的 Qt 6 Widgets 桌面客户端**；
> UI 只通过 node 已有的 **REST + WebSocket** 访问数据，不链接 `tqsdk_core`。

## 1. 技术选型

### 1.1 Qt Widgets，而不是 QML

第一阶段选择 **Qt Widgets 6 + 自绘 QPainter 图表**：

- 交易终端以表格、Dock、Splitter、快捷键为主，Widgets 更成熟；
- K 线需要自定义坐标、十字光标、密集重绘，QPainter/Widget 控制力更直接；
- 不引入 QML/Quick 的 GPU/渲染后端差异，Linux 远程/X11/Wayland 部署更简单；
- Qt Designer/代码布局均可，调试和性能分析路径熟悉；
- 后续若需要更现代的动效，可局部引入 Qt Quick，不影响网络/模型层。

不使用 QtCharts 作为主 K 线控件：它更适合通用统计图，交易终端的十字光标、
多周期切换、成交量联动和自定义叠加指标需要单独实现。

### 1.2 独立前端进程

```text
┌─────────────────────┐         REST / WebSocket        ┌─────────────────────┐
│  tqsdk-ui (Qt)      │  ─────────────────────────────▶ │  tqsdk-node         │
│  行情/图表/交易面板  │  ◀───────────────────────────── │  TqApi + DataManager │
└─────────────────────┘                                └─────────────────────┘
                                                               │ IPC
                                                               ▼
                                                        tqsdk-gw / CTP
```

UI **不链接** `tqsdk_core`，原因：

- UI 与 C++ 核心可以独立升级、独立崩溃恢复；
- 避免 Qt/Boost/CTP 的 ABI、编译选项、动态库路径互相污染；
- UI 可以连接本机、另一台机器或未来远端 node；
- node 已经是权威数据源，UI 不应复制数据面合并逻辑。

代价是多一次进程间 JSON 序列化；本地 HTTP/WS 的开销相对 UI 绘制可以接受。
若未来极端追求低延迟，可增加 Unix Domain Socket 的 WS-like 通道，但协议仍是
现有 `subscribe/snapshot/diff`，不把核心库链进 UI。

## 2. 与 node 的数据交互

### 2.1 传输选择

| 方式 | 用途 | 结论 |
|---|---|---|
| REST | 合约目录、历史 K 线、账户/持仓/委托快照、下单/撤单 | 使用 |
| WebSocket `/stream` | quote/K 线/交易实体增量、重连快照 | 使用 |
| 直接 IPC | node/gw 内部协议 | 不暴露给 UI |
| 直接链核心库 | 单进程嵌入 | 不做 |

对应 Qt 组件：

```text
QNetworkAccessManager  -> REST
QWebSocket             -> /stream
QJsonDocument/QJsonObject -> 协议编解码
QAbstractTableModel/QSortFilterProxyModel -> 表格
QWidget + QPainter     -> K 线
QSettings              -> node URL、主题、布局
```

### 2.2 启动与连接

```text
QSettings / CLI: node_url = http://127.0.0.1:18081
        │
        ├─ GET /readyz
        ├─ GET /catalog/status
        ├─ GET /catalog/instruments?limit=...
        └─ WebSocket ws://127.0.0.1:18081/stream
                 │
                 ├─ welcome
                 ├─ subscribe {symbols, klines, topics}
                 ├─ subscribed
                 └─ snapshot
```

WebSocket URL 由 `node_url` 推导：

```text
http://host:port  -> ws://host:port/stream
https://host:port -> wss://host:port/stream
```

`/readyz` 不 ready 时：

- 可以显示连接/登录状态；
- 禁止下单/撤单；
- 行情页面可以继续显示本地缓存或最后快照。

### 2.3 WS 订阅策略

UI 只订阅“当前可见、当前有用”的数据：

- 当前选中合约：quote + 当前周期 K 线；
- 自选列表可见合约：quote；
- 期货列表：只订阅当前视口约 50~100 行；
- 期权链：当前交易所/品种/标的的 quote 和选中合约 K 线；
- 交易实体：account/orders/trades/positions。

订阅消息沿用 node 现有协议：

```json
{
  "op": "subscribe",
  "symbols": ["SHFE.rb2701"],
  "klines": [
    {"symbol": "SHFE.rb2701", "duration_ns": 60000000000, "view_width": 200}
  ],
  "topics": ["account", "orders", "trades", "positions"]
}
```

收到 `diff` 后：

- quote 进入 `QuoteStore`，按 symbol 合并；
- K 线按 `reset/bars` 更新 `KlineSeries`；
- 交易实体按 id 合并；
- **不直接在每个 diff 里调用 `update()`**，由 60Hz/30Hz 合并定时器统一刷新视图，
  避免高频行情把 GUI 事件循环打满。

### 2.4 REST 使用

```text
GET  /readyz
GET  /catalog/status
GET  /catalog/instruments
GET  /catalog/options/products
GET  /catalog/option-chain/{underlying}
GET  /quote/{symbol}
GET  /kline/{symbol}/{duration_ns}
GET  /account /orders /trades /positions
POST /order
DELETE /order/{id}
GET  /kline-cache/status
```

大合约目录（2.9 万条 symbol）解析不要阻塞 GUI：

- `QNetworkReply` 完成后只拿 `QByteArray`，不解析；
- 通过 `JsonParserWorker`（独立 QThread）解析为紧凑 `QVector<Instrument>`；
- 完成后通过 queued signal 回 GUI 线程替换模型；
- 解析失败保留上一版目录。

### 2.5 重连与恢复

```text
connected
  -> 发送当前 symbols/klines/topics 的 subscribe
  -> 等待 subscribed + snapshot
  -> 进入 streaming

disconnected
  -> 状态栏显示“行情断开”
  -> QTimer 指数退避 1s, 2s, 4s ... 30s
  -> 重连成功后重新 subscribe
  -> 用 snapshot 替换本地市场截面

REST 失败
  -> 保留旧数据
  -> 错误进入状态栏/日志，不弹阻塞式对话框
```

order/quote 请求使用 `QNetworkReply` 的取消和超时；不阻塞 GUI 线程。

## 3. UI 架构

### 3.1 目录

```text
ui/
  CMakeLists.txt
  README.md
  src/
    main.cpp
    app/
      MainWindow.{h,cpp}
      AppController.{h,cpp}
      ThemeManager.{h,cpp}
    net/
      NodeRestClient.{h,cpp}
      NodeStreamClient.{h,cpp}
      Protocol.{h,cpp}
      NodeSettings.{h,cpp}
    model/
      InstrumentModel.{h,cpp}
      QuoteStore.{h,cpp}
      MarketTableModel.{h,cpp}
      KlineSeries.{h,cpp}
      AccountModel.{h,cpp}
      PositionModel.{h,cpp}
      OrderModel.{h,cpp}
      TradeModel.{h,cpp}
    chart/
      KLineChartWidget.{h,cpp}
      ChartTheme.{h,cpp}
    pages/
      TerminalPage.{h,cpp}
      FuturesPage.{h,cpp}
      OptionsPage.{h,cpp}
    widgets/
      WatchlistWidget.{h,cpp}
      OrderTicketWidget.{h,cpp}
      StatusBarWidget.{h,cpp}
  resources/
    qss/dark.qss
    qss/light.qss
  tests/
    ProtocolTest.cpp
    ModelTest.cpp
```

### 3.2 主窗口布局

```text
┌────────────────────────────────────────────────────────────────────┐
│ Toolbar: 合约搜索 | 周期 1m..1w | 连接状态 | 主题                   │
├───────────────┬──────────────────────────────────────┬─────────────┤
│ Watchlist     │                                      │ Order Entry │
│ 自选/最近合约 │             KLineChart               │ 下单/撤单   │
│               │                                      │             │
├───────────────┴──────────────────────────────────────┴─────────────┤
│ 持仓 / 委托 / 成交 / 账户 / 持仓明细（各自 Dock）                   │
├────────────────────────────────────────────────────────────────────┤
│ Status: node URL | ws 状态 | 延迟 | 行情时间 | readyz                │
└────────────────────────────────────────────────────────────────────┘
```

- 使用 `QMainWindow` + `QDockWidget` + `QSplitter`，允许布局保存/恢复；
- 主页面用 `QStackedWidget`：交易终端、期货列表、期权链；
- 表格统一 `QTableView + QAbstractTableModel`，便于排序和虚拟化；
- 期权 T 型报价单独 `QAbstractTableModel`，CALL/PUT 各一组列；
- 底部持仓/委托/成交用 `QTabWidget`，避免窗口过宽。

### 3.3 自绘 K 线控件

第一阶段能力：

- 蜡烛图 + 成交量；
- 十字光标、当前价线；
- 鼠标滚轮缩放、拖拽平移；
- 1m/5m/15m/30m/1h/2h/4h/1d/3d/1w 周期；
- 主题切换；
- 数据更新时保持视口，只有新 bar/重置时才重新 fit；
- 高 DPI 适配。

绘制策略：

```text
paintEvent:
  - 计算可见 bar 区间
  - 绘制网格/坐标
  - 绘制 K 线 + 成交量
  - 绘制十字光标/当前价
  - 不在 paintEvent 中做网络/JSON 解析
```

数据更新：

- `QuoteStore`/`KlineSeries` 只做数据；
- `KLineChartWidget` 用 `QTimer` 合并重绘；
- 不每 tick 调 `repaint()`，使用 `update()` 合帧。

## 4. 线程与对象生命周期

- GUI、QNetworkAccessManager、QWebSocket 在主线程；socket 只负责收包；
- WebSocket/Market JSON、目录、K 线历史、期权链、quote/account/order/trade REST 响应全部由 `JsonParserWorker` 在独立线程解析；
- 市场增量在 worker 内约 33ms 合帧后回主线程，UI 每帧最多处理一次；主线程不做 JSON 解析；
- futures/options 表格不可见时不刷新，显示时用最新 quote 缓存补齐；
- InstrumentTableModel 使用 symbol->row 索引，只对变化行发 dataChanged；
- 所有 `QObject` 父子关系明确：`MainWindow -> AppController -> NodeClient`；
- `QWebSocket` 的信号连接使用 `Qt::QueuedConnection` 或同线程直连；
- `NodeRestClient` 持有 `QNetworkAccessManager`，reply 完成用 `deleteLater()`；
- 重连 timer 归 `NodeStreamClient` 所有；
- 退出时：停止 timer、abort replies、`QWebSocket::close()`，再析构窗口。

## 5. CMake 与 Preset

### 5.1 根构建

根 `CMakeLists.txt` 增加：

```cmake
option(TQSDK_BUILD_UI "Build Qt desktop UI" OFF)
if(TQSDK_BUILD_UI)
    add_subdirectory(ui)
endif()
```

- 默认 **OFF**：现有核心/gw/node/package 构建不依赖 Qt；
- `ui/CMakeLists.txt` 只在 UI 开启时 `find_package(Qt6 ...)`；
- `tqsdk-ui` **不链接** `tqsdk_core`，只链接 Qt；
- UI 目标安装到 `bin/tqsdk-ui`；
- 核心包和 UI 包可以独立构建、独立升级。

### 5.2 Preset

新增两个 configure preset：

```text
ui-dev       RelWithDebInfo，TQSDK_BUILD_UI=ON，TQSDK_BUILD_TESTS=ON
ui-package   Release，TQSDK_BUILD_UI=ON，TQSDK_BUILD_TESTS=OFF
```

现有 `default/package` 保持 UI=OFF，避免 Qt 尚未安装时影响后端开发。

### 5.3 Qt 依赖

vcpkg 需要 Qt 6：

```json
{
  "name": "qtbase",
  "features": ["wayland"]
},
"qtwayland",
"qtwebsockets"
```

`qtwayland` 提供 Wayland QPA/client 模块；`qtbase[wayland]` 是其构建前提。
`qtbase` 默认仍保留 XCB，因此 Wayland/XWayland 可以共存。

静态 Qt 不会自动把 Wayland platform plugin 链入可执行文件，`ui/CMakeLists.txt`
显式导入：

```cmake
qt_import_plugins(tqsdk-ui INCLUDE Qt6::QWaylandIntegrationPlugin)
```

至少需要：

```text
Qt6::Core
Qt6::Gui
Qt6::Widgets
Qt6::Network
Qt6::WebSockets
```

配置阶段需要确认 `qtwebsockets` 和 Wayland plugin 都存在。

## 6. 实施阶段

### Phase 0：骨架（已完成）

- `ui/` 目录和最小 `QMainWindow`；
- `NodeClient` 传输无关接口；
- `HttpWsNodeClient` 的 HTTP/WS 基础实现；
- CMake 子目录和 preset；
- 文档、目录约定；
- Qt 6.11.2 + Wayland 静态链接验证通过。

### Phase 1：只读行情（已完成，2026-11）

- 自选/最近行情表和 `QuoteTableModel`；
- `InstrumentTableModel` 合约目录表，支持搜索和双击切换；
- `MarketStore` 合并 quote 和 K 线，支持 reset/append/update/裁剪；
- `KLineChartWidget`：QPainter 蜡烛、成交量、坐标、当前价和十字光标；滚轮缩放、左键拖动，头部显示价格/涨跌/涨幅；
- 周期切换（1m/5m/15m/30m/1h/2h/4h/1d/3d/1w）及 REST 历史 + WS 增量；
- 深色/浅色主题；
- Wayland 原生运行，XCB fallback；
- QtTest 协议和模型测试。

### Phase 2：期权与交易（基础版已完成，2026-11）

- 期权链 CALL/PUT 表格，双击加入自选；
- 右侧行情快照 + 分笔成交上下可拖动组件；分笔为有界最新优先队列，当前由 quote volume 增量生成；
- 底部独立的“三键下单”面板：买开、卖开、平仓；限价单、直接提交；dock 面板可关闭/浮动；
- 交易面板合约输入与自选/图表同步，支持模糊搜索和回车切换；
- 账户、持仓、委托、成交表；REST 快照与 WS diff 合并；
- 合约/持仓/委托/成交表右键显示/隐藏列，选择持久化到 QSettings；
- node ready + 账户加载门禁；`/readyz` 仍由 node 侧负责。

### Phase 3：工程质量

- QtTest 协议/模型测试；
- fake REST/WS node server；
- 连接断开、慢行情、异常 JSON、超大合约目录测试；
- Linux/Windows/macOS Qt 运行时打包。

### 工作区组件化

- 自选、期货、期权、行情快照、分笔、持仓/委托/成交、三键下单全部使用 QDockWidget；
- 每个 dock 可拖动、浮动、关闭，关闭后从“组件”菜单重新打开；
- 布局通过 QSaveState/QSettings 持久化，窗口几何、dock 尺寸、图表合约/周期/可见区间、表格列显示/宽度/顺序都会保存；
- 表头支持左右拖动列，列宽可拖动调整；
- 所有 dock 不设置人为最小宽度，自选列表默认窄宽度；
- 表格统一复用 `TablePanel`（选择行为 + 右键列配置 + QSettings）；
- 图表为中央组件，可从“视图 / 组件 → 图表弹出 / 收回”独立窗口显示。

## 7. 指标插件

指标插件（DSL 优先、Python sidecar 第二阶段）设计见
[`INDICATOR_PLUGINS.md`](./INDICATOR_PLUGINS.md)。

## 8. 打包

- `tqsdk-ui` 作为独立可执行文件安装到 `bin/`；
- node/gw 仍由核心包提供；
- Linux 首版可依赖系统/vcpkg Qt 动态库，后续用 Qt deploy 工具或 AppImage 打包；
- Windows 使用 `windeployqt`；
- macOS 使用 `macdeployqt`；
- UI 的配置（node URL、主题、布局）放 `QSettings`，不写死 build 路径。

## 9. 不建议的方案

- **UI 直接链接 `tqsdk_core`**：编译链复杂、ABI 耦合、崩溃互相影响；
- **UI 直接走 gw 的二进制 IPC**：需要把 IPC 协议和 CTP/Tq 网关生命周期带进 UI；
- **REST 轮询代替 WS**：高频行情延迟和请求量不可接受；
- **把所有合约一次性订阅**：node 和 UI 都会被推送打满；
- **QML 作为第一阶段主框架**：图表/表格交互成本高，先 Widgets 稳定落地。


## 10. 内部架构与测试

各组件、线程模型、数据流、持久化键和测试 target 见
[`UI_ARCHITECTURE.md`](./UI_ARCHITECTURE.md)。

## UI ↔ node 传输优化

- REST `GET /quotes?symbols=...`：批量 quote 快照；
- REST `GET /trading-snapshot`：合并交易快照；
- 交易确认后不再发四次独立请求；
- WebSocket `/stream` 支持 `coalesce_ms`，live 配置 10ms，服务端在 core strand
  上合并变化后发送单帧 diff；
- quote 静态字段来自 catalog，避免 `price_tick=null`。

## WebSocket 订阅语义

- `subscribe` 不带 `topics` 或 `topics: []`：只推送 symbols/klines，不推 account/orders/trades/positions；
- 需要交易数据时显式发送 `"topics":[...]`；
- 同一订单/成交/持仓 JSON 未变化时，服务端不会在每个 CTP 查询周期重复广播；
- 首帧 `snapshot` 仍会返回当前订阅的完整实体。

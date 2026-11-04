# Qt UI 架构、组件与数据流

本文描述 `tqsdk-ui` 进程的内部结构。目标是让 UI 独立于后端 C++ 核心：

- UI 不链接 `tqsdk_core`；
- UI 只通过 `NodeClient` 抽象访问 node；
- 所有入站 JSON 解析在独立线程完成；
- UI 主线程只处理强类型数据、模型更新、绘制和用户输入。

## 1. 进程与分层

```text
┌─────────────────────────────────────────────────────────────┐
│ tqsdk-ui (Qt process)                                       │
│                                                             │
│  MainWindow / Dock widgets / KLineChartWidget               │
│          │                                                  │
│          ▼                                                  │
│  Table models + MarketStore + Trading models                 │
│          │                                                  │
│          ▼                                                  │
│  NodeClient (abstract)                                      │
│          │                                                  │
│          ▼                                                  │
│  HttpWsNodeClient                                           │
│    ├─ QNetworkAccessManager / QWebSocket  (GUI thread)       │
│    └─ JsonParserWorker                    (worker QThread)   │
└─────────────────────────────────────────────────────────────┘
              │ REST / WebSocket
              ▼
┌─────────────────────────────────────────────────────────────┐
│ tqsdk-node                                                  │
│   TqApi / DataManager / REST / WS                           │
└─────────────────────────────────────────────────────────────┘
```

### 1.1 扩展边界

`NodeClient` 是前端唯一的数据/交易入口。未来增加 UDS、二进制协议或嵌入式
实现时，页面和模型不需要知道传输层：

```cpp
class MyNodeClient final : public NodeClient {
    // connectToNode / setSubscription / request* / placeOrder ...
};
```

`HttpWsNodeClient` 是当前实现；`MainWindow` 只依赖 `NodeClient*`。

## 2. 线程模型

### 2.1 GUI 线程

以下对象和操作位于 Qt GUI 线程：

- `QApplication`、`MainWindow`、所有 `QWidget`；
- `QNetworkAccessManager`；
- `QWebSocket`；
- `QAbstractTableModel` 的修改和 `dataChanged`；
- `KLineChartWidget::paintEvent`；
- `QSettings` 写入和 dock 布局保存；
- 交易按钮、鼠标滚轮/拖动等输入处理。

GUI 线程**不执行入站 JSON 解析**。

### 2.2 JsonParserWorker

`ui/src/net/JsonParserWorker.{h,cpp}` 运行在独立 `QThread` 中。

输入：

```text
WS raw QByteArray
REST raw QByteArray (catalog/kline/option/quote/account/orders/...)
```

输出：

```text
MarketUpdate
QVector<Instrument>
QVector<KlineBar>
OptionChain
Quote / Account / OrderAck
```

市场更新在 worker 内按约 33ms 合并：

```text
WS messages ──► parse ──► pending MarketUpdate
                              │
                              ├─ quote 按 symbol merge
                              ├─ KlineFrame 按 symbol/duration merge
                              └─ 交易快照追加
                              │
                       33ms timer
                              ▼
                  一次 queued signal 到 GUI
```

这样可以避免高频行情导致 GUI 线程事件队列堆积、模型更新和重绘抖动。

### 2.3 连接方式

- `QWebSocket::textMessageReceived` 只把原始 `QByteArray` 投递给 worker；
- worker 到 `HttpWsNodeClient` 使用 `Qt::QueuedConnection`；
- `HttpWsNodeClient` 再通过 `NodeClient` 的强类型信号发给 `MainWindow`；
- 所有模型/图表对象都只被 GUI 线程访问。

### 2.4 销毁顺序

`HttpWsNodeClient` 析构时：

1. `socket_.abort()`，停止新消息；
2. 用 `BlockingQueuedConnection` 调用 worker `stop()`；
3. `parserThread_.quit()` 并 `wait()`；
4. `QThread` 结束后 worker 通过 `deleteLater()` 回收。

## 3. 网络层

### 3.1 NodeClient

`ui/src/net/NodeClient.h` 定义：

- 连接状态：`Disconnected / Connecting / Connected / Ready / Reconnecting`；
- 订阅：symbols、klines、topics；
- 一次性请求：catalog、quote、option chain、kline history、account、orders、
  positions、trades；
- 交易：`placeOrder()`、`cancelOrder()`；
- 强类型信号：quote/kline/catalog/account/.../orderAck/error。

### 3.2 HttpWsNodeClient

`ui/src/net/HttpWsNodeClient.{h,cpp}` 负责：

- 拼接 REST URL；
- 管理 WebSocket 连接、重连和订阅；
- 将 REST body/WS text 的**原始字节**交给解析线程；
- 将解析结果转成 `NodeClient` 信号；
- 维护 `RawCallback`，不在网络回调中做 JSON 解析。

注意：`requestKlineHistory(..., viewWidth)` 目前 node REST 默认返回约 200
根，后续可在 node 路由支持 `view_width`。

### 3.3 Protocol

`ui/src/net/Protocol.h` 是 UI 与 node 的 wire model：

- `Quote`、`KlineBar`、`KlineFrame`、`Instrument`、`Account`、`Position`、
  `Order`、`Trade`、`OptionChain`、`MarketUpdate`；
- `parseXxx()` / `parseMarketData()`；
- `mergeQuote()`：只覆盖有值字段，保留完整快照中的昨结算、涨跌停、
  持仓、时间戳等；
- `makeSubscribe()` / `makeOrder()`。

`Quote` 的部分 diff 不允许直接覆盖完整快照，否则涨跌、增仓等计算字段
会因为缺字段而变成 `—`。

## 4. 数据模型

### 4.1 MarketStore

`ui/src/model/MarketStore.h`

- `quotes_`: symbol -> 合并后的 `Quote`；
- `klines_`: symbol -> durationNs -> 最近 K 线；
- `applyQuotes()` / `applyKline()`；
- 只保存 UI 展示需要的数据，不是 node DataManager 的复制品。

### 4.2 QuoteTableModel

自选列表模型：

- 行顺序由用户 watchlist 决定；
- 最新价、涨跌、涨幅、增仓等在 `data()` 中计算；
- 最新价/涨跌/涨幅使用红绿前景色；
- 缺失字段显示 `—`，但不会因为一次部分 diff 清空已有快照。

### 4.3 InstrumentTableModel

期货/期权合约列表模型：

- `setInstruments()` 建立 `symbol -> row` 索引；
- `applyQuotes()` 只更新命中的行；
- 每个变化行只发一个 `dataChanged`；
- 隐藏时由 `MainWindow` 跳过刷新，显示时用最新 quote 缓存补齐。

### 4.4 交易模型

`ui/src/model/TradingModels.{h,cpp}`：

- `PositionTableModel`：持仓；
- `OrderTableModel`：委托，按 order id 索引；
- `TradeTableModel`：成交；
- `TradeTapeModel`：分笔成交，最新在上、有界队列、时间/价格/手数/方向。

`TradeTapeModel` 当前仍由 quote volume 增量生成，后续可以替换为真实 tick 流。

## 5. 组件

### 5.1 MainWindow

`ui/src/app/MainWindow.{h,cpp}` 是工作区编排器：

- 组装 toolbar 和 QDockWidget；
- 连接 `NodeClient` 信号；
- 处理合约选择、下单、布局保存；
- 不直接解析 JSON。

主要 dock：

| objectName | 内容 | 默认区域 |
|---|---|---|
| `watchlistDock` | 自选列表 | 左 |
| `futuresDock` | 期货/合约列表 | 左侧 tab |
| `optionsDock` | 期权链 | 右 |
| `quoteDock` | 行情快照 | 右 |
| `tapeDock` | 分笔成交 | 右下 |
| `tradingDock` | 持仓/委托/成交/账户 | 下 |
| `orderDock` | 三键下单 | 下右 |

所有 dock 均允许关闭、拖动、浮动和重新显示。工具栏“组件”菜单和
菜单栏“视图 / 组件”可重新打开组件。

### 5.2 KLineChartWidget

`ui/src/chart/KLineChartWidget.{h,cpp}`：

- 自绘蜡烛、成交量、网格、十字光标；
- 头部显示最新价、涨跌、涨跌幅；
- 滚轮缩放；
- 左键拖动平移；
- 十字光标下方显示对应 bar 的起始时间；
- `visibleCount/visibleStart/followLatest` 可持久化；
- 只绘制可见区间。

### 5.3 QuoteSnapshotPanel / OrderTicketPanel / TradingTablesPanel

这三个面板从 MainWindow 拆出，分别负责：

- `QuoteSnapshotPanel`：行情字段展示和红绿格式化，不持有 node/model；
- `OrderTicketPanel`：三键下单输入、模糊补全入口和按钮信号，不直接下 order；
- `TradingTablesPanel`：持仓/委托/成交/账户页签、撤单入口和账户字段展示。

MainWindow 只做这些组件与模型/NodeClient 之间的连接。交易方向、OrderRequest
构造和是否允许平仓仍由 MainWindow 的交易协调逻辑决定，但具体控件不再写在
MainWindow 里。

### 5.4 TablePanel

`ui/src/widgets/TablePanel.{h,cpp}`：

- 统一表格选择和外观；
- 表头可拖动换序；
- 列宽可拖动；
- 右键显示/隐藏列；
- 列顺序、列宽、隐藏状态写入 QSettings。

## 6. 数据流

### 6.1 启动

```text
MainWindow 构造
  -> 读取 watchlist/chart/window QSettings
  -> buildToolbar
  -> buildWorkspace (docks + chart)
  -> buildBottomDock
  -> buildViewMenu
  -> restoreWorkspace (restoreState + restoreGeometry)
  -> connectNode
  -> selectSymbol(saved symbol)
```

### 6.2 WebSocket 行情

```text
node /stream text
  -> QWebSocket (GUI thread)
  -> JsonParserWorker::parseWsMessage
  -> parseMarketData on worker
  -> 33ms merge queue
  -> marketUpdateParsed (queued)
  -> MainWindow
       -> MarketStore
       -> QuoteTableModel
       -> visible InstrumentTableModel
       -> updateQuotePanel
       -> chart setBars (once per batch)
       -> TradeTapeModel
```

### 6.3 REST 历史/目录

```text
QNetworkReply::finished
  -> raw QByteArray (GUI thread, no parse)
  -> JsonParserWorker parse*
  -> queued typed signal
  -> MainWindow / model
```

K 线历史返回后：

```text
KlineFrame{reset=true, bars}
  -> MarketStore
  -> chart_->setBars(...)
```

### 6.4 交易

```text
三键下单
  -> makeOrder(QJsonObject)
  -> sendRawJson (GUI thread)
  -> reply body raw
  -> worker parseOrderAckPayload
  -> orderAckParsed (queued)
  -> status/order ack UI
```

持仓/委托/成交 REST 快照同样经过 worker 解析，再由模型合并。

## 7. 布局与状态持久化

QSettings key：

```text
ui/workspaceState       QMainWindow::saveState()
ui/windowGeometry       QMainWindow::saveGeometry()
ui/watchlist            自选合约列表
ui/chart/symbol         当前图表合约
ui/chart/interval       当前周期文字
ui/chart/durationNs     当前周期纳秒
ui/chart/visibleCount   K 线可见根数
ui/chart/visibleStart   左边界
ui/chart/followLatest   是否跟随最新
ui/columns/<key>/hidden/<logical>
ui/columns/<key>/width/<logical>
ui/columns/<key>/order
```

保存时机：

- dock resize / 移动 / 显示隐藏；
- 主窗口 resize / move；
- 图表缩放/拖动；
- 选中合约/切换周期；
- 表格列拖动/缩放/隐藏；
- 关闭窗口。

## 8. 测试

CMake targets：

```text
tqsdk-ui-tests           Protocol JSON / request serialization
tqsdk-ui-model-tests     models, quote merge, tape ordering
tqsdk-ui-parser-tests    worker parse, burst coalescing
tqsdk-ui-chart-tests     chart viewport, drag, wheel, paint smoke test
tqsdk-ui-table-tests     TablePanel column order/width/hidden persistence
tqsdk-ui-window-tests    MainWindow chart/selection integration + QSettings restore
tqsdk-ui-panel-tests     Quote/order/trading panels formatting and actions
```

运行：

```bash
cmake --build --preset ui-dev
ctest --test-dir build-ui --output-on-failure
```

GUI 组件测试使用 offscreen platform；MainWindow 集成测试使用 FakeNodeClient，
验证恢复图表合约/周期/视口和关闭时写回 QSettings：

```text
QT_QPA_PLATFORM=offscreen
```

## 9. 已知边界与后续扩展

- 分笔当前是 quote volume 增量，不是交易所逐笔；
- 图表指标插件尚未实现；设计见 `docs/INDICATOR_PLUGINS.md`；
- 超大 catalog/option chain 的模型 reset 仍会占用一次 GUI 时间，后续可做
  增量/分页模型；
- 若 node 增加 view_width 参数，K 线历史请求可以一次拿更深历史；
- `NodeClient` 的传输层可替换，不影响 Qt widget 和模型。

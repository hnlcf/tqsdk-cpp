# 模块实现

本文是模块级索引：每个模块的职责、关键文件、对外接口和禁止的依赖方向。
总体依赖见 [ARCHITECTURE.md](ARCHITECTURE.md)。

## 1. 领域模型 `domain/`

```text
include/tqsdk/domain/
  quote.h       Quote + QuoteField bitset
  order.h       Order + OrderField
  trade.h       Trade + TradeField
  position.h    Position + PositionField
  account.h     Account + AccountField
  kline.h       KlineBar / KlineStream
  enum_util.h   枚举 <-> magic_enum
  sentinel.h    kUnknownDouble / kUnknownInt
```

约定：

- `changed_fields` 是 `uint64_t` 位图；
- 非有限浮点写 JSON 时转 `null`；
- 领域对象不持有网络/线程资源。

## 2. Channel `channel/`

`Channel<T>` 是 Asio 协程队列：

- `async_send`：有界时阻塞，关闭后抛 `operation_aborted`；
- `async_receive`：返回 `optional<T>`，关闭且空时 `nullopt`；
- `try_send/post_send/post_close` 供跨线程生产者使用；
- 等待基于 `steady_timer`，不额外开线程。

有界 Channel 是背压边界；WS 客户端还另有发送队列上限。

## 3. 数据层 `data/`

### DataManager

`include/tqsdk/data/data_manager.h`

```text
EntityStore<Quote>    quotes
EntityStore<Order>    orders
EntityStore<Trade>    trades
EntityStore<Position> positions
Account                account
KlineRegistry          klines
```

`clear_trading()` 只清交易实体，用于交易快照重建；行情/K 线保留。
`clear_all_changes()` 在 `wait_update` 下一轮清 dirty 位。

### DiffMergeEngine

```text
src/data/diff_merge_engine.cpp     顶层路由
src/data/diff_merge_quotes.cpp
src/data/diff_merge_trade.cpp
src/data/diff_merge_positions.cpp
src/data/diff_merge_klines.cpp
```

职责：把 JSON diff 合并进强类型实体并设置字段 dirty 位。
订单合并会用合并前/后的实际值比较，CTP 全量重复查询不会产生假 dirty。

### KlineStore

`include/tqsdk/data/kline_store.h`、`src/data/kline_store.cpp`

- SQLite 持久化；
- 独立异步 writer；
- 每序列最多 `max_bars`；
- node 启动时 hydrate 到本地 KlineStream；
- 不是 core strand 上的阻塞调用。

## 4. 网关 `gateway/`

### 接口

```text
include/tqsdk/gateway/market_data_gateway.h
include/tqsdk/gateway/trading_gateway.h
```

`TradingGateway` 关键方法：

```text
start/stop
insert_order/cancel_order
snapshot
event_output
snapshot_ready
requires_initial_trade_snapshot
needs_shfe_price_normalization
session_phase
```

### Tq MD

`gateway/tq/tq_md_gateway.*`、`tq_md_protocol.*`

- WebSocket 行情；
- 处理订阅引用、diff/Kline；
- 断线重连。

### CTP

```text
gateway/ctp/
  ctp_config.h          连接参数
  ctp_task_queue.*      CTP 回调线程 -> Asio
  ctp_spi_impl.*        Trader/Md SPI
  ctp_md_gateway.*
  ctp_td_gateway.*      生命周期/回调桥接
  ctp_td_orders.cpp     订单映射/持久化/报撤
  ctp_td_queries.cpp    查询结果合并/对账/快照
  ctp_query_pipeline.*  查询队列/generation/watchdog/retry/restart
  ctp_order_store.*     有界持久化订单
  ctp_convert.h         CTP 字段 -> JSON/领域转换
```

CTP 回调线程不执行业务；查询有 watchdog/generation，迟到响应会被丢弃。

### Sim

`gateway/sim/sim_td_gateway.*`：本地撮合，支持部分成交/账户/持仓，
用于回测和 e2e。

### Mock

`gateway/mock/*`：测试用行情/网关注入。

## 5. IPC `ipc/`

```text
frame.h                帧头/长度/编码
protocol.h             各 FrameType 的 body 类型
codec.h                JSON / MessagePack 编码抽象
ipc_session.*          握手/心跳/seq/路由
ipc_client.*           node -> gw 客户端
gateway_server.*       生命周期
gateway_server_pumps.* md/td 上行泵
gateway_server_control.* 订阅/命令/Resume
remote_md_gateway.*    MarketDataGateway 远程代理
remote_td_gateway.*    TradingGateway 远程代理
gateway_subscription_book.h 连接级订阅引用账本
command_ack_cache.h     Command 幂等缓存
event_bus.h            多 node 扇出
stream_seq.h            seq/replay
```

`GatewaySubscriptionBook` 让 quote/K 线共享 symbol 引用计数，且显式 subscribe
幂等；`CommandAckCache` 防止 ACK 超时重发导致重复报单。

## 6. TqApi `src/api/`

```text
tq_api.cpp         生命周期、md_loop/td_loop、wait_update、订阅、K线
tq_api_order.cpp   本地占位订单、价格归一化、insert/cancel ack 处理
include/tqsdk/tqapi.h
```

TqApi 是唯一门面：

- `start/close`
- `wait_update`
- `get_quote/get_order/get_account`
- `get_kline_serial`
- `subscribe_quotes/subscribe_kline/release`
- `insert_order/cancel_order`
- `readiness_json/metrics_json`

## 7. Serve `serve/`

```text
router.h               类型化路径/Query/Body 路由
http_server.*          accept/session/dispatch
http_routes.cpp        REST 路由表
ws_protocol.h          WS 强类型消息
ws_stream.*            WebSocketHub：连接/订阅/push_loop
ws_entity_filter.*     客户端快照裁剪/topic 过滤/实体去重
snapshot_service.*     交易快照投影（HTTP/WS 共用）
json_convert.*         领域 -> JSON
```

数据访问在 core strand；socket I/O 在连接 strand。WS 默认只推行情，
交易 topics 必须显式订阅；未变化实体不会重复广播。

## 8. 交易日/静态数据

```text
instrument/instrument_catalog.*   symbol.json/latest.json 加载与刷新
trading/trading_calendar.h        phase/night/holiday 规则
trading/holiday_calendar.*        节假日 URL、缓存、后台刷新
trading/order_normalizer.h        SHFE 市价/对价 -> 限价
util/http_download.*              低频同步 HTTP(S) GET
```

## 9. 配置 `config/`

`src/config/tqapi_factory.cpp`：

- TOML -> `TqApiConfig` / `GwConfig` / `NodeConfig`；
- 选择网关 kind；
- 构造本地或远程 TqApi；
- CTP 参数、serve、instruments、kline cache、ws。

配置文件入口：

```text
config/gw.toml
config/node.toml
config/live.local.env.example
```

## 10. 应用入口

```text
apps/gw_main.cpp       启动真实网关 + IPC server
apps/node_main.cpp     Remote 网关 + TqApi + serve + catalog/cache
```

## 11. Web 前端 `web/`

```text
web/src/api/client.ts           REST/WS、重连、订阅
web/src/chart/                  K线、分时、指标
web/src/components/             自选/下单/盘口/分笔/交易表/顶栏
web/src/pages/                  期货列表、期权链
web/src/indicators/             MA/EMA/BOLL/MACD/RSI
web/src/lib/                    localStorage、格式化
```

详见 [WEB_FRONTEND.md](WEB_FRONTEND.md)。Qt UI 代码已删除。

## 12. 依赖规则速查

| 模块 | 可以依赖 | 禁止依赖 |
|---|---|---|
| domain | pch/types | gateway/serve/ipc |
| channel | Asio | 业务模块 |
| gateway | domain/channel | serve/UI |
| data | domain/channel | HTTP/UI |
| ipc | gateway/protocol | UI |
| tqapi | gateway/data/ipc | serve |
| serve | tqapi/data | UI |
| web | 仅 REST/WS | tqsdk_core |

## 13. 测试对应

```text
tests/channel/       Channel 语义
tests/data/          DataManager/DiffMerge/KlineStore
tests/ipc/           帧/会话/重连/replay/幂等/订阅账本
tests/gateway/       TQ 协议、CTP 映射、故障注入
tests/serve/         router/REST/WS/filter/snapshot/readiness
tests/e2e/           Sim/Mock 全链路
web/src/**/*.test.ts  指标/session 单测
```

# 协议与接口

本文描述 wire-level 契约。业务流见 [FLOWS.md](FLOWS.md)。

## 1. IPC

### 帧格式

```text
+--------+--------+----------+-------------------+
| length | type   | encoding | payload            |
| u32 BE | u8     | u8       | length-2 bytes     |
+--------+--------+----------+-------------------+
```

- `length` 包含 type + encoding + payload；
- 默认 `encoding=JSON`；
- payload 是强类型 body 的编码。

### FrameType

| 编号 | 类型 | 方向/用途 |
|---|---|---|
| `0x01` | Hello | client -> core，握手 |
| `0x02` | HelloAck | 握手确认 |
| `0x03` | Subscribe | node -> gw，行情/K线订阅 |
| `0x04` | SubscribeAck | 订阅结果 |
| `0x05` | Snapshot | 首帧/恢复完整快照 |
| `0x06` | Diff | 增量 |
| `0x07` | Command | 报单/撤单 |
| `0x08` | CommandAck | 命令回执 |
| `0x09` | GatewayEvent | 连接/错误状态 |
| `0x0A` | MarketDataPush | gw -> node 行情 |
| `0x0B` | TradePush | gw -> node 交易 |
| `0x0C` | QueryOrders | 订单查询兜底 |
| `0x0D` | QueryOrdersAck | 查询结果 |
| `0x0E` | Heartbeat | 应用层心跳 |
| `0x0F` | HeartbeatAck | 心跳回应 |
| `0x10` | Goodbye | 优雅关闭 |
| `0x11` | Resume | 重连恢复 |
| `0x12` | ResumeAck | 恢复策略 |
| `0x13` | TradeSnapshot | 交易对账快照 |

### Seq / Epoch / Resume

- 上行 stream 带 `seq`，每连接 `epoch`；
- `Resume{epoch, md_next_seq, td_next_seq}`；
- epoch 相同且 seq 在 replay window 内 -> replay；
- epoch 不同/缺口不可恢复 -> 完整 resync；
- gateway 对每个连接维护独立 outbound stream 和 replay buffer。

### 命令幂等

`Command` 可能因 ACK 超时被 `IpcClient` 用同一 rid 重发。要求：

```text
key = "<op>:<rid>"
第一次 -> 执行 -> 缓存 CommandAck
重复   -> 直接返回缓存 ack，不再次报单
```

`GatewayIpcServer` 使用有界 `CommandAckCache` 实现。

## 2. REST

Base URL 默认 `http://127.0.0.1:18081`。

### 只读

| Method | Path | Body |
|---|---|---|
| GET | `/healthz` | `{"ok":true}` |
| GET | `/readyz` | readiness + md/td + `phase` + `expected_disconnect` |
| GET | `/metrics` | readiness + gateway metrics + kline cache |
| GET | `/holiday/status` | holiday URLs/cache/date count/version |
| GET | `/catalog/status` | catalog loaded/source/count |
| GET | `/catalog/instruments` | `{instruments,count,catalog}` |
| GET | `/catalog/instruments/{id}` | instrument |
| GET | `/catalog/options/products` | option product list |
| GET | `/catalog/option-chain/{underlying}` | calls/puts/underlying quote |
| GET | `/quote/{symbol}` | quote snapshot |
| GET | `/quotes?symbols=A,B` | batched quotes |
| GET | `/kline/{symbol}/{duration_ns}` | `{symbol,duration_ns,bars}` |
| GET | `/kline-cache/status` | KlineStore status |
| GET | `/account?account=default` | account |
| GET | `/orders` | `{"orders":{...}}` |
| GET | `/trades` | `{"trades":{...}}` |
| GET | `/positions` | `{"positions":{...}}`；REST 隐藏零仓位 |
| GET | `/trading-snapshot` | account/orders/trades/positions 合并 |
| GET | `/ticks/{symbol}` | quote 固化 tick 快照 |
| GET | `/trading-status?account=default` | TradingStatus 快照 |
| GET | `/accounts` | 多账户列表与账户快照 |
| GET | `/option/analytics/{underlying}` | ATM + Black-Scholes Greeks |

### 写

| Method | Path | Body |
|---|---|---|
| POST | `/order?account=default` | `InsertOrderReq` JSON |
| DELETE | `/order/{id}` | 撤单 |
| POST | `/task/target` | TargetPosTask：`symbol,target_volume,mode,max_volume` |
| DELETE | `/task/target/{id}` | 取消并删除 target task |
| POST | `/task/order` | InsertOrderTask：`{order: InsertOrderReq}` |
| DELETE | `/task/order/{id}` | 取消并删除 order task |
| GET | `/tasks` | 任务状态列表 |
| POST | `/risk/rules` | 新增风控规则 |
| DELETE | `/risk/rules` | 清空风控规则 |
| GET | `/risk/status` | 当前规则数量 |

`InsertOrderReq` 关键字段：

```json
{
  "instrument_id": "SHFE.ao2701",
  "direction": "BUY",
  "offset": "OPEN",
  "price_type": "LIMIT",
  "limit_price": 2868.0,
  "time_cond": "IOC",
  "volume_cond": "ANY",
  "advanced": "FAK",
  "offset_leg2": "",
  "volume": 1
}
```

枚举按 magic_enum 名称序列化，例如 `BUY/SELL/OPEN/CLOSE/CLOSETODAY/ANY/BEST/FIVELEVEL/LIMIT/IOC/GFD/FAK/FOK`。
`advanced=FAK/FOK` 会由 `apply_advanced_order()` 映射为 `IOC + ANY/ALL`；
非 LIMIT 价格类型默认 IOC。

## 3. WebSocket `/stream`

文本 UTF-8 JSON。

### Welcome

```json
{"type":"welcome","protocol":1,"heartbeat_interval_ms":30000}
```

### Subscribe

```json
{
  "op":"subscribe",
  "symbols":["SHFE.rb2701"],
  "klines":[{"symbol":"SHFE.rb2701","duration_ns":60000000000,"view_width":200}],
  "topics":["orders"]
}
```

语义：

- symbols/klines 为增量订阅；
- **topics 缺省或空数组 -> 只订阅行情，不推送交易实体**；
- 需要交易数据必须显式列出 `account/orders/trades/positions`；
- 显式 topics 整体替换当前交易 topic 集合；
- 服务端回 `subscribed` + 首帧 `snapshot`。

### Snapshot / Diff

```json
{"type":"snapshot","data":{"quotes":{...},"klines":{...}}}
{"type":"diff","seq":1,"timestamp_ns":...,"data":{...}}
```

交易 diff 只包含该客户端显式订阅的 topic，且相同订单/成交/持仓/账户 JSON
不会重复发送。慢消费者队列达到 `max_queue` 时丢最旧消息。

### Ping/Error

```json
{"op":"ping"}
{"type":"pong"}
{"type":"error","error":"..."}
```

## 4. 序列化约定

- 非有限 double（NaN/Inf）-> `null`；
- `null` -> `kUnknownDouble`；
- 时间戳：纳秒整数，交易/行情跨层使用 `TimeStamp=uint64_t`；
- 枚举字符串由 magic_enum 生成；
- 账户/订单/成交/持仓 key 使用字符串 id；
- K 线 duration 用纳秒整数。

## 5. SHFE 价格归一化

CTP SHFE 不接受 AnyPrice/BestPrice。node 侧统一转换为可报限价：

| 用户意图 | CTP 字段 |
|---|---|
| BUY ANY | `LIMIT`, `upper_limit`, `IOC` |
| SELL ANY | `LIMIT`, `lower_limit`, `IOC` |
| BUY BEST | `LIMIT`, `ask_price1`, `IOC` |
| SELL BEST | `LIMIT`, `bid_price1`, `IOC` |

缺少有效 quote 时本地直接拒绝，不发送非法 CTP 报单。详见 [CTP.md](CTP.md)。

## 6. 错误与状态码约定

- REST 参数错误：400；
- 资源不存在：404；
- catalog/未 ready：503；
- 命令层拒绝：HTTP 200 + `{"success":false,"error":"..."}`；
- 业务/交易错误以 `Order.last_msg`/`CommandAck.error_msg` 表达；
- `/readyz` 503 但 body 中仍有 md/td 细状态，调用方应读取 body 而不是只看状态码。

## 7. UI transport contract

Web 前端只通过 node REST/WS 访问数据和交易，不链接 `tqsdk_core`：

- 初始快照用 REST，增量用 `/stream` snapshot/diff；
- WS 断线指数退避重连并重新发送显式 topics 订阅；
- K 线历史 REST + WS 增量合并，图表命令式更新；
- 交易 topics 必须显式包含 `account/orders/trades/positions`；
- `/readyz` 或 WS 未就绪时禁止报单。

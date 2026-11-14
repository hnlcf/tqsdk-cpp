# TQ 协议与 TqTdGateway

本文记录天勤 OTG 交易协议、C++ 消息结构和本项目的接入方式。协议参考官方
[tqsdk-python](https://github.com/shinnytech/tqsdk-python)：

- `tqsdk/account.py`：旧版 TqAccount/TqKq 数据流；
- `tqsdk/tradeable/otg/base_otg.py`：OTG 通道路由与账户映射；
- `tqsdk/tradeable/otg/tqaccount.py`：登录报文；
- `tqsdk/connect.py`：WebSocket、peek、重连；
- `tqsdk/api.py`：订单、风控、查询报文。

## 1. 账户模型

官方有两套实现：

```text
tqsdk/account.py
  TqAccount / TqKq / TqKqStock
  较旧，直接处理 req_login、confirm_settlement、
  insert_order/cancel_order/set_risk_management_rule。

tqsdk/tradeable/otg/tqaccount.py
  BaseOtg + TqAccount
  新实现，额外支持：
    - rtn_brokers / OTG capabilities
    - pre_insert_order
    - TdReconnectHandler 的全截面恢复
    - 多账户 account_key 路由
```

我们的 C++ 实现采用新模型，但每个 `TqApi` 只承载一个交易账户；多账户由
`MultiAccountManager` 为每个账户创建独立 gateway。

## 2. 消息目录

C++ 定义在：

```text
include/tqsdk/gateway/tq/tq_protocol.h
```

### 客户端 -> 服务端

| aid | C++ struct | 用途 |
|---|---|---|
| `req_login` | `LoginRequest` | 期货账户登录 |
| `confirm_settlement` | `ConfirmSettlement` | 自动确认结算单 |
| `subscribe_quote` | `SubscribeQuote` | 订阅行情 |
| `ins_query` | `InsQuery` | GraphQL 合约查询 |
| `set_chart` | `SetChart` | K 线/tick 序列 |
| `set_chart_data` | `SetChartData` | 图表/指标数据 |
| `peek_message` | `PeekMessage` | 拉取一帧数据 |
| `insert_order` | `InsertOrder` | 报单 |
| `cancel_order` | `CancelOrder` | 撤单 |
| `pre_insert_order` | `PreInsertOrder` | 预计算保证金 |
| `set_risk_management_rule` | `SetRiskManagementRule` | 交易所风控规则 |
| `subscribe_trading_status` | `SubscribeTradingStatus` | 交易状态 |
| `set_report_data` | `SetReportData` | 回测报告数据 |
| `ratio` | `Ratio` | 回放倍速 |
| `heartbeat` | `Heartbeat` | 回放心跳 |
| `terminate` | `Terminate` | 回放/会话终止 |

### 服务端 -> 客户端

| aid | C++ struct | 用途 |
|---|---|---|
| `rtn_data` | `RtnData` | 合并 diff / 交易截面 |
| `rtn_brokers` | `RtnBrokers` | OTG 能力 |
| `rsp_login` | `RspLogin` | 登录响应 |
| `rsp_quotes` | `RspQuotes` | 行情请求响应 |

`tq::parse()` 返回 `std::expected<Message, ParseError>`；未知 aid 不会被静默
吞掉，而是返回错误，避免协议升级后“看起来正常但没有数据”。

## 3. TQ TD 数据流

```text
TqAuth::get_td_url()
        │
        ▼
TqTdGateway (WebSocket wss)
  req_login
  confirm_settlement
  peek_message
        │
        ▼
rtn_data
  ├─ trade[user_id] -> trade[account_key]
  ├─ trade_more_data=false -> TradeEvent::Snapshot
  └─ 后续 rtn_data -> TradeEvent::Diff
        │
        ▼
TqApi::td_loop -> DiffMergeEngine
```

订单：

```text
TqApi::insert_order
  -> TqTdGateway::insert_order
  -> tq::InsertOrder
  -> WebSocket insert_order
  -> 后续 rtn_data 推进 Order 状态
```

`CommandAck` 只表示命令已写入 WebSocket；交易所/柜台是否接受，以后续 order
diff 的 `is_error` / `status` 为准。

## 4. 重连

断线后：

1. 发出 `TradeEvent::Disconnected`；
2. 重建 wss；
3. 重发 `req_login`；
4. 重发 `confirm_settlement`；
5. 重新 `peek_message`；
6. 等 `trade_more_data=false`；
7. 发出完整 `TradeEvent::Snapshot`，由 `TqApi` 先清理旧交易实体再合并。

这样不会把断线前的旧挂单、旧持仓残留到新一轮连接。

## 5. 配置

TQ TD 只在 `tqsdk-gw` 进程中使用：

```toml
[td]
kind = "tq"
auto_url = true

[td.tq]
broker_id = "simnow"      # TqAccount 的 broker_id
account_id = "..."         # 期货资金账号
password = "..."
# 可选
front_broker = ""
front_url = ""
td_url = ""                # 显式覆盖动态解析结果
account_key = ""           # 默认等于 account_id
```

`client_app_id` 固定为官方值 `SHINNY_TQ_1.0`，`client_mac_address` 由
`util::machine_mac_address()` 从本机非 loopback 网卡读取；两者都不作为用户配置项。

`auto_url=true` 时通过 `[tq_auth]` 登录快期账号并请求动态 TQ TD URL。
`[tq_auth]` 的快期账号和 `[td.tq]` 的期货账户是两套凭据，不能混用。

## 6. 当前实现状态

- `TqTdGateway` 已实现 wss 连接、`req_login`、`confirm_settlement`、`peek_message`、
  `rtn_data`/`rtn_brokers` 处理、断线重连、Snapshot 恢复、1 手报单/撤单报文；
- TQ OTG 端点要求客户端声明 `permessage-deflate`；网关握手显式写入该扩展并启用
  Beast 压缩，否则服务端会以 HTTP 400 拒绝；
- `TqMdGateway` 已改用 `tq::Message` 生成发送报文并用 `tq::parse` 解析服务端帧；
- `TqProtocol` 单测覆盖全部 aid 和 `TqTdNormalization` 数据映射。

## 6.1 当前限制

- 只实现 `wss://`；`sm://` 国密和 `zqotg://` 本地适配器暂不支持。
- 尚未接入 `pre_insert_order` 的保证金预计算 API；消息结构已就绪。
- `set_risk_management_rule` 仅 SSE/SZSE 股票风控场景；当前期货 gateway 不发送。
- 真实 TQ 柜台仍需 SimNow/TQ 账户验收；消息层已有确定性单测。
- `TqTdGateway` 当前由 gw 进程构造；node 仍通过 remote IPC 访问；
- 2026-11-14 对 `wss://otg-simnow.shinnytech.com/trade` 做过只读握手冒烟：
  握手返回 101，收到 `rtn_brokers`；当时 SimNow 交易时段已关闭，未继续完成
  真实账户登录/报单验收。

## 7. 测试

```bash
ctest --test-dir build-ui -R TqProtocol --output-on-failure
ctest --test-dir build-ui -R TqApiConfigTest --output-on-failure
```

`TqProtocol` 覆盖：

- 全部 aid 名称与解析；
- 登录、查询、图表、订单、撤单、预插入、风控、控制消息；
- `rtn_data` / `rtn_brokers` / `rsp_login` / `rsp_quotes`；
- 未知 aid 返回 `ParseError`；
- `TqTdNormalization` 覆盖 OTG trade 节点到 `trade[account_key].user` 的转换；
- `MachineId` 覆盖本机 MAC 格式。

```text
本地最终结果：CTest 379/379（2 项网络测试按环境跳过），Python 21/21。
```

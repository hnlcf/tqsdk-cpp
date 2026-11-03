# CTP 网关

本文说明 tqsdk-gw 的 CTP 行情/交易接入：构建方式、配置、线程桥接、订单/成交映射、
断线重连与查询对账，以及当前验证边界。

## 1. 构建

CTP SDK 放在 `third_party/ctp/`，支持两种布局：

```text
third_party/ctp/
  ThostFtdcTraderApi.h
  ThostFtdcMdApi.h
  ...
  thosttraderapi_se.so
  thostmduserapi_se.so
```

或：

```text
third_party/ctp/
  include/...
  lib/libthosttraderapi_se.so
  lib/libthostmduserapi_se.so
```

构建：

```bash
cmake -S . -B build-ctp -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_INSTALLED_DIR="$PWD/build/vcpkg_installed" \
  -DVCPKG_MANIFEST_INSTALL=OFF \
  -DTQSDK_ENABLE_CTP=ON \
  -DTQSDK_BUILD_TESTS=ON
cmake --build build-ctp --target tqsdk-gw tqsdk_tests -j
```

未开启 `TQSDK_ENABLE_CTP` 时，`md.kind=ctp` / `td.kind=ctp` 会记录错误并退化为
`nullptr`，不会把未实现的网关静默当成可用。

## 2. 配置

```toml
[md]
kind = "ctp"

[td]
kind = "ctp"

[ctp]
broker_id = "9999"
user_id = "your_user"
password = "your_password"
auth_code = "0000000000000000" # 部分经纪商需要
app_id = "your_app_id"         # 与 auth_code 配套
td_front = "tcp://180.168.146.187:10130"
md_front = "tcp://180.168.146.187:10131"
flow_path = "/tmp/tqsdk_ctp"
```

`flow_path` 必须是可写目录；CTP 会写流文件/私有流恢复数据。不同 broker 的前置地址
不同，以经纪商提供的为准。

## 3. 线程桥接

CTP SDK 的 SPI 回调在 SDK 自管线程触发，不能直接改 Asio 状态。实现：

```text
CTP SPI 线程
  OnRtnOrder / OnRtnTrade / Query ...
        |
        | 拷贝 CTP struct + 回调
        v
CtpTaskQueue (mutex + deque, post 到 core executor)
        |
        v
tqsdk-gw core strand
        |
        v
CtpTdGateway / CtpMdGateway 状态、Channel、IPC push
```

`CtpTaskQueue` 用 `shared_from_this()` 持有生命周期；`stop()` 丢弃未执行任务，
已 post 的 drain 会检查 `stopped_` 后退出。SPI 端的 `MdState/TdState` 用
`alive` 标记和 `callbacks` 共享指针，停止后 queued 回调不会访问已析构网关。

## 4. 行情网关（CtpMdGateway）

- `start()`：创建 `CThostFtdcMdApi`，`RegisterSpi`、`RegisterFront`、`Init`。
- `OnFrontConnected` -> `ReqUserLogin`。
- 登录成功后重放 `desired_` 中所有订阅；CTP 断线自动重连后会再次执行该流程。
- `OnRtnDepthMarketData` -> `depth_json()` -> `{"quotes":{sym:{...}}}` diff。
- `subscribe/release` 维护引用计数；引用归零才调用 `UnSubscribeMarketData`。
- CTP 不提供 K 线序列，`subscribe_kline` 为 no-op；K 线仍由 `TqMdGateway` 提供。
- 无效价格（CTP 的 `DBL_MAX`）转成 `kUnknownDouble`，最终 JSON 写 `null`。

## 5. 交易网关（CtpTdGateway）

### 5.1 登录与查询

1. `OnFrontConnected`：
   - 配置了 `app_id/auth_code` 时先 `ReqAuthenticate`，否则直接登录；
2. `OnRspUserLogin` 缓存 `FrontID/SessionID/TradingDay`；
3. `ReqSettlementInfoConfirm`；
4. 结算确认后并发查询：
   - `ReqQryInstrument`（仅首次/重连）
   - `ReqQryTradingAccount`
   - `ReqQryInvestorPosition`
   - `ReqQryOrder`
   - `ReqQryTrade`
5. 所有查询 `bIsLast` 后发出 `TradeEvent::Kind::Snapshot`（完整 orders/trades/
   positions/accounts）；重连后 node 侧会先 `clear_trading()` 再应用快照。

查询缓存不会在查询开始前清空：CTP 重连/查询窗口内 `snapshot()` 仍返回上一份可用
截面，避免 node 被空快照误清空。查询成功完成后按新结果重建。

### 5.2 报单/撤单

- `insert_order`：
  - 本地 `rid` 映射到 CTP `OrderRef`（≤12 字符直接用，超出用 `r<seq>`）；
  - 填充 `CThostFtdcInputOrderField`：方向、开平、价类型、有效期、数量条件、
    投机套保、立即触发等；
  - `ReqOrderInsert` 返回 0 即 CommandAck success；真实状态由 `OnRtnOrder` /
    `OnRtnTrade` 推 diff。
- `cancel_order`：
  - 优先使用缓存的 `FrontID/SessionID/ExchangeID/OrderSysID`；
  - 没有缓存时用 `rid`/`exchange_order_id` 填 `OrderRef`/`OrderSysID`；
  - `ActionFlag=Delete`，`ReqOrderAction` 返回 0 即 ack success。
- `OnRspOrderInsert` / `OnErrRtnOrderInsert` 错误会生成
  `{"trade":{"":{"user":{"orders":{rid:{status:FINISHED,is_error:true,is_dead:true}}}}}}`
  的 reject diff，保证上层能看到失败终态。
- `OnRtnOrder` / `OnRtnTrade` 更新缓存并 200ms debounce 查询账户/持仓/订单/成交，
  推完整实体 diff。这样避免高频成交时每个事件都做一次全量查询。

命令幂等与重试：

- `IpcClient::send_command_and_wait` 在 ack 超时后可用**同一 rid** 重发
  （`RemoteTdGateway` 最多 2 次）；会话关闭不重发；
- `GatewayIpcServer` 按 `<op>:<rid>` 缓存首次 `CommandAck`；重复命令直接回放
  缓存 ack，**不会再次调用网关**，因此 CTP 报单不会被重复提交；
- 缓存每连接最多 4096 条；连接重建后缓存丢失，跨连接的命令重放仍需依赖
  订单查询/人工对账，不能把 `IpcClient` 超时重试等同于跨会话幂等；
- 订单最终状态仍以 `OnRtnOrder` / 查询结果为准，ack 只表示网关是否接受命令。

### 5.3 断线/重连

CTP API 自带前置重连。网关在 `OnFrontDisconnected`：

- 关闭 logged_in 状态；
- 清空 in-flight 查询计数（旧响应不会再到达）；
- 发 `TradeEvent::Kind::Disconnected`；
- `OnFrontConnected` 再次触发登录/结算/查询，查询完成后发 Snapshot 对账。

## 6. 字段映射

`ctp_convert.h` 负责 CTP 结构 -> 统一领域 JSON：

| 领域 | 关键映射 |
|---|---|
| Order | `OrderRef -> order_id`, `OrderSysID -> exchange_order_id`, `VolumeTotalOriginal/VolumeTotal -> volume_orign/volume_left`, `OrderStatus -> status/is_dead`, `StatusMsg -> last_msg` |
| Trade | `TradeID -> trade_id/exchange_trade_id`, `OrderRef -> order_id`, `OffsetFlag -> offset`, `Price/Volume` |
| Position | 多空分别聚合 `Position/TodayPosition/OpenCost/UseMargin/PositionProfit`；`OpenCost/(volume*VolumeMultiple)` 还原开仓均价 |
| Account | `Balance/Available/CurrMargin/FrozenMargin/Commission/CloseProfit/PreBalance/Deposit/Withdraw` |
| Depth | 最新价/五档/成交量/持仓量/涨跌停/结算价，无效价转 NaN -> JSON null |

## 7. 测试与验证边界

已覆盖：

- `tests/gateway/ctp_mapping_test.cpp`
  - Order/Trade/Account/Position/Depth JSON 映射；
  - `CtpTaskQueue` 从其他线程投递后在目标 executor 执行，stop 后丢弃任务；
- `tests/ipc/gateway_server_test.cpp::DuplicateCommandReusesCachedAck`
  - 同一 `<op>:<rid>` 发送两次，网关只调用一次交易网关，两次都回同一 ack；
- `-DTQSDK_ENABLE_CTP=ON` 全量构建：`tqsdk_ctp_gateway` + `tqsdk-gw`；
- CTP 网关进程启动/停止 smoke：指向不可达前置，确认 `Init/Release`、
  event loop 退出正常；
- 全量测试：CTP 构建下 ctest 通过（联网行情测试仍跳过）。

未覆盖（受外部条件限制）：

- 真实 CTP 经纪商账号/前置的登录、认证、报单、成交、查询回调时序；
- 不同经纪商对 `auth_code/app_id`、组合开平、套保标志、持仓日期的差异；
- CTP SDK 线程在极端断线/高频回报下的长时间 soak；
- 真实柜台 `ReqQryInstrument` 返回大量合约时的内存/时延。

这些项需要真实柜台环境，建议上线前用仿真/7x24 柜台做专项回归。

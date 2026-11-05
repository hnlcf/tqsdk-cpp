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
cmake --build build-ctp -j
```

`tqsdk_core` 现在是**唯一的动态库**：CTP 源文件直接编入 `libtqsdk_core.so`，
不再生成 `tqsdk_ctp_gateway` 第二个库。CTP 的 `thost*.so` 作为私有运行时依赖
链接进核心库。

打包：

```bash
cmake --build build-ctp --target package
# build-ctp/tqsdk_cpp-<version>-Linux-<arch>.tar.gz
```

包内布局：

```text
bin/tqsdk-gw
bin/tqsdk-node
lib/libtqsdk_core.so
lib/libthosttraderapi_se.so
lib/libthostmduserapi_se.so
include/tqsdk/...
include/ThostFtdc*.h
share/tqsdk/config/{gw,node}.toml
share/doc/tqsdk/...
```

- `libtqsdk_core.so` 的 `DT_NEEDED` 只记录 `libthost*.so` 文件名，不写构建机绝对路径；
- 安装 RPATH：核心库 `$ORIGIN`，可执行文件 `$ORIGIN/../lib`，解压即可运行；
- CTP SDK 头文件一并安装到 `include/`，下游可继续使用 CTP 网关头文件。

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
order_interval_ms = 100
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
4. 结算确认后串行查询：
   - `ReqQryTradingAccount`
   - `ReqQryInvestorPosition`
   - `ReqQryOrder`
   - `ReqQryTrade`
   - 仅当有持仓时，按持仓合约逐个 `ReqQryInstrument` 查合约乘数/保证金率
5. 所有查询 `bIsLast` 后发出 `TradeEvent::Kind::Snapshot`（完整 orders/trades/
   positions/accounts）；重连后 node 侧会先 `clear_trading()` 再应用快照。

查询串行化 + 流控：CTP 对查询接口有每秒请求数/未处理请求数限制，连续查询
会返回 `-2/-3`。`CtpTdGateway` 因此：

- 一次只发一条查询，收到 `bIsLast` 后间隔 300ms 再发下一条；
- 遇到 `-2/-3` 时同一查询退避 1s 重试，最多 8 次；
- 每条查询记录 `request_id + QueryKind + generation`，非当前查询的**迟到回调
  直接丢弃**，不会错误完成下一轮查询；
- 单查询 10s watchdog 超时后跳过并继续后续查询，避免某条 CTP 回调丢失导致
  整个快照永久卡住；
- 查询失败时继续后续查询，最终仍发一次可用截面（不会卡死）；
- CTP 回调队列区分优先级：报单/成交/错误回报优先于查询回调；单次 drain
  最多 256 条后让出 core strand，避免全量合约查询淹没交易回报。

查询缓存不会在查询开始前清空：CTP 重连/查询窗口内 `snapshot()` 仍返回上一份可用
截面，避免 node 被空快照误清空。查询成功完成后按新结果重建。

### 5.2 报单/撤单

- `insert_order`：
  - 报单/撤单共用一个 100ms 令牌节流器，串行化并避免触发交易所/柜台频率限制；
  - 本地 `rid` 映射到 CTP `OrderRef`：**只使用十进制数字且保持递增**；
    非数字 rid 会生成纯数字序号，并检查历史 OrderRef 冲突；
    node 的 rid 计数器用启动时间（秒）播种，格式 `r<decimal>`，网关映射时去掉
    前缀，避免进程重启后与柜台历史订单重号；
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
- 对 SHFE/INE/GFEX 的通用 `CLOSE`，若缓存显示有今仓，自动改用 `CLOSETODAY`；
  避免 SimNow 实测中“平昨仓位不足”。
- 订单身份/状态落盘到 `flow_path/ctp_orders_<user>.json`（原子写）；
  网关重启后加载同一交易日的映射，重复 rid 直接回放 ack，不再次报单。

命令幂等与重试：

- `IpcClient::send_command_and_wait` 在 ack 超时后可用**同一 rid** 重发
  （`RemoteTdGateway` 最多 2 次）；会话关闭不重发；
- `GatewayIpcServer` 按 `<op>:<rid>` 缓存首次 `CommandAck`；重复命令直接回放
  缓存 ack，**不会再次调用网关**，因此 CTP 报单不会被重复提交；
- 缓存每连接最多 4096 条；连接重建后内存缓存丢失，但 `CtpTdGateway` 会加载
  持久化订单映射，同一 rid 直接回放 ack，**跨网关重启也不会再次报单**；
- 查询/重启对账仍以 CTP 为权威：`ReqQryOrder` 刷新状态，成交/持仓随快照重建；
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

符号约定：

- node/TqApi 统一使用 `EXCHANGE.instrument`（如 `SHFE.rb2701`）；
- CTP 报单/撤单/订阅时拆成 `ExchangeID` + `InstrumentID`；
- CTP 回报/查询/行情 JSON 用 `ExchangeID + '.' + InstrumentID` 还原完整符号；
- CTP 文本字段（`StatusMsg/ErrorMsg`）为 GBK，`cstr()` 统一校验/转 UTF-8，
  避免 JSON 序列化因中文状态消息失败。

## 7. 测试与验证边界

已覆盖：

- `tests/gateway/ctp_mapping_test.cpp`
  - Order/Trade/Account/Position/Depth JSON 映射；
  - GBK 中文 `StatusMsg/ErrorMsg` 转 UTF-8；
  - `CtpTaskQueue` 从其他线程投递后在目标 executor 执行，stop 后丢弃任务；
  - 高优先级报单/成交任务优先于查询回调执行；
- `tests/ipc/gateway_server_test.cpp::DuplicateCommandReusesCachedAck`
  - 同一 `<op>:<rid>` 发送两次，网关只调用一次交易网关，两次都回同一 ack；
- `-DTQSDK_ENABLE_CTP=ON` 全量构建：`libtqsdk_core.so` + `tqsdk-gw`（CTP 已并入核心库）；
- CTP 网关进程启动/停止 smoke：指向不可达前置，确认 `Init/Release`、
  event loop 退出正常；
- 全量测试：CTP 构建下 ctest 通过（联网行情测试仍跳过）。

## 8. SimNow 实测记录

2026-11-03 使用 SimNow 标准仿真环境（BrokerID `9999`，AppID
`simnow_client_test`，AuthCode 16 个 0）：

```text
Trade Front: tcp://182.254.243.31:30001
Market Front: tcp://182.254.243.31:30012
```

已确认（2026-11-03 下午交易时段）：

- CtpMd：前置连接、登录、`SubscribeMarketData(rb2701)` 成功收到 tick；
  `SHFE.rb2701` 最新价 3005、买一 3004、卖一 3005、成交量 496924；
- CtpTd：认证、登录成功（`FrontID=1`，`TradingDay=20261103`）、结算确认；
- 查询：账户 `Balance=916746.38`；持仓/订单/成交查询成功；
- node 首连通过 Resume 拿到 `TradeSnapshot`，REST `/account` 显示柜台余额；
- 报单 `SHFE.rb2701` BUY OPEN LIMIT 3000 返回 ack success，随后
  `OnRtnOrder` 推回 `ALIVE/未成交` + `exchange_order_id`；
- 撤单返回 ack success，`OnRtnOrder` 推回 `FINISHED/已撤单`，node `/orders`
  正确更新 `is_dead=true`；
- 按用户建议用**对价单**：卖一 3002 时以 3003 BUY OPEN 1 手，成交价 3002，
  `/positions` 出现 `volume_long=1`；再以买一-1 的 3000 SELL CLOSETODAY，
  成交价 3001，`volume_long` 归零，完成开仓->成交->平今全链路；
- 发现 CTP 的 `OrderRef` 必须按**十进制数字且递增**处理；带 `r` 前缀或十六进制
  字符串会被判为“不允许重复报单”。实现已将内部 rid 映射为纯数字 OrderRef，
  并对柜台历史 OrderRef 做冲突/递增值维护；
- 自动化平今验证：买入后仅传 `offset=CLOSE`，网关根据今仓自动发
  `CLOSETODAY`，SimNow 成交且持仓归零；
- 订单映射文件实测生成于 `/tmp/tqsdk_ctp_simnow/ctp_orders_259377.json`，
  文件内含当日 rid/OrderRef/订单 JSON；网关重启会加载同一交易日映射；
- 实盘就绪观测：node 启动后、行情未到时 `/readyz` 返回 503；订阅并收到
  `SHFE.rb2701` tick 后返回 200，`/metrics` 显示 md_frames/td_frames/td_gaps；
- 收盘前回归进一步验证：
  - 远价限价单 `ALIVE/未成交` -> 撤单 `FINISHED/已撤单`；
  - 连续 3 笔限价单 + 3 笔撤单全部成功，验证报单节流不会误拒；
  - 过期合约返回 `is_error=true / CTP:找不到合约`；
  - 网关重启后从持久化文件恢复 20 笔订单，原 rid 的挂单仍为 `ALIVE`，
    重启后撤单成功；
  - 用原始 IPC 重放同一 rid 的 InsertOrder，网关直接回放 persisted ack，
    日志为 `duplicate insert ... replayed from persisted store`，未再次发往 CTP；
  - 网关重启窗口内 `/readyz` 持续 503，直到新一轮 CTP 查询 Snapshot 完成才
    恢复 200（约 3.3s），避免旧缓存被误判为已就绪；
- `TradeSnapshotBody.ready` 已加入协议：网关 Resume 下发的旧缓存为 false，
  最新查询完成的快照为 true；node 只在 true 时恢复 td_ready；
- 过期合约 `SHFE.rb2601` 返回“CTP:找不到合约”，验证了合约名校验/错误映射；
- CTP 返回的 GBK 中文 `StatusMsg/ErrorMsg` 已转为 UTF-8，JSON 不再因
  `invalid UTF-8 byte` 中断；
- 实测发现连续 `ReqQryInstrument`（17472 个合约）的回调洪峰会延迟报单
  callback。实现已改为：查询串行化、CTP 回调高低优先级队列、仅针对已有持仓
  按合约懒查规格，不再全市场查询。

未完成/受环境限制：

- 标准仿真 7x24 前置 `40001/40011` 在本机网络不可达，未测试 7x24 环境；
- 未测试真实经纪商、夜盘、高频回报和长时间 soak；
- 成交链路在 SimNow 标准仿真验证，真实柜台仍需专项回归。

> 注：`brokers.json` 中 SimNow 行情端口记录为 `30011`，实测该端口不可用；
> 本次使用 SimNow 官方当前标准环境 `30012`。

## 8. SimNow 自动化验收

启动 `run_live.sh --no-ui` 后，可运行：

```bash
python3 scripts/simnow_acceptance.py --symbol SHFE.rb2701 --metrics
python3 scripts/simnow_acceptance.py --symbol SHFE.rb2701 --orders --yes
```

默认只读；`--orders` 必须配合 `--yes`，避免误写真实 SimNow 账户。完整检查项、
安全策略、市价/对价可选测试和报告格式见
[`SIMNOW_ACCEPTANCE.md`](./SIMNOW_ACCEPTANCE.md)。

脚本自测（本地 fake node，不连接 SimNow）：

```bash
python3 scripts/test_simnow_acceptance.py
```

## 9. SHFE 市价/对价单转换

SHFE 不接受 CTP 的 `AnyPrice` / `BestPrice` 报单类型。node 在
`TqApi::insert_order` 发送命令前，会利用已合并的 quote 做确定性转换：

| 用户意图 | SHFE 实际报单 |
|---|---|
| BUY 市价 | 涨停价 LIMIT + IOC |
| SELL 市价 | 跌停价 LIMIT + IOC |
| BUY 对价 | 卖一价 LIMIT + IOC |
| SELL 对价 | 买一价 LIMIT + IOC |

- 转换发生在 node 侧，`tqsdk-gw` / CTP 最终收到的都是合法限价单；
- 没有有效停板价或买一/卖一价时，下单直接返回错误，不会发送错误的 CTP 报单；
- 其他交易所仍保留原有 `ANY` / `BEST` 映射；
- CTP 网关仍对市价单发送 `LimitPrice=0`，作为最后的字段兜底。

## 10. quote 静态元数据

HTTP quote、批量 `/quotes` 和 WebSocket snapshot 都会从 `InstrumentCatalog`
补齐：

```text
price_tick
volume_multiple
price_decs
```

CTP 行情回调只保证动态行情字段，静态规格以合约目录为准；这样 UI 价格步进、
限价校验和验收脚本不再看到 `price_tick=null`。

## 11. 持仓历史字段

CTP `InvestorPositionField.YdPosition` 现在映射为：

```text
volume_long_his
volume_short_his
```

并保留 `volume_long_today` / `volume_short_today`。这是平昨判断和 UI 持仓拆分
的基础；已有测试覆盖 `CtpConvert.AccountPositionDepth`。

## 12. 交易日/结算阶段

`TradingCalendar` 为 `/readyz` 提供统一交易阶段：

```text
closed / pre_open / open / break / settlement / reconnecting / unknown
```

`/readyz` 的 `td` 对象现在包含：

```json
{
  "connected": true,
  "ready": true,
  "phase": "open",
  "expected_disconnect": false
}
```

- CTP front 在闭市/结算阶段断开时，`expected_disconnect=true`，不应按普通网络故障告警；
- `RemoteTdGateway` 也维护同样的阶段，node 通过 IPC 代理 CTP 时不会丢失该信息；
- 节假日和品种差异仍需要真实交易日历覆盖；当前默认日盘 09:00-15:00、
  夜盘 21:00-次日 02:30，并包含 10:15-10:30 休息。品种差异可用
  `TradingCalendar::Config` 覆盖。

### 交易时段 soak

```bash
python3 scripts/ctp_session_soak.py \
  --manage-processes \
  --gw-config run/live/gw.toml \
  --node-config run/live/node.toml \
  --duration 28800 --interval 30 \
  --out artifacts/ctp_session_YYYYMMDD.jsonl
```

工具把结算/闭市断线记为 expected，把 `open/pre_open/break` 阶段的断线记为
unexpected。CTP 不能以连续 24h 在线作为验收标准，应跨交易日运行并在每次
结算后重新对账。

## 13. 节假日/节前夜盘

交易日历会定时加载：

```text
https://files.shinnytech.com/shinny_chinese_holiday.json
```

- 文件是 JSON 字符串数组，例如 `["2026-10-01", ...]`，只列非周末节假日；
- node/gw 启动时由 `HolidayCalendar::start_global()` 在后台线程加载；
- 本地缓存 `/tmp/tqsdk_holidays.json`，网络失败时保留旧表；
- 默认每 12 小时刷新一次；
- `/readyz` 的 `phase` 会把这些日期视为 Closed。

### 节前夜盘规则

夜盘属于下一个交易日，但：

```text
正常周五夜盘（周末无节假日）    -> 保留
节假日前一晚（如 9/30 晚）     -> 关闭
周五后周一放假                  -> 周五夜盘关闭
节假日当天凌晨                  -> 关闭
```

实现上 `night_session_allowed()` 会向后寻找下一个交易日；如果途中遇到
非周末节假日则拒绝当晚夜盘。这个规则比简单“周五夜盘都开”更严格，避免把
节前夜盘误判为 Open。

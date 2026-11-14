# TradingStatus 与动态端点

## 1. TradingStatus

`include/tqsdk/trading/trading_status.h` 把 `TradingCalendar::Phase` 转成 API/UI
直接消费的状态：

```json
{
  "phase": "open",
  "trading": true,
  "expected_disconnect": false,
  "trading_day": "20261110",
  "session": "night",
  "updated_at_ns": 1794210000000000000
}
```

- 21:00 后的夜盘归入下一交易日；
- Closed/Settlement 设置 `expected_disconnect=true`；
- 前端可用 `phase/session/trading` 决定下单开关和提示；
- REST：`GET /trading-status`。

`TqApi::session_phase()` 优先使用交易网关阶段（CTP 的 phase）；无 TD 或未知时
回退到本地 `TradingCalendar`。

## 2. 动态 MD URL

配置：

```toml
[md]
kind = "tq"
[md.tq]
auto_url = true

[tq_auth]
enabled = true
user_name = "..."
password = "..."
```

`tqsdk-gw` 启动时：

1. `TqAuth.login()`；
2. `DynamicEndpointResolver::md()` 调用官方名称服务获取 `mdurl`；
3. 解析 `wss://host/path`，覆盖静态 `host/port/path`；
4. 失败时保留静态配置并记录 warning。

URL 解析支持 `ws/wss`、显式端口、query 参数；默认端口 80/443。

## 3. 动态 TD URL

配置：

```toml
[td]
auto_url = true
```

`TqAuth.get_td_url()` 返回的是 **Tq 交易网关地址**，不是 CTP `td_front`。
当前实现会解析并记录该 URL 用于诊断；CTP 交易仍使用 `[ctp].td_front`。
接入 TqTdGateway 后可直接消费该 URL，不需要改配置层。

## 4. 测试

```bash
ctest --test-dir build-ui -R 'DynamicEndpoint|TradingStatus' --output-on-failure
```

覆盖 URL 解析、trading day/夜盘归属、Closed/Settlement 预期断线。

## 5. 边界

- `TradingCalendar` 是轻量配置日历；交易所逐合约夜盘差异仍需扩展 session 配置；
- 动态 MD 依赖 TqAuth 网络可用；失败不影响已启动的静态端点；
- 动态 TD URL 的实盘交易需要 TqTdGateway，CTP 不使用该 URL。

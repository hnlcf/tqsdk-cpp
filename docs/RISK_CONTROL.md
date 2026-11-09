# 风控（RiskManager）

## 1. 位置

风控位于 `TqApi::insert_order` / `TqApi::cancel_order` 的入口，在请求送到网关之前执行：

```text
REST /order
  -> TqApi::insert_order
     -> normalize advanced (FAK/FOK)
     -> RiskManager::check_insert
     -> TradingGateway::insert_order
     -> ack success
     -> RiskManager::on_insert
```

被拒绝的订单仍会在本地 DataManager 中生成一条 `is_error=true`、`is_dead=true` 的订单，
`last_msg` 是具体风控原因，便于 REST/WS/UI 显示和审计。

## 2. 规则接口

```cpp
#include "tqsdk/trading/risk_manager.h"

class RiskRule {
public:
    virtual std::optional<std::string> check_insert(const InsertOrderReq&);
    virtual void on_insert(const InsertOrderReq&);
    virtual std::optional<std::string> check_cancel(const CancelOrderReq&);
    virtual void on_cancel(const CancelOrderReq&);
    virtual void on_new_trading_day();
};
```

规则是同步、轻量、单账户的；不要在规则中阻塞等待 I/O。

## 3. 内置规则

| 类 | 作用 |
|---|---|
| `OpenCountsLimitRule` | 指定合约交易日内开仓次数上限 |
| `OpenVolumesLimitRule` | 指定合约交易日内开仓手数上限 |
| `OrderRateLimitRule` | 滑动窗口内报单频率上限 |

```cpp
api.risk_manager().add(
    std::make_shared<OpenCountsLimitRule>(2, std::vector<std::string>{"SHFE.rb2701"}));
api.risk_manager().add(
    std::make_shared<OpenVolumesLimitRule>(10, std::vector<std::string>{"SHFE.rb2701"}));
api.risk_manager().add(
    std::make_shared<OrderRateLimitRule>(20, std::chrono::seconds(1)));
```

## 4. 交易日重置

`RiskManager` 使用中国时区自然日作为轻量交易日 key；跨日自动调用每条规则的
`on_new_trading_day()`。这是与官方 `TqRiskManager` 的近似实现。

后续接交易所交易日历时，应改为使用 `TradingCalendar` 的 trading day，
尤其是夜盘归属下一交易日的场景。当前版本不会把夜盘归入下一交易日，文档和测试都已注明。

## 5. REST 验收接口

```text
GET    /risk/status
POST   /risk/rules
DELETE /risk/rules
```

示例：

```bash
curl -s -X POST http://127.0.0.1:18081/risk/rules \
  -H 'content-type: application/json' \
  -d '{"type":"OPEN_COUNT_LIMIT","limit":1,"symbols":["SHFE.rb2701"]}'
```

规则只对之后通过的订单生效，不追溯已有订单。清除规则：

```bash
curl -s -X DELETE http://127.0.0.1:18081/risk/rules
```

## 6. 测试

```bash
ctest --test-dir build-ui -R 'RiskRule|RiskManager|OrderTasks.Risk' --output-on-failure
```

覆盖次数、手数、频率、规则移除，以及风控拒绝发生在网关之前。

# 多账户与回测

## 1. MultiAccountManager

`include/tqsdk/multiaccount/multi_account_manager.h` 用“每账户独立 TqApi”的方式
保证账户隔离：

```cpp
MultiAccountManager manager;
manager.add_account("sim-a", api_a);
manager.add_account("sim-b", api_b);

auto* order = co_await manager.insert_order("sim-a", request);
auto  cancel = co_await manager.cancel_order("sim-b", cancel_req);

json snap = manager.snapshot();
// {"sim-a": {"account":..., "positions":..., ...}, "sim-b": ...}
```

特性：

- 按 account_id 路由 insert/cancel；
- 快照按键聚合，不混合不同账户的 DataManager；
- 未知账户返回失败/null，不把订单静默发到默认账户；
- 可分别用两个 SimTd/Remote TD 构造独立 TqApi；
- `close()` 统一关闭全部账户 API。

边界：

- 当前 node 进程仍只构造一个上游 TqApi；REST 多账户路由尚未接入 node 配置；
- 多账户共享一个 gw 的部署需要 gw 支持多交易连接；框架 API 已预留；
- 风控规则应每个账户独立配置（每 TqApi 一个 RiskManager）。

## 2. BacktestEngine

`include/tqsdk/backtest/backtest_engine.h` 是独立事件驱动 bar 回测：

```cpp
BacktestConfig cfg;
cfg.volume_multiple = 10;
cfg.commission_per_lot = 1;
BacktestEngine engine(cfg);

BacktestStats stats = engine.run(bars, [](BacktestContext& ctx) {
    if (ctx.bar().datetime == open_time) ctx.target_position(1);
    if (ctx.bar().datetime == close_time) ctx.target_position(0);
});
```

支持：

- `buy_open/sell_open/buy_close/sell_close`；
- `target_position` 自动开平/反手；
- 成交价默认 bar close，可切换为 bar open；
- 手续费（按手 + 按成交额）；
- 已实现/浮动盈亏、权益、最大回撤、成交数、胜出平仓次数。

边界：

- Bar 级回测，不含撮合队列/盘口滑点/涨跌停拒单；
- 不模拟保证金强平、交割、结算价和交易所手续费差异；
- 成交在策略回调时立即按当前 bar 价格完成，属于研究级近似；
- 需要 tick/盘口级回测时，应使用 SimTd 或真实回放通道。

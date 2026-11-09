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

- node 现在支持默认账户 + 可选 `[[accounts]]`：每个额外账户连接独立 gw 端点；
- REST `/accounts` 和 `?account=<id>` 已支持按账户查询账户/持仓/委托/成交，
  `POST /order?account=<id>` 已支持按账户下单；
- WebSocket 仍由默认账户提供（多账户 WS 过滤是下一步）；
- 每个账户有独立 `TqApi`/`DataManager`；风控/任务目前仍默认落在默认账户，
  需要后续把 task/risk 路由扩展 account 参数。

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

// tick 回放：TickStore::load_range 后直接驱动同一套账户/持仓/手续费模型
BacktestStats tick_stats = engine.run_ticks(ticks, [](BacktestTickContext& ctx) {
    if (ctx.tick().datetime == open_time) ctx.buy_open(1);
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
- 新增 tick 级回测 `run_ticks()`：成交价使用 `tick.last_price`，可配合
  `TickStore::load_recent/load_range` 回放本地 SQLite；
- tick 回测暂不把字符串 datetime 映射为 BacktestFill 的 TimeStamp，成交时间保留 0；
- 需要撮合队列/盘口滑点/涨跌停拒单时，仍应使用 SimTd 或扩展撮合模型。

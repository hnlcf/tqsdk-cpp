# Tick / DataSeries / DataDownloader

## 1. 模型

天勤行情协议以 quote diff 为原子更新。C++ 侧在 quote 合并后把有效快照固化为
`Tick`，并写入固定窗口 `TickSeries`：

```text
md diff -> DiffMergeEngine -> Quote
                         └─ TickSeries.append_quote(quote)
                               └─ DataSeries<Tick>
```

- `include/tqsdk/domain/tick.h`：Tick 字段与 Quote 的稳定映射；
- `include/tqsdk/data/data_series.h`：固定容量窗口；
- `include/tqsdk/data/tick_series.h`：按合约去重后的 tick 窗口；
- `DataManager::ticks`：独立 registry，不参与交易对账清空。

## 2. TqApi

```cpp
TickSeries* ticks = api.get_tick_serial("SHFE.rb2701", 200);
```

- 首次调用会创建 tick 窗口并订阅 quote；
- `ticks->data.size()`、`at()`、`back()` 可读取；
- 相同的 `datetime/last_price/bid1/ask1/volume` 指纹不会重复追加；
- 窗口容量裁剪只保留最近 N 个快照。

REST：

```text
GET /ticks/{symbol}                         # 内存窗口
GET /ticks/{symbol}?source=cache&limit=200  # TickStore 历史
GET /tick-cache/status                      # 缓存状态
```

返回：

```json
{"instrument_id":"SHFE.rb2701","source":"cache","ticks":[{...}]}
```

## 2.1 TickStore 持久化

`TickStore` 把每条新 `Tick` 异步写入 SQLite（WAL），供重启后回放/分析：

```toml
[market_cache]
file = "market_cache.sqlite3"   # K 线与 tick 共用同一个 SQLite

[tick_cache]
enabled = true
max_ticks_per_series = 200000
flush_interval_ms = 250
max_pending_ticks = 50000
```

```cpp
TickStore store(cfg);
api.set_tick_sink([&](const Tick& tick) { store.append(tick); });
auto recent = store.load_recent("SHFE.rb2701", 200);
auto range  = store.load_range("SHFE.rb2701", start, end, 5000);
store.flush();
```

- 异步写线程，队列满时计数 drop，不阻塞行情合并；
- 按 `symbol` 保留最近 `max_ticks_per_series` 条；
- 读写连接分离，SQLite 打开失败自动降级；
- `run_live.sh` 支持 `TICK_CACHE_ENABLED=1` 和 `TICK_CACHE_MAX_TICKS`。

## 3. DataDownloader

```cpp
DataDownloader downloader;
downloader.download_ticks(*ticks, "ticks.csv");
downloader.download_klines(kline_stream, "bars.csv");
```

- UTF-8 CSV，可选表头/分隔符/最近 N 条；
- 不隐式访问网络，先由 TqApi 准备数据；
- 导出包含五档盘口与 K 线 OHLC/oi 字段；
- 适合策略回放、离线分析和验收归档。

## 4. 边界

- tick 是 quote 快照，不是逐笔成交；同价位盘口变化是否产生新 tick 由指纹决定；
- tick 默认不落盘；显式启用 `[tick_cache]` 后才写 SQLite，导出仍可用 DataDownloader；
- tick 容量由调用方控制，避免无界内存增长；
- 高频合约建议使用较小的 view_width，并根据磁盘预算设置 `max_ticks_per_series`；
- TickStore 存的是 quote 快照，不替代交易所逐笔成交明细。

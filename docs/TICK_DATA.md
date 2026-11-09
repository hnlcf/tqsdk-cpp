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
GET /ticks/{symbol}
```

返回：

```json
{"instrument_id":"SHFE.rb2701","ticks":[{...}]}
```

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
- 不落盘 tick（K 线有 SQLite 缓存，tick 导出由 DataDownloader 负责）；
- tick 容量由调用方控制，避免无界内存增长；
- 高频合约建议使用较小的 view_width，并在策略侧按需归档。

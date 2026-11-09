# node K 线历史缓存（SQLite）

> 专题文档。总体架构/模块/信息流/状态机以 [ARCHITECTURE.md](ARCHITECTURE.md)、
> [MODULES.md](MODULES.md)、[FLOWS.md](FLOWS.md)、[STATE_MACHINES.md](STATE_MACHINES.md)
> 和 [PROTOCOLS.md](PROTOCOLS.md) 为准。

> 目标：node 重启后先使用本地历史，避免每次重新从远端拉取完整 200 根 K 线；
> 本地缓存足够新时，只向上游请求最新 2 根补差。

## 1. 为什么放在 node 层

K 线合并由 node 的 `TqApi` / `DataManager` 完成，缓存写入点天然在
`DiffMergeEngine` 合并之后：

```text
TqMd / RemoteMd
      │  kline diff
      ▼
TqApi.md_loop
      │
DiffMergeEngine.merge_one_kline
      │
      ├─ DataManager.klines ── HTTP /kline、WS /stream
      │
      └─ KlineStore.put_bars ── 后台 SQLite writer
```

放在 node 层而不是 gw 层的好处：

- `gw` 可以保持无状态，便于多 node / 多客户端；
- 缓存格式与 node 的 `KlineBar`、周期、容量语义一致；
- node 重启、升级时缓存仍然有效；
- gw 的 IPC 首帧、Resume/replay 逻辑不需要感知 SQLite。

## 2. 存储模型

### 2.1 schema

```sql
CREATE TABLE kline_bars (
    symbol       TEXT    NOT NULL,
    duration_ns  INTEGER NOT NULL,
    datetime_ns  INTEGER NOT NULL, -- K 线起始时间，epoch nanoseconds
    open         REAL    NOT NULL,
    high         REAL    NOT NULL,
    low          REAL    NOT NULL,
    close        REAL    NOT NULL,
    volume       INTEGER NOT NULL DEFAULT 0,
    open_oi      INTEGER NOT NULL DEFAULT 0,
    close_oi     INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (symbol, duration_ns, datetime_ns)
) WITHOUT ROWID;

CREATE TABLE kline_series (
    symbol        TEXT    NOT NULL,
    duration_ns   INTEGER NOT NULL,
    last_bar_ns   INTEGER NOT NULL DEFAULT 0,
    last_write_ns INTEGER NOT NULL DEFAULT 0, -- 实际进程写入时间，用于 freshness
    bar_count     INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (symbol, duration_ns)
) WITHOUT ROWID;
```

- 主键 `(symbol, duration_ns, datetime_ns)`：同一根 bar 只存一份，更新用 upsert。
- `WITHOUT ROWID`：按主键顺序聚集，读最近 N 根很快。
- WAL：读写连接分离，读缓存不阻塞后台写入。
- `kline_series.last_write_ns` 是“进程最后写缓存时间”，不是 bar 时间；用于判断
  进程是否刚刚重启过。

### 2.2 写入路径

- `persist_changed_klines()` 在每次 `merger_.apply()` 后调用；
- 只写有 `Reset/LastUpdated/Appended` 变化的序列；
- `Reset` 写完整当前序列，单根更新只写最后一根；
- `KlineStore` 用内存 `map` 对 `(symbol,duration,datetime)` 合并，后台线程按
  `flush_interval_ms` 批量 flush；
- writer 队列有上限，慢磁盘时丢最旧/超限条目并统计 `dropped`，绝不反压行情
  core strand。

## 3. 读取与远端补差

### 3.1 启动/首次订阅

`TqApi::get_kline_serial()` 或 `/stream` 首次订阅时：

1. `KlineStore.load_recent(symbol, duration, view_width)` 从 SQLite 读历史；
2. 合并进 `KlineStream.bars`；
3. 标记 `cached_history = true`；
4. 读取 `kline_series.last_write_ns`：
   - 距现在 `<= min(fresh_after_sec, max(60s, 2*duration))`：远端 `set_chart`
     只请求 `min(view_width, 2)`；
   - 否则：远端请求完整 `view_width`，覆盖/补齐停机期间可能缺失的历史；
   - 对 1 分钟 K 线，这个上限通常只有 2 分钟，避免停机 14 分钟后只补 2 根
     而留下 12 根缺口；对 1 小时 K 线由 `fresh_after_sec` 上限约束。
5. 远端历史帧返回后，`DiffMergeEngine` 按 `datetime` 归并，**不会清掉本地
   更早的 bar**。

`/kline` REST 第一次调用时就会拿到本地历史，不需要等远端首帧。

### 3.2 为什么远端只取 2 根

天勤 `set_chart` 的 `view_width` 决定服务端返回的历史根数。缓存新鲜时：

```text
本地 SQLite: 3000 ... 3050
远端 tail:                3051 (当前 bar)
```

只取最新 2 根足以补上断线/重启期间正在形成的一根；更早的历史完全由 SQLite
提供，避免重复下载。

### 3.3 停机期间缺失

如果重启距上次缓存写入超过上述自适应窗口，视为可能跨越了交易时段：

- 仍会加载本地历史用于立即可读；
- 远端请求完整 `view_width`；
- 新旧数据按 datetime 合并，远端能覆盖的重叠 bar 以远端为准；
- 远端窗口之外的历史仍保留在 SQLite，但 `KlineStream.capacity` 只保留最近
  `view_width` 根。

## 4. 配置

`config/node.toml`：

```toml
[market_cache]
file = "market_cache.sqlite3"   # K 线与 tick 共用同一个 SQLite

[kline_cache]
enabled = true
max_bars_per_series = 2000
flush_interval_ms = 250
max_pending_bars = 50000
fresh_after_sec = 900
```

字段：

| 字段 | 说明 |
|---|---|
| `enabled` | 关闭后完全不影响主链路 |
| `file` | 未设置时继承 `[market_cache].file`；相对路径按 node 工作目录解析 |
| `max_bars_per_series` | 每个 `(symbol,duration)` 保留上限，写入后裁剪 |
| `flush_interval_ms` | writer 批量落盘周期 |
| `max_pending_bars` | 未落盘队列上限，超限丢弃并计数 |
| `fresh_after_sec` | 缓存新鲜窗口上限；实际还受 `max(60s, 2*duration)` 约束 |

## 5. 状态接口

```text
GET /kline-cache/status
```

返回：

```json
{
  "enabled": true,
  "ready": true,
  "writer_failed": false,
  "file": "market_cache.sqlite3",
  "max_bars_per_series": 2000,
  "flush_interval_ms": 250,
  "fresh_after_sec": 900,
  "accepted": 1234,
  "written": 1200,
  "dropped": 0,
  "flushes": 8,
  "pending": 0,
  "last_error": ""
}
```

`/metrics` 的 `kline_store` 字段同一份数据。

## 6. 语义边界

1. **不是行情源**：缓存只保存已经收到的 bar；从未订阅过的合约没有本地历史。
2. **不是 tick 存储**：只存 OHLC/volume/OI，不存逐笔。
3. **新鲜度是启发式**：没有交易所交易日历时，用 `last_write_ns` 判断重启间隔；
   长假后第一次启动会请求完整 `view_width`，这是有意的保守选择。
4. **reconnect**：RemoteMdGateway 重连时按至少 200 根重放 K 线订阅，
   以补齐可能缺失的窗口；因此重连会比正常启动多取一些历史。
5. **合约到期/换月**：`symbol` 是完整合约代码，不同合约不会串数据。
6. **容量裁剪**：每个序列按 `max_bars_per_series` 保留最新 N 根，不保留全历史。
7. **SQLite 故障**：打开/写入失败会降级，行情和交易继续工作；状态接口暴露
   `writer_failed/last_error`。

## 7. 测试

```text
KlineStore.PersistsAndReloads
KlineStore.PrunesToNewestBars
KlineStore.SkipsIncompleteBars
KlineStore.DisabledIsNoop
KlineStore.CorruptDatabaseDegrades
KlineStore.ConcurrentProducersAndFlush
KlineStore.FreshnessAdaptsToBarInterval
KlineCache.HydratesBeforeRemoteTailAndMerges
KlineCache.NoCacheRequestsFullWidth
KlineCache.StaleCacheRequestsFullWidth
```

集成测试验证了：

- 5 根本地缓存先进入 `KlineStream`；
- fresh 缓存时 MockMd 收到 `view_width=2`；
- 远端只返回 2 根时，本地 5 根不被覆盖，合并成 6 根；
- 新 bar 被异步入库，重新打开 SQLite 后仍可读；
- 无缓存时请求完整 `view_width=200`。

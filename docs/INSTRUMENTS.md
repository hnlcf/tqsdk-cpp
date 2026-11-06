# 标准合约目录（symbol.json / latest.json）

> 专题文档。总体架构/模块/信息流/状态机以 [ARCHITECTURE.md](ARCHITECTURE.md)、
> [MODULES.md](MODULES.md)、[FLOWS.md](FLOWS.md)、[STATE_MACHINES.md](STATE_MACHINES.md)
> 和 [PROTOCOLS.md](PROTOCOLS.md) 为准。

> 数据源：天勤 openmd
>
> - `https://openmd.shinnytech.com/t/md/symbols/symbol.json`（约 35MB，
>   只含当前有效合约）
> - `https://openmd.shinnytech.com/t/md/symbols/latest.json`（约 350MB，
>   含历史合约/更完整的字段）
>
> 文件均不入库；由 node 在本地加载，空闲时下载更新。

## 1. 目录格式

两个文件都是：

```json
{
  "SHFE.rb2701": {
    "class": "FUTURE",
    "instrument_id": "SHFE.rb2701",
    "exchange_id": "SHFE",
    "ins_id": "rb2701",
    "ins_name": "螺纹2701",
    "product_id": "rb",
    "last_price": 3017.0,
    "pre_volume": 715629,
    "open_interest": 1855764,
    "volume_multiple": 10.0,
    "price_tick": 1.0,
    "price_decs": 0,
    "expired": false
  }
}
```

期权额外字段：

```json
{
  "class": "FUTURE_OPTION",
  "option_class": "CALL",
  "strike_price": 3000,
  "underlying_symbol": "SHFE.rb2701",
  "expire_datetime": 1798182000.0,
  "delivery_year": 2027,
  "delivery_month": 1,
  "product_short_name": "螺纹钢期权"
}
```

`class` 主要取值：

- `FUTURE`：期货合约
- `FUTURE_CONT`：期货主连
- `FUTURE_INDEX`：指数/指数主连
- `INDEX`：指数
- `FUTURE_OPTION`：商品期货期权
- `OPTION`：指数期权
- `FUTURE_COMBINE`：组合合约
- `SPOT`：现货

`symbol.json` 的 29,439 条均为 `expired=false`；`latest.json` 有约 25 万条，
包含历史合约。运行时默认加载 `symbol.json`，查询时默认过滤 `expired`。

## 2. 配置

`tqsdk-node` 的 `[instruments]`：

```toml
[instruments]
enabled = true
file = "symbol.json"
url = "https://openmd.shinnytech.com/t/md/symbols/symbol.json"
refresh_interval_sec = 21600
initial_delay_sec = 60
```

- `file`：本地文件；不存在时依次尝试 `symbol.json`、`latest.json`
- `url`：后台下载地址；可改成 `latest.json`
- `refresh_interval_sec`：下载刷新周期，默认 6 小时
- `initial_delay_sec`：本地目录已加载时，首次下载前先等待，避免启动抖动

启用后：

1. 启动时同步加载本地文件；
2. 后台线程在空闲窗口下载并原子替换文件；
3. 解析成功后一次 swap 到读侧，HTTP 查询不阻塞行情/交易线程；
4. 下载/解析失败保留旧目录。

## 3. HTTP API

```text
GET /catalog/status
GET /catalog/instruments?classes=FUTURE,FUTURE_CONT&exchange=SHFE&q=rb&limit=100
GET /catalog/instruments/{instrument_id}
GET /catalog/options/products
GET /catalog/option-chain/{underlying}
```

`/catalog/instruments` 支持：

- `classes`：逗号分隔，如 `FUTURE,FUTURE_CONT,FUTURE_INDEX`
- `exchange`：如 `SHFE`
- `product`：如 `SHFE.rb`
- `q`：代码/中文名/拼音模糊搜索
- `underlying`：按标的过滤
- `limit` / `offset`

`/catalog/options/products` 返回每个 `(exchange, product, underlying_symbol)`
的看涨/看跌数量和到期时间，前端用于三级选择器。

`/catalog/option-chain/{underlying}` 返回：

- 标的合约和实时 quote
- CALL/PUT 合约元数据
- 每个期权的最新 quote
- `expire_datetime` / product / product_name

## 4. 实时行情

目录只负责“合约是什么”，行情仍来自现有数据面：

```text
GET /quote/{symbol}
GET /kline/{symbol}/{duration_ns}
WS  /stream
```

前端通过 Vite proxy：

```text
/api/*  ->  http://127.0.0.1:18081/*
```

启动示例：

```fish
cd web
nvm use lts
env VITE_PROXY_TARGET=http://127.0.0.1:18082 npm run dev
```

`18082` 是直接使用天勤行情的 node；生产环境应把 `/api` 反向代理到实际 node。

## 5. 内存与性能

- `symbol.json`：35MB JSON，解析后只保留约 20 个字段；
- `latest.json`：约 350MB，解析峰值更高，适合离线/历史查询；
- 当前运行时 29,439 条目录的 RSS 增量约几十 MB；
- 查询按 `instrument_id` 建索引，列表在共享锁下过滤/排序；
- 期权链一次最多返回 2,000 个合约的元数据和 quote，避免大品种链阻塞。

## 6. 已知边界

- `latest.json` 的 `last_price` 是快照字段，不是实时行情；实时价格必须走
  `/quote` / `/stream`。
- 目录下载是后台线程同步 HTTPS；下载期间不影响 HTTP 读取。
- `symbol.json` 与 `latest.json` 字段可能随天勤更新；解析器对缺失字段使用默认值。
- 期权链 quote 会触发对应合约订阅；大链首次加载可能较慢。

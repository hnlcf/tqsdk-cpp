# 配置模型

配置模型集中定义在 `include/tqsdk/config/model.h`，加载器在
`include/tqsdk/config/tqapi_factory.h`。不再保留旧的 `TqApiConfig`、`GwConfig`
三套重复结构，也不再兼容旧 `[listen]`、node `[md]/[td] remote` 或
`gw_host/gw_port` 配置。

## 1. 三层模型

```text
IpcConfig       一组 gw/node IPC 交互
EngineConfig    运行时引擎：ipc + md + td + ctp
GatewayConfig   gw：engine + tq_auth + runtime
NodeConfig      node：accounts + serve + cache + tq_auth + audit + runtime
```

- `EngineConfig` 是 `make_tqapi()` 的输入，不是独立配置文件入口；
- `GatewayConfig` 和 `NodeConfig` 才是 gw/node 的 app 配置；
- 所有 app 配置字段都在 `model.h`，实现类自己的 `Config` 只作为叶子类型组合，
  不再散落在入口解析代码里。

## 2. 启动与目录

```bash
./build-ui/tqsdk-gw   -c config
./build-ui/tqsdk-node -c config
```

```text
config/
├── gw.toml
├── node.toml
├── ipc/
│   ├── gw-a.toml
│   └── gw-b.toml
└── shared/
    ├── tq.toml
    ├── tq_md.toml
    ├── ctp.toml
    ├── sim.toml
    └── cache.toml
```

## 3. IPC 组

`config/ipc/<key>.toml` 只描述一组 gw/node 交互：

```toml
[ipc]
name = "gw-a"
transport = "tcp"
tcp_host = "127.0.0.1"
tcp_port = 4321
# UDS：
# transport = "uds"
# socket = "/tmp/tqsdk-gw-a.sock"
encoding = "json"
handshake_timeout_ms = 5000
heartbeat_interval_ms = 5000
heartbeat_timeout_ms = 15000
```

gw 选一组：

```toml
[gateway]
ipc = "gw-a"
```

node 每个账户选一组：

```toml
primary_account = "default"

[[accounts]]
id = "default"
ipc = "gw-a"

[[accounts]]
id = "b"
ipc = "gw-b"
```

node 不配置 md/td，每个账户固定构造 Remote MD + Remote TD；两者共用该 IPC 组。

也支持入口内联组字典：

```toml
[gateway]
ipc = "gw-a"

[ipc.gw-a]
transport = "tcp"
tcp_host = "127.0.0.1"
tcp_port = 4321
```

## 4. gw 配置

```toml
[gateway]
ipc = "gw-a"

[include]
files = ["shared/tq.toml", "shared/tq_md.toml", "shared/ctp.toml", "shared/sim.toml"]

[md]
kind = "tq"

[td]
kind = "sim"
```

网关叶子配置统一嵌套：

```toml
[md.tq]
host = "free-openmd.shinnytech.com"
port = "443"
path = "/t/md/front/mobile"
auto_url = false

[td.tq]
broker_name = "simnow"
account_id = "..."
password = "..."
broker_id = ""
front_url = ""
td_url = ""
account_key = ""

[td.sim]
initial_balance = 1000000.0
[td.sim.specs."SHFE.rb2610"]
volume_multiple = 10.0
price_tick = 1.0
margin_rate = 0.1
[td.sim.last_prices]
"SHFE.rb2610" = 3800.0

[ctp]
# 字段和 CtpConfig 一致
```

## 5. node 配置

`serve` 和 `cache` 是聚合 section：

```toml
[serve]
bind = "127.0.0.1"
port = 8080
symbols = ["SHFE.rb2701"]

[serve.auth]
enabled = false
token_env = "TQSDK_SERVE_TOKEN"

[serve.ws]
path = "/stream"
heartbeat_interval_ms = 30000
max_clients = 256
max_queue = 1024
coalesce_ms = 10

[cache]
file = "market_cache.sqlite3"

[cache.instruments]
enabled = true
file = "symbol.json"
refresh_interval_sec = 21600
initial_delay_sec = 60

[cache.kline]
enabled = true
max_bars_per_series = 2000
flush_interval_ms = 250
max_pending_bars = 50000
fresh_after_sec = 900

[cache.tick]
enabled = false
max_ticks_per_series = 200000

[cache.continuous]
enabled = true
file = "continuous_table.json"
refresh_interval_sec = 86400
```

`[cache].file` 是 kline/tick 的默认共享 SQLite 文件；`[cache.kline].file` 或
`[cache.tick].file` 显式设置时才覆盖。

## 6. 配置归属

| 配置 | 文件/入口 |
|---|---|
| IPC name/transport/endpoint/超时 | `ipc/<key>.toml` |
| gw 监听、真实网关选择 | `gw.toml` + `[gateway].ipc` |
| node 账户/IPC 选择 | `node.toml` + `[[accounts]]` |
| TQ MD endpoint | `shared/tq_md.toml` | gw only |
| TQ TD/TqAuth | `shared/tq.toml` | gw 与 node 目录认证共用 TqAuth |
| CTP | `shared/ctp.toml` |
| Sim 规格/初始价格 | `shared/sim.toml` |
| serve/audit/runtime | `node.toml` |
| 合约目录、K线/tick/主连缓存 | `shared/cache.toml` |

## 7. include

- 路径相对于当前文件；
- 后面的 include 覆盖前面的；
- 入口文件最后合并；
- table 递归合并，标量和数组替换；
- 禁止循环，最大深度 32；
- 缺失 include 返回 `file_io`，语法错误返回 `parse`。

## 8. 本地凭据

```bash
cp -a config config.local
chmod 600 config.local/shared/tq.toml config.local/shared/ctp.toml
./build-ui/tqsdk-gw -c config.local
./build-ui/tqsdk-node -c config.local
```

`config.local/` 已 gitignore。

## 9. 验证

```bash
ctest --test-dir build-ui -R 'TqApiConfigTest|TqApiFactoryTest' --output-on-failure
```

覆盖 IPC 组文件/内联组、嵌套 md/td、node serve/cache、include 优先级/循环、
TQ broker_name 和 Remote 引擎构造。

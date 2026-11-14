# 配置目录与 IPC 组

本文描述 gw/node 的目录式配置。目标是让一组 gw/node IPC 交互只维护一份配置，
gw/node 入口只保留进程特有项。

## 1. 启动方式

```bash
./build-ui/tqsdk-gw   -c config
./build-ui/tqsdk-node -c config
```

`-c` 可以是目录，也可以是单个 TOML 文件（兼容旧配置）：

- 目录：gw 自动读取 `<dir>/gw.toml`，node 自动读取 `<dir>/node.toml`；
- 文件：直接读取该文件；文件内的 `[include]` 同样生效。

## 2. 仓库示例布局

```text
config/
├── gw.toml                 # gw 入口：gateway.ipc、md/td.kind、runtime
├── node.toml               # node 入口：accounts、serve、audit、runtime
├── ipc/
│   ├── gw-a.toml           # 一组 gw/node IPC 交互
│   └── gw-b.toml           # 可选的第二组
└── shared/
    ├── tq.toml             # TqAuth、TQ MD 默认地址、TQ TD 账户
    ├── ctp.toml            # CTP 前置和账户
    ├── sim.toml            # SimTd 规格/初始价格
    └── cache.toml          # 合约目录、SQLite 缓存、主连表
```

## 3. IPC 组

一个 `config/ipc/<key>.toml` 代表一组 gw/node 交互。超时、name、编码、UDS/TCP
端点全部在这里：

```toml
# config/ipc/gw-a.toml
[ipc]
name = "gw-a"
transport = "tcp"
tcp_host = "127.0.0.1"
tcp_port = 4321
# UDS 时改为：
# transport = "uds"
# socket = "/tmp/tqsdk-gw-a.sock"
encoding = "json"
handshake_timeout_ms = 5000
heartbeat_interval_ms = 5000
heartbeat_timeout_ms = 15000
```

gw 入口通过 key 选择要监听的组：

```toml
# config/gw.toml
[gateway]
ipc = "gw-a"
```

node 入口每个账户通过 key 选择要连接的组：

```toml
# config/node.toml
primary_account = "default"

[[accounts]]
id = "default"
ipc = "gw-a"

[[accounts]]
id = "account-b"
ipc = "gw-b"
```

node 不支持 `md.kind` / `td.kind`，也不需要在 node 里写 remote host/port；
每个账户固定构造为 Remote MD + Remote TD，两者共用该 IPC 组。

### 3.1 内联字典

除了多个文件，也可以在入口中用 `[ipc.<key>]` 放组字典（适合测试或小型配置）：

```toml
[gateway]
ipc = "gw-a"

[ipc.gw-a]
name = "gw-a"
transport = "tcp"
tcp_host = "127.0.0.1"
tcp_port = 4321
```

文件和内联组二选一；同名时内联组优先。

## 4. 入口与共享片段

gw 入口只保留：

```toml
[gateway]
ipc = "gw-a"

[include]
files = ["shared/tq.toml", "shared/ctp.toml", "shared/sim.toml"]

[md]
kind = "tq"
[td]
kind = "sim"
```

node 入口只保留账户/IPC key、serve、audit、runtime：

```toml
[include]
files = ["shared/tq.toml", "shared/cache.toml"]

primary_account = "default"

[[accounts]]
id = "default"
ipc = "gw-a"

[serve]
bind = "127.0.0.1"
port = 8080
```

## 5. include 规则

```toml
[include]
files = ["shared/tq.toml", "shared/cache.toml"]
```

- 路径相对于**当前文件**；
- 按数组顺序加载，后面的覆盖前面的；
- 入口文件最后合并，优先级最高；
- TOML table 递归合并，标量和数组直接替换；
- 支持嵌套 include，但禁止循环，最大深度 32；
- include 缺失返回 `ConfigErrorCode::file_io`，语法错误返回 `parse`。

## 6. 配置归属

| 配置 | 归属 | 说明 |
|---|---|---|
| IPC 组（name/transport/endpoint/超时） | `ipc/<key>.toml` | gw 监听、node 连接 |
| `[gateway].ipc` | gw 入口 | 选择 IPC 组 |
| `[[accounts]].ipc` | node 入口 | 每个账户选择一个 IPC 组 |
| `[md]` TQ 默认地址 | `shared/tq.toml` | gw 入口选择 kind |
| `[td.tq]` | `shared/tq.toml` | `broker_name`、账号、可选 `broker_id` |
| `[tq_auth]` | `shared/tq.toml` | gw TD/MD 和 node 目录下载共用 |
| `[ctp]` | `shared/ctp.toml` | gw CTP |
| `[td].specs/last_prices` | `shared/sim.toml` | gw SimTd |
| `[instruments]`/缓存 | `shared/cache.toml` | node |
| `[serve]` | node 入口 | HTTP/WS 对外配置 |
| `[runtime]` | 入口 | gw/node 可不同线程数 |

## 7. 本地凭据

仓库中的 `shared/tq.toml`、`shared/ctp.toml` 只放占位符。真实账号建议使用
不会入库的副本：

```bash
cp -a config config.local
chmod 600 config.local/shared/tq.toml config.local/shared/ctp.toml
$EDITOR config.local/shared/tq.toml
$EDITOR config.local/shared/ctp.toml

./build-ui/tqsdk-gw -c config.local
./build-ui/tqsdk-node -c config.local
```

`config.local/` 已加入 `.gitignore`。不要把密码、token 或 AuthCode 写进
tracked 的 `config/`。

## 8. 兼容与迁移

旧单文件入口仍支持；旧 `[listen]` + `[ipc]` 或 node `[md]/[td] remote` 会
按兼容路径读入，但新配置应迁移到 `ipc/<key>.toml`。`run_live.sh` 生成的
`run/live/*.toml` 仍是自包含单文件，和目录模式可共存。

## 9. 错误诊断

```text
failed to load config '...': <detail> (<file_io|parse|invalid_value>)
```

IPC 组缺失、循环 include、UDS/TCP 端点不完整都会在启动阶段明确失败。

## 10. 验证

```bash
ctest --test-dir build-ui -R TqApiConfigTest --output-on-failure
```

覆盖目录入口、IPC key 解析、include 优先级/循环、node 账户选择、TQ broker_name
校验和旧配置兼容。

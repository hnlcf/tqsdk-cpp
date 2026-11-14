# 配置目录与 include

本文描述 gw/node 的目录式配置。目标是让 IPC、TQ、CTP、缓存等可复用配置只维护
一份，gw/node 入口只保留进程特有项。

## 1. 启动方式

```bash
./build-ui/tqsdk-gw   -c config
./build-ui/tqsdk-node -c config
```

`-c` 可以是目录，也可以是单个 TOML 文件（兼容旧配置）：

- 目录：gw 自动读取 `<dir>/gw.toml`，node 自动读取 `<dir>/node.toml`；
- 文件：直接读取该文件；文件内的 `[include]` 同样生效。

推荐目录式启动，避免 gw/node 重复维护 IPC/TQ/缓存配置。

## 2. 仓库示例布局

```text
config/
├── gw.toml                 # gw 入口：listen、md/td.kind、runtime
├── node.toml               # node 入口：upstream、serve、audit、runtime
└── shared/
    ├── ipc.toml            # IPC encoding/timeouts
    ├── tq.toml             # TqAuth、TQ MD 默认地址、TQ TD 账户
    ├── ctp.toml            # CTP 前置和账户
    ├── sim.toml            # SimTd 规格/初始价格
    └── cache.toml          # 合约目录、SQLite 缓存、主连表
```

入口只选择“用哪套网关”，共享文件保存同一类参数：

```toml
# config/gw.toml
[include]
files = [
    "shared/ipc.toml",
    "shared/tq.toml",
    "shared/ctp.toml",
    "shared/sim.toml",
]

[md]
kind = "tq"   # 只改这里
[td]
kind = "sim"  # sim / ctp / tq
```

## 3. include 规则

```toml
[include]
files = ["shared/ipc.toml", "shared/tq.toml"]
```

- 路径相对于**当前文件**；
- 按数组顺序加载，后面的文件覆盖前面的值；
- 入口文件最后合并，优先级最高；
- TOML table 递归合并，标量和数组直接替换；
- 支持嵌套 include，但禁止循环，最大深度 32；
- include 文件缺失返回 `ConfigErrorCode::file_io`，语法错误返回 `parse`。

示例：`shared/ipc.toml` 设置通用超时，node 入口覆盖 `transport`、`role`、
`local_name`：

```toml
# shared/ipc.toml
[ipc]
encoding = "json"
heartbeat_interval_ms = 5000

# node.toml
[ipc]
transport = "tcp"
role = "node"
local_name = "tqsdk-node"
```

## 4. 配置归属

| 配置 | 归属 | 说明 |
|---|---|---|
| `[listen]` | gw 入口 | gw 监听端点 |
| `[ipc]` | `shared/ipc.toml` + 入口覆盖 | 编码/心跳；role/transport 由入口定 |
| `[md]` TQ 默认地址 | `shared/tq.toml` | kind 由 gw 入口选择 |
| `[td.tq]` | `shared/tq.toml` | `broker_name`、账号、可选 `broker_id` |
| `[tq_auth]` | `shared/tq.toml` | 快期认证，gw TD/MD 和 node 目录下载共用 |
| `[ctp]` | `shared/ctp.toml` | gw CTP |
| `[td].specs/last_prices` | `shared/sim.toml` | gw SimTd |
| `[instruments]`/缓存 | `shared/cache.toml` | node |
| `[serve]` | node 入口 | HTTP/WS 对外配置 |
| `[runtime]` | 入口 | gw/node 可不同线程数 |

## 5. 本地凭据

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

## 6. 兼容与迁移

旧的单文件配置继续可用：

```bash
./build-ui/tqsdk-gw -c config/gw.toml
./build-ui/tqsdk-gw -c run/live/gw.toml
```

迁移时把重复段拆到 `shared/`，入口保留 `[include]` 和进程特有项。不要同时保留
两份同名配置；入口值会覆盖 include 值，容易造成排查困难。

## 7. 错误诊断

配置错误由 app 入口打印：

```text
failed to load config '...': <detail> (<file_io|parse|invalid_value>)
```

建议先用文件/目录 `-c` 直接验证，再启动完整拓扑。

## 8. 验证

```bash
ctest --test-dir build-ui -R TqApiConfigTest --output-on-failure
```

覆盖目录入口选择、include 合并优先级、缺失 include 和 TQ broker_name 校验。

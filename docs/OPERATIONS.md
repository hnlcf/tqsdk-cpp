# 构建与运维

## 1. 依赖

- Linux（当前主要验证平台）；
- CMake ≥ 3.20；
- C++23 编译器与标准库（`CMAKE_CXX_STANDARD 23`；需要 `std::expected`，Ubuntu 24.04 使用 `g++-14/libstdc++-14-dev`；本地 Clang 23 验证）；
- Boost.Asio/Beast、OpenSSL、spdlog、fmt、nlohmann_json、magic_enum、
  tomlplusplus、CLI11、SQLiteCpp；
- SolidJS + pnpm + Node.js LTS（Web 前端）；
- CTP SDK（可选，放在 `third_party/ctp/`）。

依赖通过 vcpkg manifest 管理。CTP 目录结构：

```text
third_party/ctp/include/ThostFtdc*.h
third_party/ctp/lib/libthosttraderapi_se.so
third_party/ctp/lib/libthostmduserapi_se.so
```

## 2. 构建

```bash
cmake --preset ui-dev
cmake --build --preset ui-dev
ctest --test-dir build-ui --output-on-failure
```

CTP 构建开关和 preset 以 `CMakePresets.json` 为准。仓库通常使用：

```text
build/              默认核心构建
build-ui/           开发构建目录（保留历史名称）
build-ctp/          CTP/live 构建
build-release/      release 性能/soak
build-package/      打包
```

纯核心构建示例：

```bash
cmake -S . -B build -G Ninja \
  -DTQSDK_BUILD_TESTS=ON \
cmake --build build
```

## 3. 配置

推荐使用目录式配置，完整规则见 [CONFIG.md](CONFIG.md)：

```bash
./build-ui/tqsdk-gw   -c config
./build-ui/tqsdk-node -c config
```

`-c` 也可指向单个 TOML 文件，继续兼容旧入口。

### live 账号

```bash
cp config/live.local.env.example config/live.local.env
chmod 600 config/live.local.env
```

该文件已被 `.gitignore` 排除。至少需要：

```text
CTP_BROKER_ID
CTP_USER_ID
CTP_PASSWORD
CTP_TD_FRONT
```

可选：

```text
CTP_AUTH_CODE
CTP_APP_ID
CTP_MD_FRONT
GW_PORT
HTTP_PORT
SYMBOLS
```

### 配置目录

```text
config/
├── gw.toml
├── node.toml
└── shared/
    ├── ipc.toml
    ├── tq.toml
    ├── ctp.toml
    ├── sim.toml
    └── cache.toml
```

入口文件通过：

```toml
[include]
files = ["shared/ipc.toml", "shared/tq.toml"]
```

合并共享配置；`gw.toml` 只保留 `listen`、`md.kind`、`td.kind`、`runtime`，
`node.toml` 只保留上游连接、`serve`、`audit`、`runtime`。

网关选择：

```toml
# gw.toml
[md]
kind = "tq"   # tq / ctp / none

[td]
kind = "sim"  # sim / ctp / tq
```

切换 TQ TD 时只改 `[td].kind = "tq"`、`auto_url = true`，账户参数在
`shared/tq.toml`；切换 CTP/Sim 同理。`[include]` 的路径相对入口文件，
入口覆盖 shared；详细优先级和本地凭据见 [CONFIG.md](CONFIG.md)。

### TD-only soak

TD-only node 必须使用：

```toml
[md]
kind = "none"
```

不要写 `md.kind="remote"` 指向一个没有 MD 的 gw，否则 RemoteMdGateway 存在但
没有上游，`/readyz` 可能一直 false。

## 4. 启动

### 一键 live（Web）

```bash
./scripts/run_web.sh --build
# 打开 http://127.0.0.1:5173
```

`run_web.sh` 启动 gw/node（`run_live.sh --no-ui`）并把 Vite 代理到 node。
只启动后端：

```bash
./scripts/run_live.sh --no-ui
./scripts/run_live.sh --gw-port 14330 --http-port 18081 --no-ui
```

`./scripts/run_live.sh --build` 只启动 gw/node；Web 使用 `run_web.sh`。退出顺序由脚本处理。

### Supervisor 与健康检查

```bash
# 自动重启 gw/node 的最小 supervisor
./scripts/run_live_supervisor.sh

# 检查 healthz/readyz/trading-status；可等待 ready
./scripts/live_healthcheck.sh --wait-ready 120
./scripts/live_healthcheck.sh --require-trading
```

`run_live_supervisor.sh` 在 `run_live.sh` 退出后按 `--restart-delay` 重启；
`--max-restarts N` 限制次数。停止可 `touch run/live/supervisor.stop`。

live 期间默认把合约目录后台首次刷新延迟到 3600 秒，避免与交易/结算竞争：

```bash
INSTRUMENTS_INITIAL_DELAY_SEC=3600 INSTRUMENTS_REFRESH_INTERVAL_SEC=21600 \
  ./scripts/run_live_supervisor.sh
```

需要立即更新目录时，在独立维护窗口执行下载/校验后重启 node。

### 快期账户认证（可选）

在 `config/live.local.env` 中设置：

```bash
TQ_AUTH_ENABLED=1
TQ_AUTH_USER=your_tq_user
TQ_AUTH_PASSWORD=your_tq_password
TQ_AUTH_URL=https://auth.shinnytech.com
```

`run_live.sh` 会生成 `[tq_auth]`，node 在下载合约目录时携带
`Authorization: Bearer ...`。详见 [TQ_AUTH.md](TQ_AUTH.md)。

### 手工启动

```bash
./build-ui/tqsdk-gw -c run/live/gw.toml
./build-ui/tqsdk-node -c run/live/node.toml
```

### 验证

```bash
curl -s http://127.0.0.1:18081/healthz | jq
curl -s http://127.0.0.1:18081/readyz | jq
curl -s http://127.0.0.1:18081/quote/SHFE.rb2701 | jq
curl -s http://127.0.0.1:18081/positions | jq
```

WebSocket：

```bash
printf '%s\n' '{"op":"subscribe","symbols":["SHFE.rb2701"],"klines":[]}' |
  websocat -t ws://127.0.0.1:18081/stream
```

默认只返回 quotes/klines；需要订单时显式：

```json
"topics":["orders"]
```

## 5. 测试

```bash
ctest --test-dir build-ui --output-on-failure
ctest --test-dir build-ui -R 'Ws|DiffMerge|CTP' --output-on-failure
```

Web:

```fish
cd web
nvm use lts
pnpm install
pnpm test
pnpm typecheck
pnpm build
```

Python 自测：

```bash
python3 scripts/test_simnow_acceptance.py
```

### SimNow 验收

只读：

```bash
python3 scripts/simnow_acceptance.py \
  --node-url http://127.0.0.1:18081 \
  --symbol SHFE.ao2701 \
  --metrics
```

写测试必须显式确认：

```bash
python3 scripts/simnow_acceptance.py \
  --node-url http://127.0.0.1:18081 \
  --symbol SHFE.ao2701 \
  --orders --yes \
  --include-market --include-counterparty \
  --cleanup-market
```

详见 [SIMNOW_ACCEPTANCE.md](SIMNOW_ACCEPTANCE.md)。

## 6. 长稳/CTP 监控

```bash
python3 scripts/soak_two_process.py \
  --duration 86400 \
  --load-threads 8 \
  --gw-bin ./build-ui/tqsdk-gw \
  --node-bin ./build-ui/tqsdk-node \
  --out artifacts/soak_24h.csv
```

CTP 交易时段监控：

```bash
python3 scripts/ctp_session_soak.py \
  --manage-processes \
  --gw-config run/live/gw.toml \
  --node-config run/live/node.toml \
  --duration 21600 \
  --out artifacts/ctp_session.jsonl
```

断线/重启恢复：

```bash
./scripts/ctp_recovery_test.sh
```

脚本使用隔离端口/flow path；先阅读脚本顶部注释。平今/平昨与结算边界以
[SIMNOW_ACCEPTANCE.md](SIMNOW_ACCEPTANCE.md) 为准。

### 连续交易周（只读）

```bash
./scripts/ctp_week_preflight.sh
python3 scripts/ctp_week_soak.py --days 5          # dry-run
python3 scripts/ctp_week_soak.py --days 5 --execute
python3 scripts/ctp_daily_reconcile.py --out-dir artifacts/ctp_reconcile
```

完整验收流程见 [SIMNOW_ACCEPTANCE.md](SIMNOW_ACCEPTANCE.md)。

## 7. 产物与诊断

常见位置：

```text
artifacts/                  soak/验收报告
run/live/                   live 配置、日志、flow、market_cache.sqlite3
run/live/logs/              运行日志
/tmp/tqsdk_holidays.json    节假日缓存
```

诊断顺序：

```text
/healthz          进程是否启动
/readyz           md/td 连接、snapshot、phase
/metrics          query/queue/ws 指标
/catalog/status   catalog 是否加载
/holiday/status   holiday 缓存是否加载
node/gw logs      IPC/CTP/查询错误
```

## 8. 恢复

- **gw 重启**：node 保持进程，IPC reconnect/replay；交易 snapshot 对账。
- **node 重启**：重新连接 gw，catalog/KlineStore 恢复，WS 客户端重连。
- **CTP 结算**：预期断线，`expected_disconnect=true`；下一交易时段恢复。
- **catalog 下载失败**：保留旧文件；`/catalog/status` 查 source/loaded。
- **holiday 下载失败**：保留旧缓存；周期重试。
- **慢 WS 客户端**：发送队列到达上限会丢最旧消息，重连后靠 snapshot 恢复。

## 9. 安全

- 默认只监听 loopback；
- 当前无 REST/WS 认证；
- 不要把 node 直接暴露公网；
- `config/live.local.env`、`brokers.json`、`auth_codes.txt` 不得提交；
- 日志中避免打印密码/AuthCode；
- 生产部署需要 TLS + 身份认证、进程权限隔离和日志脱敏。

## 10. 已知限制

- CTP 仅 Linux 构建验证；
- 连续 5 个交易日真实 CTP session soak 需要按交易日运行；
- 真实部分成交依赖 SimNow/交易所撮合窗口；
- Qt UI 已删除，Web 是唯一前端；
- 交易所品种级夜盘/完整官方交易日历仍需扩展。

生产 TLS/认证/systemd/审计见 [DEPLOYMENT.md](DEPLOYMENT.md)。

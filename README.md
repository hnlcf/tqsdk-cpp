# TqSdk C++

C++23 期货数据与交易基础设施，目标是把 TqSdk 的“异步数据流 + 可替换网关”
思想落到可部署的 C++ 工程中。当前支持：

- Tq 行情、CTP 真实交易、Sim 本地撮合；
- `tqsdk-gw` / `tqsdk-node` 两进程部署；
- HTTP REST、WebSocket 实时 diff；
- `web/` SolidJS + TypeScript + pnpm 独立 Web 终端；
- TradingView Lightweight Charts K 线/分时/指标；
- 多自选列表、代码/名称模糊搜索、快捷下单（买开/卖开/平多/平空）、合并持仓、账户权益联动；
- WASM 指标插件：自动扫描 manifest、浮动指标选择器、逐输出颜色/线型/显隐、EMA/MA 动态均线；
  已内置 EMA/MA/BOLL/MACD/RSI/ATR/Donchian/KDJ/CCI/OBV/VWAP；
- K 线基础画线：水平线/趋势线、清除、按 symbol|period 持久化；
- KQ.i@ 指数 / KQ.m@ 主连；主连 1d 原始/前复权/后复权；
- EMA60 + Donchian22 + ATR 策略扫描脚本；
- 2016-2026 十年日线指数回测报告（69 个品种，含保证金/合约乘数/整数手/杠杆和图表）；
- C++ BacktestEngine 通过 C ABI + ctypes 暴露给 Python（`python/tqsdk_backtest.py`）；
- Python 实时策略适配器（node REST -> TargetPosTask -> CTP，`python/tqsdk_live.py`）；
- Python 策略库 `tqsdk_py`：策略注册表/管理器、数据适配器、Strategy 接口、SignalConsumer/企业微信/组合消费者、BacktestRunner/LiveRunner；
- 回测/实盘启动用 `--strategy <name>` 运行时选择策略；
- REST/WS token 认证、日志脱敏、JSONL 审计、systemd/Caddy/nginx 部署模板；
- 高级报单 FAK/FOK、市价/对价/五档价、TargetPosTask/InsertOrderTask、RiskManager 风控；
- Tick/DataSeries/DataDownloader、期权 ATM+Greeks、TradingStatus/动态 URL、多账户与回测；
- 合约目录、节假日日历、统一市场 SQLite 缓存（K 线 + tick）；
- CTP SimNow 验收、交易时段 soak、故障注入；
- TQ OTG 消息结构、TqTdGateway 登录/peek/重连/报单协议；
- 公共边界错误处理已迁移到 `std::expected`，项目自建异常类已删除；

## 快速开始

### 构建

```bash
cmake --preset ui-dev
cmake --build --preset ui-dev
ctest --test-dir build-ui --output-on-failure
```

C++23 由 `CMAKE_CXX_STANDARD 23` + `CMAKE_CXX_EXTENSIONS OFF` 固定；需要支持 C++23 的编译器与标准库（`std::expected`；Ubuntu 24.04 使用 `g++-14/libstdc++-14-dev`，当前验证 Clang 23 与 GCC 16/14）。

CTP 需要把 SDK 放在 `third_party/ctp/`。默认 `ui-dev` preset 会按项目当前配置
启用核心测试；CTP 构建细节见 [docs/OPERATIONS.md](docs/OPERATIONS.md)。

Web 前端：

```fish
cd web
nvm use lts
pnpm install
pnpm dev
```

### Python 策略环境

```bash
uv sync --extra dev
uv run tqsdk-strategies
uv run python -m unittest discover -s python/tests -v
```

依赖取舍、uv lock、wheel 与 native C++ 库的打包边界见
[docs/PYTHON_PACKAGING.md](docs/PYTHON_PACKAGING.md)。

### 启动 live

```bash
cp config/live.local.env.example config/live.local.env
chmod 600 config/live.local.env
$EDITOR config/live.local.env

./scripts/run_web.sh --build
```

`run_web.sh` 启动 gw/node + Vite；打开 `http://127.0.0.1:5173`。
只启动后端使用 `./scripts/run_live.sh`；需要自动重启用 `./scripts/run_live_supervisor.sh`。
交易前健康检查：`./scripts/live_healthcheck.sh --wait-ready 120`。

手工启动也可以使用目录式配置（每个 IPC 组一份文件，TQ/CTP/缓存共享）：

```bash
./build-ui/tqsdk-gw -c config
./build-ui/tqsdk-node -c config
```

配置布局和 include 规则见 [docs/CONFIG.md](docs/CONFIG.md)。

只启动 gw/node：

```bash
./scripts/run_live.sh --no-ui
```

默认 node：

```text
http://127.0.0.1:18081
ws://127.0.0.1:18081/stream
```

### 测试

```bash
ctest --test-dir build-ui --output-on-failure
python3 scripts/test_simnow_acceptance.py
./scripts/ci_local.sh
```

真实 SimNow 写测试必须显式确认，见
[docs/SIMNOW_ACCEPTANCE.md](docs/SIMNOW_ACCEPTANCE.md)；
脚本入口见 [scripts/README.md](scripts/README.md)。

## 进程拓扑

```text
Tq 行情  /  CTP TD  /  Sim TD
              │
              ▼
       tqsdk-gw（接入进程）
       ├─ 真实行情/交易网关
       ├─ IPC server + seq/replay
       └─ CTP SPI -> Asio 桥接
              │
       IPC（UDS/TCP，帧 + seq + resume）
              │
              ▼
      tqsdk-node（数据与 API 进程）
       ├─ RemoteMd / RemoteTd
       ├─ TqApi + DataManager + DiffMerge
       ├─ InstrumentCatalog / KlineStore
       └─ HTTP / WebSocket
              │
       REST + WS
              │
              ▼
        TQSDK Board（Web）或外部策略
       （前端不链接 tqsdk_core）
```

## 代码结构

```text
include/tqsdk/
  channel/        Asio 异步 Channel
  domain/         Quote/Order/Trade/Position/Account/Kline
  data/           DataManager、DiffMerge、KlineStore
  gateway/        MarketDataGateway / TradingGateway 及 Tq/CTP/Sim/Mock
  ipc/            IPC frame、session/client/server、Remote 网关
  serve/          Router、HTTP、WebSocket、快照投影
  trading/        TradingCalendar、HolidayCalendar、订单归一化
  instrument/     合约目录
  config/         TOML/环境配置
src/              对应实现
apps/             gw_main、node_main
web/              TQSDK Board：SolidJS + pnpm + Lightweight Charts
tests/            GTest / e2e / CTP 映射
scripts/          live、soak、SimNow 验收、诊断
docs/             架构、模块、信息流、状态机、运维、专题
```

## 文档入口

先读 canonical 文档：

- [docs/README.md](docs/README.md) — 精简索引与阅读顺序
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — 进程拓扑、分层、线程和数据所有权
- [docs/MODULES.md](docs/MODULES.md) — 模块职责和文件边界
- [docs/FLOWS.md](docs/FLOWS.md) — 行情/交易/订阅/重连信息流
- [docs/STATE_MACHINES.md](docs/STATE_MACHINES.md) — 进程、订单、连接状态机
- [docs/PROTOCOLS.md](docs/PROTOCOLS.md) — IPC/REST/WS 协议
- [docs/CONFIG.md](docs/CONFIG.md) — 配置目录、IPC 组、include 和共享配置
- [docs/OPERATIONS.md](docs/OPERATIONS.md) — 构建、运行、验收、诊断

常用专题：

- [docs/CTP.md](docs/CTP.md) — CTP 接入和报单映射
- [docs/SIMNOW_ACCEPTANCE.md](docs/SIMNOW_ACCEPTANCE.md) — 单账户 CTP/SimNow 验收
- [docs/SIMNOW_DUAL_ACCOUNT.md](docs/SIMNOW_DUAL_ACCOUNT.md) — 双 SimNow 账户验收
- [docs/ERROR_HANDLING.md](docs/ERROR_HANDLING.md) — `std::expected` 和错误边界
- [docs/TQ_PROTOCOL.md](docs/TQ_PROTOCOL.md) — TQ OTG 消息与 TqTdGateway
- [docs/TQ_AUTH.md](docs/TQ_AUTH.md) — 快期账户认证和动态 URL
- [docs/WEB_FRONTEND.md](docs/WEB_FRONTEND.md) / [docs/WEB_CUSTOM_INDICATORS.md](docs/WEB_CUSTOM_INDICATORS.md) — Web 前端和 WASM 指标
- [docs/ROADMAP.md](docs/ROADMAP.md) — 当前计划

数据/策略/回测和 Python 文档在 [docs/README.md](docs/README.md) 中按主题列出。
不再维护按日期堆积的 campaign 文档；一次性结果写入 `artifacts/`。

## 当前状态

- 最新 Release：`v0.4.0`（错误处理重构、C++23 GCC 14 构建、12 个 assets）；
- `ctest`：387/387（2 项 TqMdNetwork 网络测试按环境跳过）；
- Python：21/21；Web typecheck/test/build 和 Playwright E2E 通过；
- 本地 Clang 23 与 GCC 16.2.1 构建通过；
- 错误处理：公共 API 使用 expected/noexcept，详见 [docs/ERROR_HANDLING.md](docs/ERROR_HANDLING.md)；
- 多账户：REST/WS/task/risk 支持 `?account=`；双 Sim 进程级验收通过；
- TQ：全部 aid 的结构化消息、TqMdGateway 迁移、TqTdGateway 登录/peek/重连/报单协议已完成；
  真实 TQ TD 账户登录后的持仓/报单验收仍待交易时段执行；
- SimTd 两进程测试和短 soak 通过；CTP 连续交易周/7 天 soak 仍在运行，
  完成前不宣称连续周验证通过；
- 性能基线见 [docs/PERFORMANCE_BASELINE.md](docs/PERFORMANCE_BASELINE.md)；
- 打包：`./scripts/package_release.sh` 生成 DEB/RPM/TGZ，详见 [docs/PACKAGING.md](docs/PACKAGING.md)。

## 安全

仓库不应提交以下内容：

```text
config/live.local.env
brokers.json
auth_codes.txt
symbol.json / latest.json
kline_cache.sqlite3
run/live/
artifacts/
```

CTP 账号、AuthCode、密码只通过本机配置或环境变量提供。

##  License / 致谢

本项目是 TqSdk 设计思想在 C++/Boost.Asio 上的独立实现，不复刻 Python
运行时。CTP 头文件和动态库由使用者自行按柜台 SDK 许可放置。

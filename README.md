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
python3 scripts/p0_simnow_acceptance.py  # P0 高级报单/任务/风控探针
```

真实 SimNow 写测试必须显式确认，见
[docs/SIMNOW_ACCEPTANCE.md](docs/SIMNOW_ACCEPTANCE.md)。

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

先读：

- [docs/README.md](docs/README.md) — 文档索引与阅读顺序
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — 总体架构、线程与数据所有权
- [docs/MODULES.md](docs/MODULES.md) — 模块实现和文件边界
- [docs/FLOWS.md](docs/FLOWS.md) — 行情/交易/订阅/重连信息流
- [docs/STATE_MACHINES.md](docs/STATE_MACHINES.md) — 进程、订单、连接、交易日状态机
- [docs/PROTOCOLS.md](docs/PROTOCOLS.md) — IPC/REST/WS 协议
- [docs/ERROR_HANDLING.md](docs/ERROR_HANDLING.md) — std::expected、模块错误与异常隔离
- [docs/OPERATIONS.md](docs/OPERATIONS.md) — 构建、配置、运行、验收、诊断
- [docs/ROADMAP.md](docs/ROADMAP.md) — 后续计划
- [docs/PERFORMANCE_BASELINE.md](docs/PERFORMANCE_BASELINE.md) — WASM/HTTP/WS 性能基线
- [docs/REFACTOR_ASSESSMENT.md](docs/REFACTOR_ASSESSMENT.md) — 前后端重构评估

专题：

- [docs/CTP.md](docs/CTP.md)
- [docs/WEB_FRONTEND.md](docs/WEB_FRONTEND.md)
- [docs/WEB_INDICATORS.md](docs/WEB_INDICATORS.md)
- [docs/TQ_AUTH.md](docs/TQ_AUTH.md)
- [web/README.md](web/README.md)
- [docs/archive/UI_QT_LEGACY.md](docs/archive/UI_QT_LEGACY.md)（历史）
- [docs/archive/INDICATOR_PLUGINS_QT_LEGACY.md](docs/archive/INDICATOR_PLUGINS_QT_LEGACY.md)（历史）
- [docs/INSTRUMENTS.md](docs/INSTRUMENTS.md)
- [docs/KLINE_CACHE.md](docs/KLINE_CACHE.md)
- [docs/SIMNOW_ACCEPTANCE.md](docs/SIMNOW_ACCEPTANCE.md)
- [docs/P0_SIMNOW_TEST_PLAN.md](docs/P0_SIMNOW_TEST_PLAN.md)
- [docs/ORDER_TASKS.md](docs/ORDER_TASKS.md)
- [docs/RISK_CONTROL.md](docs/RISK_CONTROL.md)
- [docs/TICK_DATA.md](docs/TICK_DATA.md)
- [docs/OPTION_ANALYTICS.md](docs/OPTION_ANALYTICS.md)
- [docs/TRADING_STATUS.md](docs/TRADING_STATUS.md)
- [docs/MULTI_ACCOUNT_ACCEPTANCE.md](docs/MULTI_ACCOUNT_ACCEPTANCE.md)
- [docs/SOAK_AUTOMATION.md](docs/SOAK_AUTOMATION.md)
- [docs/MULTI_ACCOUNT_BACKTEST.md](docs/MULTI_ACCOUNT_BACKTEST.md)
- [docs/SOAK_PERF.md](docs/SOAK_PERF.md)
- [docs/DEPLOYMENT.md](docs/DEPLOYMENT.md)
- [docs/PACKAGING.md](docs/PACKAGING.md)
- [docs/CONTAINERS.md](docs/CONTAINERS.md)
- [docs/CONTINUOUS_CONTRACTS.md](docs/CONTINUOUS_CONTRACTS.md)
- [docs/STRATEGY_EMA60_DONCHIAN.md](docs/STRATEGY_EMA60_DONCHIAN.md)
- [docs/STRATEGY_SCAN_20261111.md](docs/STRATEGY_SCAN_20261111.md)
- [docs/DAILY_STRATEGY_BACKTEST.md](docs/DAILY_STRATEGY_BACKTEST.md)
- [docs/DAILY_BACKTEST_DATA.md](docs/DAILY_BACKTEST_DATA.md)
- [docs/PYTHON_BACKTEST.md](docs/PYTHON_BACKTEST.md)
- [docs/PYTHON_LIVE_TRADING.md](docs/PYTHON_LIVE_TRADING.md)
- [docs/PYTHON_LIBRARY.md](docs/PYTHON_LIBRARY.md)
- [docs/PYTHON_PACKAGING.md](docs/PYTHON_PACKAGING.md)
- [docs/RELEASE_0.1.0.md](docs/RELEASE_0.1.0.md)
- [docs/CTP_SINGLE_ACCOUNT_STRATEGY.md](docs/CTP_SINGLE_ACCOUNT_STRATEGY.md)
- [docs/CTP_TRADE_MATRIX.md](docs/CTP_TRADE_MATRIX.md)
- [docs/CTP_WEEKLY_TEST_PLAN.md](docs/CTP_WEEKLY_TEST_PLAN.md)
- [docs/P6_AUTOMATION.md](docs/P6_AUTOMATION.md)

旧版设计/评审文档已归档到 [docs/archive/](docs/archive/)，只用于历史追溯；
新实现以本 README 和上述 canonical docs 为准。

## 当前状态

- `./scripts/package_release.sh`：生成 DEB/RPM/TGZ；详见 docs/PACKAGING.md；
- 最新 Release：`v0.4.0`（错误处理重构、C++23 GCC 14 构建、12 个 assets）；
- `ctest`：378 项通过（2 项网络测试按环境跳过；以本地构建目录为准）；
- Web：`pnpm typecheck`、`pnpm test`（28 项）、`pnpm build`、Playwright E2E 通过；
- 日线回测 helper：`python3 scripts/test_backtest_daily.py`（4/4）；
- Python 策略/绑定：`python3 -m unittest discover -s python/tests`（21/21）；
  K 线/分时/WASM 指标/盘口/三键下单/合并持仓/委托/成交/权益已完成浏览器验证；
- 多账户：REST/WS/task/risk 均支持 `?account=`，双 Sim 账户集成测试通过；
- 指标配置/启用状态/输出样式/动态均线和 panel 尺寸浏览器本地持久化；
- 合约目录：支持 TqAuth Bearer 下载；本地全量目录加载 29,439 条已验证；
- SimTd 24h soak：完成，`http_err=0`、`ws_err=0`、`fatal=""`；
- P7.5 基线：WASM 7 插件约 0.48ms/5000 根，HTTP/WS 数据见
  [docs/PERFORMANCE_BASELINE.md](docs/PERFORMANCE_BASELINE.md)；
- SimNow CTP 写验收：15/15 PASS；
- 平昨自动化：已完成；
- 错误处理：公共 API 已使用 expected/noexcept，详见 docs/ERROR_HANDLING.md；
- CTP session monitor：支持结算预期断线；
- Windows CTP、连续 5 个交易日 CTP soak 仍待后续验证；
- 已准备 `ctp_week_soak.py` / `ctp_daily_reconcile.py` / `ctp_week_preflight.sh`。

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

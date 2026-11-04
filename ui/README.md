# TQSDK Qt UI

独立的 Qt 6 Widgets 桌面前端。它只通过 node 的 REST + WebSocket 获取行情/
交易数据，**不链接 `tqsdk_core`**。

内部架构、组件与数据流见 [`../docs/UI_ARCHITECTURE.md`](../docs/UI_ARCHITECTURE.md)；
内部架构已收敛为 WorkspaceLayout + SymbolController + NodeDataController +
TradingController；完整设计见 [`../docs/UI_QT.md`](../docs/UI_QT.md)；指标插件设计见
[`../docs/INDICATOR_PLUGINS.md`](../docs/INDICATOR_PLUGINS.md)。

## 依赖

- Qt 6.5+
- Qt6 Core / Gui / Widgets / Network / WebSockets
- Linux 下通常还需要 X11/Wayland、字体和 OpenGL 运行库

vcpkg 至少需要：

```json
{
  "name": "qtbase",
  "features": ["wayland"]
},
"qtwayland",
"qtwebsockets",
```

`qtwayland` 提供 Qt Wayland QPA/client 模块；`qtbase[wayland]` 是它的构建前提。
XCB 仍由 `qtbase` 默认 feature 保留，因此可以在 Wayland 和 XWayland 之间切换。
如果 `"qt"` metaport 已包含所需模块，也可以只使用 metaport；配置阶段会明确
报告缺少的 Qt 组件。

## 构建

UI 默认关闭，避免影响后端核心构建。先安装 Qt，再用 UI preset：

```bash
export VCPKG_ROOT=/path/to/vcpkg
cmake --preset ui-dev
cmake --build --preset ui-dev
ctest --test-dir build-ui --output-on-failure
# 只测指标：ctest --test-dir build-ui -R 'tqsdk-ui-(indicator|chart|window)' --output-on-failure
./build-ui/bin/tqsdk-ui
```

或者手动：

```bash
cmake -S . -B build-ui -DTQSDK_BUILD_UI=ON
cmake --build build-ui --target tqsdk-ui -j
```

## 指标插件（动态加载）

每个指标只有一个 Python 文件，元数据和计算放在一起：

```text
~/.config/tqsdk/indicators/rsi.py
# 或 ~/.config/tqsdk/indicators/rsi/indicator.py
```

```python
MANIFEST = {
    "id": "rsi", "name": "RSI", "panel": "sub",
    "params": [{"key": "period", "default": 14, "min": 2, "max": 500}],
    "series": [{"name": "rsi", "color": "#f0c75e"}],
}

def compute(params, bars):
    ...
    return {"rsi": values}
```

也可以使用 `def manifest(): return {...}` 接口。把文件放入目录后，在
“指标”菜单点“刷新指标列表”即可，不需要重新编译 UI。

## 当前阶段

已在 Qt 6.11.2 + QtWebSockets + Wayland 上验证 configure/build 和运行时连接。

Phase 1（已完成）：

- 自选/最近行情表，绑定 node WS quote snapshot/diff；
- 期货合约列表，绑定 REST `/catalog/instruments`，支持模糊搜索、回车加入自选和双击切换；
- REST quote 快照 + WS quote 推送，无推送时也先显示已有截面/占位；
- QPainter 自绘 K 线：蜡烛、成交量、价格/时间坐标、当前价、十字光标；支持滚轮缩放和左键拖动；头部显示最新价/涨跌/涨幅；
- 1m/5m/15m/30m/1h/2h/4h/1d/3d/1w 周期切换，K 线 REST 历史 + WS diff；
- 深色/浅色主题；
- Wayland 原生运行，保留 XCB/XWayland fallback；
- QtTest 覆盖协议解析、MarketStore 合并和 table models。

Phase 3（P3 Phase B 已完成）：
- Python 指标 sidecar，独立进程运行，长度前缀 JSON 协议；
- 内置 MA/EMA/BOLL/MACD；
- 主图叠加和 MACD 副图；
- 指标菜单、参数编辑、启停和 QSettings 持久化；
- 单文件 Python 插件动态发现，新增指标无需重新编译；
- 指标菜单“刷新指标列表”可在运行时加载新插件；
- sidecar 超时、输出上限、崩溃重启和敏感环境变量隔离；
- `tqsdk-ui-indicator` 测试覆盖协议、计算、增量更新和 UI 端到端。

Phase 2（已完成基础版）：

- 期权链 CALL/PUT 表格，双击合约加入自选；
- 限价单下单、撤单，三键直接提交；
- 账户、持仓、委托、成交、持仓明细均为独立 panel；REST 快照 + WS diff；
- 行情快照、五档订单簿、分笔成交为三个独立 panel；
- 交易面板在 node ready + 账户加载后才可提交；三键下单是独立 dock。

启动时指定 node：

```bash
./build-ui/bin/tqsdk-ui --node-url http://127.0.0.1:18081
# 指标 sidecar（正常由 IndicatorManager 自动启动）
python3 ui/indicator_sidecar/indicator_sidecar.py
```

## Tq + CTP 一键启动

复制并填写 CTP 本地配置：

```bash
cp config/live.local.env.example config/live.local.env
chmod 600 config/live.local.env
$EDITOR config/live.local.env
```

然后：

```bash
./scripts/run_live.sh --build      # 构建并启动 gw/node/ui
./scripts/run_live.sh --no-ui      # 只启动 gw/node
```

脚本生成 `run/live/gw.toml`、`run/live/node.toml`，日志在
`run/live/logs/`。默认 Tq md + CTP td，退出时统一清理三个进程。

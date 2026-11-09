# K 线指标插件设计

> 专题文档。总体架构/模块/信息流/状态机以 [ARCHITECTURE.md](ARCHITECTURE.md)、
> [MODULES.md](MODULES.md)、[FLOWS.md](FLOWS.md)、[STATE_MACHINES.md](STATE_MACHINES.md)
> 和 [PROTOCOLS.md](PROTOCOLS.md) 为准。

> 状态：Phase B（Python sidecar、单文件指标插件、动态菜单、内置 MA/EMA/BOLL/MACD）已实现；插件签名/依赖管理和 DSL 仍待 Phase C。
>
> 目标不是把指标公式塞进 `paintEvent`，而是让指标计算、渲染和生命周期与
> K 线数据、图表控件解耦，后续可以支持 Python 脚本、内置 DSL 和第三方插件包。

## 1. 约束

- `tqsdk-ui` 不链接 `tqsdk_core`，指标不能绕过 node 数据边界；
- GUI 主线程不能执行用户脚本；
- 指标不能直接持有 `KLineChartWidget`、`MarketStore` 或网络对象；
- 指标输入只允许 OHLCV/持仓量等行情字段，不允许访问文件、网络和交易接口；
- 指标崩溃、死循环、异常输出不能拖垮主 UI；
- 图表可以同时显示主图叠加指标和副图指标；
- 指标结果必须可缓存、可重算、可热切换。

## 2. 方案比较

### 方案 A：纯 C++ 内置指标

优点：性能最好、无额外运行时、调试简单。

缺点：新指标必须重新编译 UI，无法让用户自行扩展，不适合作为长期插件体系。

### 方案 B：DSL（声明式指标）

示例：

```yaml
id: ma_cross
name: 均线交叉
version: 1
inputs: [close]
params:
  fast: {type: int, default: 5, min: 1}
  slow: {type: int, default: 20, min: 1}
plot: overlay
series:
  ma_fast: "sma(close, fast)"
  ma_slow: "sma(close, slow)"
  cross: "crossover(ma_fast, ma_slow)"
```

优点：

- 无 Python/GIL 依赖；
- 可静态校验参数、输入和表达式；
- 适合 80% 常见指标；
- 容易在 C++ 中做向量化缓存。

缺点：复杂条件、循环、状态机和多品种逻辑表达力有限。

### 方案 C：Python 插件子进程

指标作者写：

```python
def on_init(ctx):
    ctx.param("fast", 5)
    ctx.param("slow", 20)

def on_bar(ctx, bar):
    fast = ctx.sma("close", ctx.param("fast"))
    slow = ctx.sma("close", ctx.param("slow"))
    ctx.plot("ma_fast", fast)
    ctx.plot("ma_slow", slow)
```

优点：表达力强、生态成熟、用户门槛低。

缺点：Python 运行时、依赖管理、GIL、崩溃隔离、性能和部署复杂度。

### 推荐路线

采用 **DSL 优先 + Python 进程插件** 的分层方案：

```text
第一阶段：C++ 求值器 + 安全 DSL（内置/用户配置）
第二阶段：Python sidecar 进程（独立可执行文件）
第三阶段：插件市场/目录、版本约束和热更新
```

不建议把 CPython 嵌入 Qt UI 进程。Python 解释器和第三方包会带来 ABI、
动态库、信号处理、崩溃和部署问题，和当前“UI 只依赖 Qt”的边界冲突。

## 3. 总体架构

```text
┌─────────────────────────────────────────────────────────────┐
│ Qt UI 主进程                                                │
│                                                             │
│  MarketStore ──bars/quotes──► IndicatorManager              │
│       ▲                           │                         │
│       │                           ├─ DSL evaluator (C++)     │
│       │                           │                         │
│       │                           └─ JSON-lines / UDS ──────┼──► Python sidecar
│       │                                                       │
│  KLineChartWidget ◄── IndicatorSeries (numeric only)          │
│       ▲                                                       │
│       └─ 主图 overlay / 副图 panel / 图例 / 参数编辑            │
└─────────────────────────────────────────────────────────────┘
```

关键边界：

- `IndicatorManager` 只知道 `KLineBar` 和指标请求；
- `KLineChartWidget` 只接收已经算好的数值序列，不执行表达式/脚本；
- Python sidecar 只看到行情数据和指标元数据，不接触 node/账户/交易；
- UI 主线程只做消息编解码和重绘，不做指标计算。

## 4. 指标描述模型

```cpp
struct IndicatorSpec {
    QString id;                 // 全局唯一
    QString name;
    QString version;
    QString kind;               // dsl | python
    QString source;             // DSL 文本或 Python 文件
    QString panel;              // main | sub
    QStringList inputs;         // close/open/high/low/volume/open_interest
    QJsonObject paramsSchema;
    QJsonObject defaults;
};
```

渲染输出：

```cpp
struct IndicatorPoint {
    qint64 datetime;       // 与 K 线 bar 对齐
    QVector<double> values;
};

struct IndicatorSeries {
    QString indicatorId;
    QString panel;         // main | sub
    QStringList seriesNames;
    QVector<IndicatorPoint> points;
    QString error;
};
```

## 5. 通信协议

建议使用 **长度前缀 JSON lines**，先支持本地 stdio，后续可切换 UDS：

```text
[4-byte big-endian length][UTF-8 JSON]\n
```

请求：

```json
{"type":"init","indicator_id":"ma_cross","version":1,
 "params":{"fast":5,"slow":20},
 "fields":["close"]}
```

```json
{"type":"bars","indicator_id":"ma_cross","reset":true,
 "bars":[{"datetime":1793689140000000000,"open":2980,"high":2995,
           "low":2975,"close":2991,"volume":10,"open_oi":20,"close_oi":21}]}
```

```json
{"type":"update","indicator_id":"ma_cross",
 "bars":[{"datetime":1793689200000000000,"open":2991,"high":2998,
           "low":2988,"close":2996,"volume":12,"open_oi":21,"close_oi":22}]}
```

响应：

```json
{"type":"series","indicator_id":"ma_cross",
 "series":[{"name":"ma_fast","points":[1793689140000000000,5.1]},
            {"name":"ma_slow","points":[1793689140000000000,4.8]}]}
```

错误：

```json
{"type":"error","indicator_id":"ma_cross","code":"timeout","message":"..."}
```

协议要求：

- 所有 `datetime` 使用与 K 线相同的 ns 时间戳；
- `reset=true` 表示重算整个可见历史；
- 增量更新只发送最后一根 bar；
- sidecar 输出点数不能超过请求窗口；
- 非法数值（NaN/Inf）在 UI 边界统一转为空值，不能进入 QPainter；
- 单次响应大小上限（例如 4MB）和超时（例如 2s）必须可配置。

## 6. DSL 设计

DSL 采用 YAML/JSON manifest，表达式限制在允许函数集合内：

```text
sma(series, window)
ema(series, window)
std(series, window)
highest(series, window)
lowest(series, window)
crossover(a, b)
crossunder(a, b)
ref(series, offset)
abs/exp/log/min/max
```

安全规则：

- 禁止赋值、循环、任意函数调用和 I/O；
- 只允许 manifest 声明的输入字段；
- 参数必须按 schema 校验类型、范围和默认值；
- 计算复杂度受最大窗口和最大 bar 数限制；
- 解释器只返回数值序列，不执行字符串拼接/QProcess/文件访问。

第一阶段可以先内置以下指标，作为 DSL 的回归样本：

- MA / EMA；
- MACD；
- KDJ；
- RSI；
- BOLL；
- VOL；
- 持仓量变化。

## 7. Python sidecar

### 7.1 生命周期

```text
UI 启动
  -> 扫描插件目录的 .py 元数据
  -> 用户启用指标时按需启动 sidecar
  -> init 指标
  -> reset bars
  -> update last bar（合并 30~60Hz）
  -> UI 关闭/指标禁用时 stop
```

### 7.2 隔离

- sidecar 是独立进程，stdout/stderr 与 UI 日志分离；
- 每个指标计算设置超时和输出大小上限；
- 连续失败禁用该指标并显示错误，不弹阻塞对话框；
- sidecar 崩溃后标记 `error`，可手动重载；
- Python 子进程默认工作目录是插件目录，禁止继承 node 凭据；
- 不在 sidecar 中提供交易/账户 API。

### 7.3 插件目录

每个指标只有一个 Python 文件；不再要求独立的 `manifest.json`：

```text
~/.config/tqsdk/indicators/
  rsi.py
  my_indicator/
    indicator.py
```

项目内自带文件：

```text
ui/indicator_sidecar/plugins/
  ma/indicator.py
  ema/indicator.py
  boll/indicator.py
  macd/indicator.py
```

`requirements.txt` 自动安装属于 Phase C，当前插件只应使用 Python 标准库。

### 7.4 Python API（已实现）

```python
MANIFEST = {
    "id": "ma_cross",
    "name": "均线交叉",
    "panel": "main",
    "params": [
        {"key": "fast", "label": "快线", "type": "int", "default": 5},
        {"key": "slow", "label": "慢线", "type": "int", "default": 20},
    ],
    "series": [
        {"name": "ma_fast", "label": "MA快", "color": "#f5c542"},
        {"name": "ma_slow", "label": "MA慢", "color": "#4f8cff"},
    ],
}


def compute(params, bars):
    # bars: [{"datetime", "open", "high", "low", "close", "volume", ...}]
    # 返回的每个 values 必须与 bars 等长
    return {"ma_fast": [...], "ma_slow": [...]}
```

也支持 `def manifest(): return {...}`。sidecar 只把元数据和计算函数暴露给
Qt，不提供网络、文件、subprocess、交易或 Qt 对象 API；插件代码本身仍在
独立进程中运行。

## 8. 图表集成

### 8.1 主图叠加

- MA/BOLL 等 `panel="main"` 序列直接使用 K 线价格坐标；
- 图例显示指标名、参数、当前值；
- 每条序列有独立颜色、线宽和可见性开关。

### 8.2 副图

- MACD/KDJ/RSI 等 `panel="sub"` 序列进入副图；
- 副图与主图共享时间轴和十字光标；
- 副图高度由 `QSplitter` 或固定比例控制；
- `KLineChartWidget` 只负责绘制，不负责计算。

### 8.3 数据更新

- 指标以 `(symbol, duration_ns, indicator_id)` 为缓存键；
- 切换周期/合约时取消旧计算；
- 同一帧多次 quote 更新合并为一次指标更新；
- 视口缩放/平移只影响绘制区间，不触发全量指标重算；
- 只有新增 bar 或显式 `reset` 才触发增量/全量重算。

## 9. UI 管理

建议增加一个独立的“指标”面板/抽屉：

- 可用指标列表；
- 启用/禁用；
- 参数编辑；
- 主图/副图选择；
- 颜色和线宽；
- 重载/查看错误；
- 删除用户插件（需要确认）。

指标配置写入 `QSettings`，但 Python 插件文件和依赖不写入 QSettings。

## 10. 分阶段计划

### Phase A：DSL 内核

- `IndicatorSpec` / `IndicatorManager`；
- YAML/JSON DSL manifest 校验；
- MA/EMA/MACD/KDJ/RSI/BOLL/VOL；
- 主图叠加和副图；
- 单元测试（向量结果、NaN、重置、窗口边界）。

### Phase B：Python sidecar

- `tqsdk-indicator-host` 独立可执行文件；
- 长度前缀 JSON 协议；
- 超时、输出上限、崩溃恢复；
- Python API 和示例插件；
- UI 启用/禁用/错误状态。

### Phase C：工程化

- 插件签名/目录扫描；
- 版本兼容和依赖提示；
- 性能基准（10 年 1m bars、100 个指标）；
- 打包（sidecar 随 UI 包分发，Python 可选）；
- 指标导出/导入。

## 11. 待定问题

1. DSL 使用 YAML 还是 JSON（建议 YAML，最终仍可转 JSON schema）；
2. Python sidecar 是否随 UI 包分发，还是要求系统 Python；
3. 指标是否允许访问多合约数据（建议 V1 不允许）；
4. 用户插件目录的权限/签名策略；
5. 副图数量上限和布局保存规则。

当前路线备注：用户倾向后续直接从 **Python sidecar（第二阶段）** 开始，因此
Phase A 的 DSL 可以只保留为轻量内置指标能力，不阻塞 Python 插件。
无论先做哪一层，主图/副图渲染抽象和数据协议都应先稳定下来。

## 12. P3 Phase B 实现记录（2026-11-04）

状态：**Phase B 已实现**，内置指标已经通过 Qt UI → Manager → Python sidecar
的完整链路运行；第三方插件目录、签名和依赖安装留到 Phase C。

### 12.1 代码位置

```text
ui/src/indicator/IndicatorTypes.h           指标 spec/series 数据结构
ui/src/indicator/IndicatorManager.{h,cpp}  QProcess 生命周期、协议、超时、重启
ui/src/indicator/IndicatorSettingsDialog.* 数值参数编辑
ui/indicator_sidecar/indicator_sidecar.py  独立 Python sidecar
ui/tests/IndicatorManagerTest.cpp          协议/计算/异常测试
ui/tests/MainWindowTest.cpp                UI 端到端指标测试
```

### 12.2 当前内置指标

| id | 面板 | 参数 | 输出 |
|---|---|---|---|
| `ma` | main | fast、slow | `ma_fast`、`ma_slow` |
| `ema` | main | fast、slow | `ema_fast`、`ema_slow` |
| `boll` | main | period、mult | mid、upper、lower |
| `macd` | sub | fast、slow、signal | macd、signal、histogram |

`IndicatorManager` 只接收数值序列；图表不会执行 Python、不会持有 sidecar 对象。
图表主图用价格坐标绘制 MA/EMA/BOLL，MACD 在副图独立缩放绘制。

### 12.3 协议

实际实现使用长度前缀 JSON，而不是换行分隔：

```text
[4-byte big-endian length][UTF-8 compact JSON]
```

请求：

```json
{"type":"init","indicator_id":"ma","params":{"fast":5,"slow":20}}
{"type":"bars","indicator_id":"ma","reset":true,"bars":[{"datetime":...,"close":...}]}
```

响应：

```json
{"type":"ready","protocol":2,"indicators":[
  {"id":"ma","name":"MA","panel":"main",
   "params":[{"key":"fast","label":"快线","default":5}],
   "series":[{"name":"ma_fast","label":"MA快","color":"#f0c75e"}]}
]}

{"type":"series","indicator_id":"ma","series":[{"name":"ma_fast",
  "panel":"main","points":[{"datetime":...,"value":...}]}]}
```

错误：

```json
{"type":"error","indicator_id":"ma","code":"indicator_error","message":"..."}
```

实现约束：

- 只接受 `init`、`bars`、`stop`；
- 每帧上限 16MB，UI 接收上限 8MB；
- 输出只允许有限数值，NaN/Inf 被过滤；
- 未知指标、未初始化指标、非法帧返回 error，不退出整个 sidecar；
- 首帧 `ready` 后 UI 才发送 init/bars。

### 12.4 Qt Manager 生命周期

1. sidecar 扫描插件 `.py`，在首帧 `ready.indicators` 返回完整 catalog；
2. Qt 根据 catalog 构建动态菜单；`IndicatorManager` 为启用指标发送 `init`；
3. K 线 reset 时发送完整窗口，后续相同合约/周期只发送最后一根 bar 或新增 bar；
4. 结果按 `indicator_id` 聚合，按启用顺序发给图表；
5. 进程退出后标记错误，1 秒后自动重启；
6. 响应 3 秒超时、输出超限、启动失败都会返回错误信号；
7. 进程环境会移除 `CTP_*`、密码和 AuthCode 变量，工作目录切到 sidecar 目录。

### 12.5 UI 使用

菜单栏新增“指标”：

- 子菜单中勾选启用 MA/EMA/BOLL/MACD；
- “参数...”打开数值参数编辑框；
- 启用/参数保存到：

```text
ui/indicators/enabled
ui/indicators/<id>/params   # compact JSON bytes
```

启用指标后，K 线数据继续只从 node 获取；指标不可用时图表保持原样，错误显示在
状态栏和顶部 Toast，不会阻塞交易或自选操作。

### 12.6 运行和测试

```bash
python3 ui/indicator_sidecar/indicator_sidecar.py   # 手工协议调试
cmake --build build-ui --target tqsdk-ui-indicator-tests -j
ctest --test-dir build-ui -R 'tqsdk-ui-(indicator|chart|window)' --output-on-failure
```

`scripts/run_live.sh` 会导出：

```text
TQSDK_INDICATOR_SIDECAR=<repo>/ui/indicator_sidecar/indicator_sidecar.py
```

也可以手动设置该环境变量或在 CMake install 后使用随包 `share/tqsdk/indicator_sidecar`。

### 12.7 当前边界

- 单文件插件目录扫描和动态菜单刷新已实现；插件签名、权限沙箱和依赖安装尚未实现；
- Python 依赖只使用标准库，暂不支持执行 `requirements.txt` 安装；
- 副图当前固定一个 MACD 区域，后续可扩展多副图和十字光标联动；
- 指标有独立覆盖层缓存，副图出现时会缩小主图和成交量区域；
- 多指标同时启用时 sidecar 按请求顺序计算；大窗口/多指标基准测试留在 Phase C。
- Python 插件由 sidecar 直接 import，仍未做文件/网络/进程访问限制。

### 12.8 动态添加指标（无需重新编译）

每个指标只需要 **一个 Python 文件**。元数据和计算放在同一个文件里：

```python
# ~/.config/tqsdk/indicators/rsi.py
MANIFEST = {
    "id": "rsi",
    "name": "RSI",
    "version": "1.0",
    "panel": "sub",
    "params": [
        {"key": "period", "label": "周期", "type": "int",
         "default": 14, "min": 2, "max": 500}
    ],
    "series": [
        {"name": "rsi", "label": "RSI", "color": "#f0c75e"}
    ]
}


def compute(params, bars):
    period = max(1, int(params.get("period", 14)))
    closes = [float(bar["close"]) for bar in bars]
    values = []
    for i in range(len(closes)):
        window = closes[max(0, i - period + 1):i + 1]
        values.append(sum(window) / len(window))
    return {"rsi": values}
```

也可以把元数据定义成接口，适合动态生成参数：

```python
def manifest():
    return {
        "id": "rsi",
        "name": "RSI",
        "panel": "sub",
        ...
    }

def compute(params, bars):
    ...
```

sidecar 支持：

- `MANIFEST` 字典；
- `MANIFEST` 可调用对象；
- `manifest()` 函数；
- `manifest` 字典。

### 目录布局

支持两种形式：

```text
~/.config/tqsdk/indicators/rsi.py
# 或
~/.config/tqsdk/indicators/rsi/indicator.py
# 或
~/.config/tqsdk/indicators/rsi/any_name.py
```

扫描目录：

```text
<应用配置>/tqsdk/indicators/
<QStandardPaths::AppConfigLocation>/indicators/
$TQSDK_INDICATOR_PLUGIN_DIRS
<sidecar 目录>/plugins/
```

UI 和 sidecar 使用同一目录列表。sidecar 在 `ready` 帧里返回完整 catalog，
Qt UI 用它构建菜单、参数编辑和颜色；GUI 不执行 Python。

### 运算契约

- `compute(params, bars)` 返回 `{series_name: values}`；
- 每个 values 的长度必须等于 bars；
- 返回 `None` 或 NaN 会从图中过滤；
- `panel` 只能是 `main` 或 `sub`；
- 参数目前按数值类型编辑，`type` 支持 `int` / `float`；
- `compute` 和元数据可以放在同一个模块；
- sidecar 启动时 import 插件，插件异常只跳过该插件，不影响其他指标。

### 使用流程

1. 放置单个 `.py` 文件；
2. 指标菜单中的“刷新指标列表”；
3. sidecar 重扫并返回新 catalog；
4. 菜单立即出现新指标；
5. 启用后 sidecar 发送 `init` 和 K 线，开始计算。

新增指标不需要重新编译 C++、Qt UI 或 sidecar 主脚本。

### 注意事项

- 插件是用户代码，当前在独立 sidecar 进程中直接 import，没有文件/网络/进程沙箱；
- 插件目录、manifest 元数据和 `.py` 文件都不写入 QSettings；
- 多指标同时启用时 sidecar 按请求顺序计算；大窗口/多指标基准测试留在 Phase C；
- 参数目前只支持数值类型，字符串/枚举参数后续扩展。

### 12.9 共享指标库 `indicator_lib.py`

通用计算和颜色集中放在：

```text
ui/indicator_sidecar/indicator_lib.py
```

插件可以直接导入：

```python
from indicator_lib import (
    COLORS, PALETTE, param, series_def,
    sma, ema, stdev, boll, macd, rsi,
    highest, lowest, ref, crossover, crossunder,
)
```

提供：

- `sma` / `ema` / `stdev`；
- `boll`（中轨、上轨、下轨）；
- `macd`（DIF、DEA、柱）；
- `rsi`；
- `highest` / `lowest` / `ref` / `crossover` / `crossunder`；
- `safe`：把非法数值转为 None；
- `COLORS` / `PALETTE`：统一的指标颜色；
- `param()` / `series_def()`：减少 manifest 重复代码。

现有 MA/EMA/BOLL/MACD 插件已经改成只保留元数据和参数映射，计算统一调用
`indicator_lib`。例如：

```python
from indicator_lib import param, series_def, sma

MANIFEST = {
    "id": "ma",
    "name": "MA",
    "panel": "main",
    "params": [param("fast", "快线", 5, 1, 500)],
    "series": [series_def("ma_fast", "MA快", "gold")],
}

def compute(params, bars):
    closes = [float(bar["close"]) for bar in bars]
    return {"ma_fast": sma(closes, int(params.get("fast", 5)))}
```

sidecar 启动时会把自身目录加入 `sys.path`，所以用户插件目录里的文件也可以
直接 `import indicator_lib`。颜色名称包括：

```text
gold / blue / orange / purple / red / teal / muted / green
```

### 12.10 验证记录

2026-11-04 在 `build-ui`（RelWithDebInfo，Qt 6.11.2）执行：

```text
ctest --test-dir build-ui --output-on-failure
273/273 tests passed
2 个真实行情网络测试按设计跳过
```

指标相关覆盖：

- `tqsdk-ui-indicator`：MA/MACD 计算、增量 update、参数下发、单文件外部插件动态加载、非法 sidecar 路径、崩溃 sidecar；
- `tqsdk-ui-chart`：主图叠加 + MACD 副图绘制 smoke；
- `tqsdk-ui-window`：FakeNodeClient K 线 → IndicatorManager → sidecar → 图表端到端；
- 4K UI stress 仍保持最大 event-loop gap 约 9ms。

已知性能边界：每次 K 线变化会由 sidecar 返回当前窗口数值序列；当前
P3 只做单窗口/单 sidecar 的性能验证，100 指标、10 年 1m bar 的 Phase C
压力测试尚未执行。

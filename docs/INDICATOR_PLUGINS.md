# K 线指标插件设计

> 状态：架构设计，暂未实现。
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
- Python sidecar 只看到行情数据和指标 manifest，不接触 node/账户/交易；
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
  -> 扫描插件目录和 manifest
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

```text
~/.config/tqsdk/indicators/
  ma_cross/
    manifest.json
    indicator.py
    requirements.txt   # 可选，首次启用时提示安装
```

项目内也可以放只读示例：

```text
indicators/builtin/
```

### 7.4 Python API 草案

```python
# indicator.py
def on_init(ctx):
    ctx.declare_output("ma_fast", panel="main", color="#f5c542")
    ctx.declare_output("ma_slow", panel="main", color="#4f8cff")

def on_bar(ctx, bar):
    ctx.plot("ma_fast", ctx.sma(bar.close, 5))
    ctx.plot("ma_slow", ctx.sma(bar.close, 20))
```

`ctx` 只暴露：

- 当前参数；
- 当前 bar 和窗口历史；
- 预编译的 SMA/EMA/STD 等基础算子；
- `plot(name, value)`；
- 错误/日志接口。

不提供：网络、文件、subprocess、交易、Qt 对象。

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

指标配置写入 `QSettings`，但 Python 文件、manifest 和依赖不写入 QSettings。

## 10. 分阶段计划

### Phase A：DSL 内核

- `IndicatorSpec` / `IndicatorManager`；
- YAML/JSON manifest 校验；
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

# Web 自定义指标：WASM 插件方案

## 1. 当前实现

Web 指标已经统一为 **WebAssembly 插件 + Worker 沙箱 + Manifest 声明式绘图**。

原先的 MA / BOLL / MACD / RSI 现在也都是 WASM 插件；原生指标渲染路径不再作为
UI 默认路径，指标设置里看到的是插件列表和插件参数。

### 1.1 当前插件

| id | 指标 | 输出 | 默认启用 |
|---|---|---|---|
| `ema6` | EMA 多均线 | EMA5/10/20/60/120/200，6 条线 | 是 |
| `ma` | MA 多均线 | MA5/10/20/60，4 条线 | 是 |
| `boll` | Bollinger Bands | 上下轨 band + 中轨 line | 否 |
| `macd` | MACD | DIF、DEA、HIST，副图 | 否 |
| `rsi` | RSI | 副图 | 否 |
| `atr` | ATR | 副图 | 是 |
| `donchian` | Donchian Channel | 上下轨 band + 中轨，主图 | 是 |

插件资源：

```text
web/public/plugins/<id>/manifest.json
web/public/plugins/<id>/<id>.wasm
web/public/plugins/<id>/sha256.txt
```

Rust 源码：

```text
wasm/plugins/<id>/
wasm/indicator-sdk/
```

## 2. 架构

```text
             manifest.json
                   │
                   ▼
          Host / IndicatorSettings
                   │
     每个启用的插件各一个 Worker
                   │
         bars + params (binary)
                   │
                   ▼
             Dedicated Worker
                   │
          WebAssembly.instantiate()
                   │
                   ▼
         alloc / run / dealloc
                   │
             f64 output arrays
                   │
                   ▼
        Lightweight Charts adapter
```

职责划分：

| 层 | 负责 |
|---|---|
| Manifest | 参数、输出、pane、颜色、绘图类型 |
| WASM | 只计算指标数值 |
| Worker | 加载、隔离、超时、执行 |
| Host | 数据编码、生命周期、manifest 校验、插件调度 |
| Chart Adapter | line/histogram/area/band/markers 绘图 |

多个插件并行计算；任一插件失败不会阻塞其他插件显示。

## 3. Manifest

以 EMA 插件为例：

```json
{
  "schema": 1,
  "abi": 1,
  "id": "ema6",
  "name": "EMA 多均线",
  "version": "0.2.0",
  "wasm": "ema6.wasm",
  "params": [
    {"id": "period1", "label": "EMA1 周期", "type": "int", "default": 5, "min": 1, "max": 500},
    {"id": "period2", "label": "EMA2 周期", "type": "int", "default": 10, "min": 1, "max": 500},
    {"id": "period3", "label": "EMA3 周期", "type": "int", "default": 20, "min": 1, "max": 500},
    {"id": "period4", "label": "EMA4 周期", "type": "int", "default": 60, "min": 1, "max": 500},
    {"id": "period5", "label": "EMA5 周期", "type": "int", "default": 120, "min": 1, "max": 500},
    {"id": "period6", "label": "EMA6 周期", "type": "int", "default": 200, "min": 1, "max": 500}
  ],
  "outputs": [
    {"id": "ema1", "title": "EMA{period1}", "pane": "main", "plot": "line", "color": "#f0b90b"},
    {"id": "ema2", "title": "EMA{period2}", "pane": "main", "plot": "line", "color": "#a78bfa"},
    {"id": "ema3", "title": "EMA{period3}", "pane": "main", "plot": "line", "color": "#53b7ff"},
    {"id": "ema4", "title": "EMA{period4}", "pane": "main", "plot": "line", "color": "#22c55e"},
    {"id": "ema5", "title": "EMA{period5}", "pane": "main", "plot": "line", "color": "#ff7a90"},
    {"id": "ema6", "title": "EMA{period6}", "pane": "main", "plot": "line", "color": "#f97316"}
  ]
}
```

标题中的 `{period1}` 会按当前参数值解析，修改周期后图例标题会同步更新为
`EMA8` 等。

### 3.1 输出类型

```text
line        主图/副图折线
histogram   柱状图
area        面积图
band        上下轨，channels=2
markers     信号标记
```

`pane`：

- `main`：主图叠加；
- 其他字符串：副图。Host 按 pane id 分配独立副图，同一 pane id 的输出共用副图。

插件输出与输入 bars 一一对齐，`NaN` 表示该 bar 不绘制。

## 4. Wasm ABI

### 4.1 导出

```c
uint8_t* alloc(uint32_t size);
void dealloc(uint8_t* ptr, uint32_t size);
int32_t run(
    const uint8_t* input,
    uint32_t input_len,
    uint8_t* output,
    uint32_t output_capacity
);
```

`run` 返回写入的字节数；负数表示错误。

### 4.2 输入布局 `TQW1`

小端序：

```text
u32 magic        0x31575154  ("TQW1")
u32 version      1
u32 bar_count
u32 param_count
u32 output_count  // 所有 channels 的总数
u32 reserved
f64 params[param_count]
f64 open[bar_count]
f64 high[bar_count]
f64 low[bar_count]
f64 close[bar_count]
f64 volume[bar_count]
```

约束：

- 24 字节 header；
- 参数和 OHLCV 使用 f64；
- 所有数组 8 字节对齐；
- 当前最大 20,000 根 bars、8 个参数、16 个输出 channels；
- 字段顺序属于 ABI v1，修改必须提升 ABI 版本。

### 4.3 输出布局

```text
f64 output_0[bar_count]
f64 output_1[bar_count]
...
```

输出数量等于 manifest 中所有输出的 `channels` 之和。

## 5. Rust SDK 与插件

SDK：

```text
wasm/indicator-sdk/
```

每个插件只需要实现一个入口函数，然后用宏导出：

```rust
use tqsdk_indicator_sdk::Context;

fn indicator(ctx: &mut Context<'_>) -> i32 {
    let period = ctx.param_usize(0, 20).clamp(1, 500);
    let high = ctx.high();
    let low = ctx.low();
    // ... 计算 ...
    if ctx.write_output(0, &upper) { 0 } else { -1 }
}

tqsdk_indicator_sdk::export_indicator!(indicator);
```

SDK 负责：

- 分配对齐内存；
- 解析 TQW1 header/参数/OHLCV；
- 校验 bar 数量、参数数量、输出容量；
- 提供 `open/high/low/close/volume` 和 `write_output`；
- 导出标准 ABI。

### 5.1 当前插件实现

- EMA：六个可配置周期；
- MA：四个可配置周期；
- BOLL：SMA 中轨 + 标准差上下轨；
- MACD：EMA fast/slow、DIF/DEA/histogram；
- RSI：Wilder RSI；
- ATR：True Range + Wilder 平滑；
- Donchian：窗口最高/最低/中位通道。

ATR 输出到 `sub:atr`，MACD/RSI 各自进入独立副图。

## 6. 构建

```bash
./scripts/build_wasm_plugins.sh
```

脚本会遍历 `wasm/plugins/*`：

```text
rustup target add wasm32-unknown-unknown
cargo build --target wasm32-unknown-unknown --release
复制 wasm + manifest 到 web/public/plugins/<id>/
生成 sha256.txt
```

只构建一个插件：

```bash
./scripts/build_wasm_plugins.sh wasm/plugins/atr
```

## 7. 安全设计

当前已实现：

- 每个插件独立 Worker；
- Wasm import 必须为空，否则拒绝加载；
- 不向插件暴露 host function；
- 不提供 WASI/网络/文件系统；
- manifest 校验 id、版本、参数范围、输出类型、颜色和 wasm 文件名；
- wasm 路径禁止 `..` 和绝对 URL；
- 输出长度由 Host 严格校验；
- 每次计算 5 秒超时，超时终止该插件的 Worker；
- 插件加载失败只影响该插件，并在设置面板显示错误；
- 主线程不执行插件代码。

仍需加强：

- host 导入 `WebAssembly.Memory` 的硬内存上限；
- Worker CPU 配额；
- manifest hash/签名校验；
- 第三方插件 allowlist；
- 第三方插件唯一 origin / sandbox iframe；
- 插件供应链审计。

当前“自己写插件”的安全模型是：

```text
无 host imports + 独立 Worker + 超时 + 输出校验
```

这不是对恶意第三方代码的最终生产沙箱。

## 8. 性能

当前策略：

- 每个启用插件独立 Worker；
- 多插件并行计算；
- bars/参数变化后 debounce 120ms；
- 主线程只负责渲染；
- 使用 transferable ArrayBuffer 返回 f64 输出；
- 每个插件最多 20,000 根 bars；
- 计算是 O(n)，ATR/Donchian 等窗口指标按当前配置周期计算。

未实现：

- 新增 bar 的增量 ABI；
- 结果 LRU；
- SharedArrayBuffer；
- 插件计算进程池。

## 9. 图表适配

`web/src/chart/KLineChart.tsx` 根据 manifest 自动创建：

```text
line      -> LineSeries
histogram -> HistogramSeries
area      -> AreaSeries
band      -> 上下轨 LineSeries
markers   -> createSeriesMarkers
```

`pane === "main"` 叠加主图；不同 pane id 自动分配副图。

## 10. 前端入口

```bash
cd web
pnpm dev
```

然后：

1. 打开 K 线；
2. 点击「指标设置」；
3. 可以看到 EMA、MA、BOLL、MACD、RSI、ATR、Donchian；
4. 勾选/取消插件启用；
5. 修改参数后自动重算；
6. 完成设置。

默认启用：

```text
EMA 多均线
MA 多均线
ATR
Donchian Channel
```

默认不启用：

```text
BOLL
MACD
RSI
```

它们可以在指标设置中启用。

## 11. 测试

```bash
# 直接检查所有 wasm 的 ABI、输出字节数和有限值
node scripts/check_wasm_plugins.mjs

cd web
pnpm typecheck
pnpm test
pnpm build
pnpm e2e
```

当前检查：

```text
node scripts/check_wasm_plugins.mjs
  PASS atr / boll / donchian / ema6 / ma / macd / rsi

Vitest: 14/14
Playwright: 1/1
```

Playwright 覆盖：

- 插件设置中包含 EMA/MA/BOLL/MACD/RSI/ATR/Donchian；
- 默认图例包含 EMA、MA、ATR、Donchian；
- 修改 EMA 参数后图例动态更新；
- 启用 MACD 后出现 DIF/HIST 输出；
- 自选大小写、下单板平多/平空不回归。

## 12. 后续

- 增加 KDJ、CCI、OBV 等插件；
- 任意条数 EMA/MA 的动态实例；
- 每线颜色/隐藏开关；
- 插件参数持久化；
- 插件签名和 hash 校验；
- 第三方插件隔离与安装流程。

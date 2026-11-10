# Web 自定义指标：WASM 插件方案

## 1. 当前实现

Web 指标已经统一为 **WebAssembly 插件 + Worker 沙箱 + Manifest 声明式绘图**。

原先的 MA / BOLL / MACD / RSI 现在也都是 WASM 插件；原生指标渲染路径不再作为
UI 默认路径，指标设置里看到的是插件列表和插件参数。

### 1.1 当前插件

| id | 指标 | 输出 | 默认启用 |
|---|---|---|---|
| `ema6` | EMA 多均线 | 最多 8 个槽位，默认显示 EMA5/10/20/60/120/200 | 是 |
| `ma` | MA 多均线 | 最多 8 个槽位，默认显示 MA5/10/20/60 | 否 |
| `boll` | Bollinger Bands | 上下轨 band + 中轨 line | 否 |
| `macd` | MACD | DIF、DEA、HIST，副图 | 是 |
| `rsi` | RSI | 副图 | 否 |
| `atr` | ATR | 副图 | 否 |
| `donchian` | Donchian Channel | 上下轨 band + 中轨，主图 | 否 |
| `kdj` | KDJ | K/D/J 三条线，副图 | 否 |
| `cci` | CCI | CCI 单线，副图 | 否 |
| `obv` | OBV | 能量潮，副图 | 否 |
| `vwap` | VWAP | 累计成交量加权均价，主图 | 否 |

插件的 `defaultEnabled` 和 `order` 写在 manifest 中，前端注册表由脚本扫描生成，
不再在 TS 文件里手工维护插件列表。

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
  "dynamicLines": true,
  "defaultEnabled": true,
  "order": 10,
  "params": [
    {"id": "period1", "label": "EMA1 周期", "type": "int", "default": 5, "min": 1, "max": 500},
    {"id": "period2", "label": "EMA2 周期", "type": "int", "default": 10, "min": 1, "max": 500},
    {"id": "period3", "label": "EMA3 周期", "type": "int", "default": 20, "min": 1, "max": 500},
    {"id": "period4", "label": "EMA4 周期", "type": "int", "default": 60, "min": 1, "max": 500},
    {"id": "period5", "label": "EMA5 周期", "type": "int", "default": 120, "min": 1, "max": 500},
    {"id": "period6", "label": "EMA6 周期", "type": "int", "default": 200, "min": 1, "max": 500},
    {"id": "period7", "label": "EMA7 周期", "type": "int", "default": 250, "min": 1, "max": 500},
    {"id": "period8", "label": "EMA8 周期", "type": "int", "default": 500, "min": 1, "max": 500}
  ],
  "outputs": [
    {"id": "ema1", "title": "EMA{period1}", "pane": "main", "plot": "line", "color": "#f0b90b"},
    {"id": "ema2", "title": "EMA{period2}", "pane": "main", "plot": "line", "color": "#a78bfa"},
    {"id": "ema3", "title": "EMA{period3}", "pane": "main", "plot": "line", "color": "#53b7ff"},
    {"id": "ema4", "title": "EMA{period4}", "pane": "main", "plot": "line", "color": "#22c55e"},
    {"id": "ema5", "title": "EMA{period5}", "pane": "main", "plot": "line", "color": "#ff7a90"},
    {"id": "ema6", "title": "EMA{period6}", "pane": "main", "plot": "line", "color": "#f97316"},
    {"id": "ema7", "title": "EMA{period7}", "pane": "main", "plot": "line", "color": "#38bdf8", "defaultVisible": false},
    {"id": "ema8", "title": "EMA{period8}", "pane": "main", "plot": "line", "color": "#e879f9", "defaultVisible": false}
  ]
}
```

`defaultEnabled` 和 `order` 控制自动生成的注册表：前者是默认启用状态，后者是
列表顺序。`dynamicLines: true` 表示 EMA/MA 支持在详情页添加/移除输出槽位。

输出项中的 `defaultVisible` 控制初始是否显示，`periodParam` 指定该输出对应哪个
周期参数。标题中的 `{period1}` 会按当前参数值解析，修改周期后图例标题会同步更新为
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

构建结束会自动执行：

```bash
cd web && node scripts/generate-indicator-registry.mjs
```

它扫描 `web/public/plugins/*/manifest.json`，按 `order` 排序，生成：

```text
web/src/indicators/wasm/registry.generated.ts
```

前端 `predev` / `prebuild` / `pretypecheck` 也会自动生成该文件；新增插件目录后
不需要手工修改 registry。

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

1. 打开 K 线，点击「指标设置」；
2. 出现浮动指标列表，可直接启用/关闭插件；
3. 点击某个指标进入详情；
4. 详情页可以修改参数；
5. 详情页可以逐条配置输出的显示、颜色、线型和线宽；
6. EMA/MA 详情页支持「+ 添加均线」和「移除」，最多 8 个周期槽位；
7. 右上角返回列表，关闭后自动重算。

当前默认启用：

```text
EMA 多均线
MACD
```

当前默认不启用：

```text
MA 多均线
BOLL
RSI
ATR
Donchian Channel
```

它们都可以在浮动列表中启用。

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
  PASS atr / boll / donchian / ema6(8ch) / ma(8ch) / macd / rsi

Vitest: 20/20
Playwright: 1/1
```

Playwright 覆盖：

- 浮动列表包含 EMA/MA/BOLL/MACD/RSI/ATR/Donchian/KDJ/CCI/OBV/VWAP，列表由 auto-registry 生成；
- 指标启用/参数/输出样式/动态均线增删刷新后恢复；
- 点击 EMA 进入详情，修改周期、线型颜色、添加/移除均线；
- 默认图例包含 EMA5/10/20/60/120/200 和 MACD DIF/DEA/HIST；
- 启用 MA 后出现 MA5；
- 关闭 MACD DIF 输出后对应图例消失；
- 自选大小写、下单板平多/平空不回归。

## 12. 本地持久化

指标配置保存在浏览器 `localStorage`：

```text
key: tqsdk.wasm.indicators.v1
```

保存内容：

- 每个插件是否启用；
- 参数值；
- 每个输出的显隐/颜色/线型/线宽；
- 动态均线的隐藏/显示状态。

坐标系恢复时会按 manifest 的 min/max 重新校验，非法颜色/线型/宽度会回退到默认值。
这不是 cookie，也不会自动同步到其他浏览器或服务器。

## 13. 后续

- 增加 KDJ、CCI、OBV 等插件；
- 将 EMA/MA 从 8 个槽位扩展为真正无上限的实例（需要动态输出 ABI 或多实例调度）；
- 输出参数持久化；
- 插件签名和 hash 校验；
- 第三方插件隔离与安装流程。

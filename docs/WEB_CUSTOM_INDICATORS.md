# Web 自定义指标：WASM 插件方案

## 1. 当前实现

Web 自定义指标采用 **WebAssembly 插件 + Worker 沙箱 + Manifest 声明式绘图**。

内置的 EMA 多均线（默认 6/12/24/60）已经由 WASM 插件提供：

```text
web/public/plugins/ema6/manifest.json
web/public/plugins/ema6/ema6.wasm
```

旧的“EMA/SMA 参数化”入口仍保留在指标设置中，作为兼容/降级能力；默认不再自动添加，
因此正常加载时不会和 WASM 多均线重复。

```text
WASM 指标：主路径
内置参数指标：兼容旧 localStorage / WASM 不可用时的 fallback
```

如果 WASM 插件加载或计算失败，图表会显示 `EMA6 (fallback)`、`EMA12 (fallback)`、`EMA24 (fallback)`、`EMA60 (fallback)`，不会静默失去均线。

## 2. 架构

```text
             manifest.json
                   │
                   ▼
          Host / IndicatorSettings
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
| Host | 数据编码、生命周期、manifest 校验 |
| Chart Adapter | line/histogram/area/band/markers 绘图 |

WASM 不负责 DOM、绘图、网络、定时器或文件访问。

## 3. Manifest

路径：

```text
web/public/plugins/ema6/manifest.json
```

示例：

```json
{
  "schema": 1,
  "abi": 1,
  "id": "ema6",
  "name": "EMA 多均线",
  "version": "0.2.0",
  "wasm": "ema6.wasm",
  "params": [
    {"id": "period1", "label": "EMA1 周期", "type": "int", "default": 6, "min": 1, "max": 500},
    {"id": "period2", "label": "EMA2 周期", "type": "int", "default": 12, "min": 1, "max": 500},
    {"id": "period3", "label": "EMA3 周期", "type": "int", "default": 24, "min": 1, "max": 500},
    {"id": "period4", "label": "EMA4 周期", "type": "int", "default": 60, "min": 1, "max": 500}
  ],
  "outputs": [
    {
      "id": "ema1",
      "title": "EMA{period1}",
      "pane": "main",
      "plot": "line",
      "color": "#f0b90b",
      "lineWidth": 1
    },
    {"id": "ema2", "title": "EMA{period2}", "pane": "main", "plot": "line", "color": "#53b7ff"},
    {"id": "ema3", "title": "EMA{period3}", "pane": "main", "plot": "line", "color": "#ff7a90"},
    {"id": "ema4", "title": "EMA{period4}", "pane": "main", "plot": "line", "color": "#a78bfa"}
  ]
}
```

当前支持的输出类型：

```text
line        主图/副图折线
histogram   柱状图
area        面积图
band        上下轨，channels=2
markers     信号标记
```

标题中的 `{period1}` 这类占位符会按当前参数值解析，因此修改周期后图例标题会同步更新为 `EMA8` 等。

`pane`：

- `main`：主图叠加；
- 其他字符串：副图。Host 按 pane id 分配独立副图，同一 pane id 的输出共用副图。

输出值必须与输入 bars 一一对齐：

```text
values[i] -> bars[i]
```

`NaN` 表示该 bar 不绘制。

## 4. Wasm ABI

### 4.1 导出

插件必须导出：

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

## 5. Rust SDK 与样例插件

SDK：

```text
wasm/indicator-sdk/
```

样例：

```text
wasm/plugins/ema6/
```

插件核心代码：

```rust
fn indicator(ctx: &mut Context<'_>) -> i32 {
    let period = ctx.param_usize(0, 6).clamp(1, 500);
    let close = ctx.close();
    let mut values = vec![f64::NAN; ctx.bar_count()];
    // ... EMA ...
    if ctx.write_output(0, &values) { 0 } else { -1 }
}

#[no_mangle]
pub extern "C" fn run(input_ptr: *const u8, input_len: usize,
                      output_ptr: *mut u8, output_capacity: usize) -> i32 {
    tqsdk_indicator_sdk::execute(
        input_ptr, input_len, output_ptr, output_capacity, indicator)
}
```

构建并复制到 public：

```bash
./scripts/build_wasm_plugins.sh
```

脚本会：

```text
rustup target add wasm32-unknown-unknown
cargo build --target wasm32-unknown-unknown --release
复制 ema6.wasm + manifest.json 到 web/public/plugins/ema6/
生成 sha256.txt
```

SDK 不依赖任何外部 crate；插件编译为 `wasm32-unknown-unknown`，不链接 WASI。

## 6. 安全设计

### 6.1 当前已实现

- Wasm 只在独立 Worker 中实例化；
- 模块 import 必须为空，否则拒绝加载；
- 不向插件暴露任何 host function；
- 不提供 WASI/网络/文件系统；
- manifest 校验 id、版本、参数范围、输出类型、颜色和 wasm 文件名；
- wasm 路径禁止 `..` 和绝对 URL；
- 输出长度由 Host 严格校验；
- 每次计算 5 秒超时，超时直接终止 Worker；
- 计算失败时回退到原生多 EMA fallback；
- 主线程不执行插件代码。

### 6.2 仍需加强

- 使用 host 导入的 `WebAssembly.Memory` 设置硬内存上限；
- Worker CPU 配额；
- manifest hash/签名校验；
- 第三方插件 allowlist；
- 第三方插件使用独立 origin 的 sandbox iframe；
- 插件供应链审计。

当前“自己写插件”的安全模型是：

```text
无 host imports + Worker + 超时 + 输出校验
```

这不是对恶意第三方代码的最终生产沙箱，责任边界已在文档中明确。

## 7. 性能

当前策略：

- 只在 bars 变化后 debounce 120ms 计算；
- 主线程只负责渲染；
- Worker 与主线程通过 transferable ArrayBuffer 返回结果；
- 运算为 O(n)；
- 最多 20,000 根 bars；
- 后续可以做“新增 bar 增量计算”和结果缓存。

目前未实现：

- 增量 ABI；
- 多插件并行池；
- 结果 LRU；
- SharedArrayBuffer。

## 8. 图表适配

`web/src/chart/KLineChart.tsx` 根据 manifest 自动创建：

```text
line      -> LineSeries
histogram -> HistogramSeries
area      -> AreaSeries
band      -> 上下轨 LineSeries
markers   -> createSeriesMarkers
```

`pane === "main"` 叠加主图；其他 pane 进入副图。

## 9. 前端入口

打开 WebUI：

```bash
cd web
pnpm dev
```

然后：

1. 打开 K 线；
2. 点击「指标设置」；
3. 顶部看到「WASM 插件 / EMA 多均线 v0.2.0」；
4. 修改四个周期，例如 8/20/40/60；
5. 完成设置；
6. 图表图例显示 `EMA8`、`EMA20`、`EMA40`、`EMA60`，副图/标记插件会按 manifest 自动出现。

如果看到：

```text
EMA6 (fallback) / EMA12 (fallback) / EMA24 (fallback) / EMA60 (fallback)
```

说明 WASM 未加载或计算失败，需要检查：

```bash
curl -I http://127.0.0.1:5173/plugins/ema6/manifest.json
curl -I http://127.0.0.1:5173/plugins/ema6/ema6.wasm
```

以及浏览器 Console 的 Worker 错误。

## 10. 测试

```bash
cd web
pnpm typecheck
pnpm test
pnpm build
pnpm e2e
```

覆盖：

- Manifest schema、路径穿越、非法输出类型；
- TQW1 编码/解码；
- 默认 WASM 插件加载；
- 图表出现 `EMA6`、`EMA12`、`EMA24`、`EMA60`，而不是 fallback；
- WASM 参数 UI 默认值依次为 6/12/24/60；
- 原有自选大小写、下单板平多/平空和图表切换不回归。

当前测试：

```text
Vitest: 14/14
Playwright: 1/1
```

## 11. 后续扩展

### 第一批

- 增加一个 WASM MACD 样例，验证副图和 histogram；
- 增加 band 样例；
- 增加 markers 样例；
- 输出 channel 到 pane 的分配器；
- 参数持久化。

### 第二批

- 插件列表和启用/禁用 UI；
- 本地插件目录扫描；
- manifest 签名；
- 插件版本/ABI 兼容检查；
- Rust SDK 文档和模板仓库。

### 第三批

- 第三方插件市场；
- sandbox iframe；
- 插件权限声明；
- 资源配额和审计日志。

# TQSDK Board 指标与自定义指标设计

## 1. 现状

Web 端已经支持：

- MA（可增删周期）；
- BOLL（周期、标准差倍数）；
- MACD（快线、慢线、信号周期）；
- RSI（周期）；
- 每条 MA/BOLL/MACD/RSI 线颜色可自定义；
- 配置保存在 `localStorage`，刷新/重连后恢复；
- 指标设置入口在 K 线工具栏。

当前实现只有 WASM 指标插件路径：`WasmManifest + Worker + ABI`，
MA/BOLL/MACD/RSI/EMA/ATR/Donchian 已迁移。旧内置指标参数化和
`tqsdk.indicators.v1` storage API 已删除，避免两套指标语义并存。

WASM 路径的 ABI、Manifest、安全边界和构建流程见
[WEB_CUSTOM_INDICATORS.md](WEB_CUSTOM_INDICATORS.md)。下面保留指标实例/配置容器的演进设计。

## 2. 目标

1. 指标定义与图表渲染解耦；
2. 参数、颜色、线宽、线型、显示位置可配置；
3. 支持同一指标多实例（MA5、MA20、MA60 视觉上可独立配置）；
4. 支持主图叠加和副图；
5. 配置按用户持久化，并可按合约覆盖；
6. 计算可迁移到 Web Worker，不阻塞 UI；
7. 自定义指标通过 WASM 插件注册，而不是运行时 `eval`。

## 3. 核心数据模型

### 3.1 指标定义

```ts
type IndicatorKind = "MA" | "EMA" | "BOLL" | "MACD" | "RSI" | "KDJ" | string;

interface InputField {
  key: string;
  label: string;
  type: "int" | "float" | "period-list" | "enum" | "boolean";
  default: unknown;
  min?: number;
  max?: number;
  step?: number;
}

interface OutputSpec {
  key: string;
  label: string;
  style: "line" | "histogram" | "area";
  defaultColor: string;       // 语义默认色，不写死主题
  defaultWidth?: number;
  defaultStyle?: "solid" | "dashed" | "dotted";
}

interface IndicatorDefinition {
  type: IndicatorKind;
  title: string;
  pane: "main" | "sub";
  inputs: InputField[];
  outputs: OutputSpec[];
  calculate(bars: OHLCV[], params: Record<string, unknown>): Record<string, OutputPoint[]>;
}
```

### 3.2 指标实例

```ts
interface IndicatorInstance {
  id: string;                 // 稳定 id，用于 series 复用
  type: IndicatorKind;
  enabled: boolean;
  params: Record<string, unknown>;
  outputs: Record<string, {
    visible: boolean;
    color?: string;           // 为空时使用当前主题默认色
    width?: number;
    lineStyle?: "solid" | "dashed" | "dotted";
  }>;
  paneHeight?: number;
}
```

### 3.3 配置容器

```ts
interface IndicatorWorkspace {
  version: 2;
  instances: IndicatorInstance[];
  global?: {
    ma?: { periods: number[]; colors: string[] };
    boll?: { period: number; multiplier: number; colors: object };
    macd?: { fast: number; slow: number; signal: number; colors: object };
    rsi?: { period: number; colors: object };
  };
  overrides?: Record<string, { instances: IndicatorInstance[] }>; // 按 symbol 可选
}
```

旧 `IndicatorConfig` 兼容层和 `?indicators=...` query 已移除；指标状态统一由
`tqsdk.wasm.indicators.v1` 管理，见 [WEB_CUSTOM_INDICATORS.md](WEB_CUSTOM_INDICATORS.md)。

## 4. 渲染架构

```text
IndicatorDefinition registry
          │
Indicator engine (pure functions, later Worker)
          │  outputs: [{key, data, pane, style}]
          ▼
Chart adapter
  ├─ pane 0: Candlestick + MA/BOLL overlays
  ├─ pane 1..N: MACD/RSI/KDJ
  ├─ stable series key = instanceId + outputKey
  └─ only update changed series
```

约束：

- **series id 稳定**：不能只用类型名，否则两个 MA 实例会互相覆盖；
- **主题色与用户色分开**：用户覆盖优先，否则跟随 light/dark；
- **参数变化才重建 series**，quote/K 线更新只 `setData/update`；
- **output 可跨 pane**，一个指标可以有主图和副图输出；
- 计算失败不能清空整个图，只禁用该实例并显示错误。

## 5. 计算性能

- 指标纯函数放 `src/indicators/engine`；
- 大 K 线量（>5000）交给 Web Worker；
- Worker 输入只传 `bar.time/close/high/low` 和参数，输出数值数组；
- 增量更新支持：
  - 当前 bar 更新；
  - 新 bar 追加；
  - 历史重载时整段替换；
- 每个指标维护自己的计算版本，避免每次 tick 全量重算。

## 6. 设置 UI

当前设置弹层由 WASM manifest 驱动：

- 插件启用/停用；
- 参数表单（整数/浮点/枚举/bool）；
- 每个输出的颜色、线型、线宽、显隐；
- EMA/MA 最多 8 个动态周期槽位；
- 恢复默认由 manifest 默认值 + 浏览器本地配置提供。

旧内置指标参数配置、`tqsdk.indicators.v1` 和 `?indicators=...` 已移除；后续扩展应
继续通过 WASM manifest/ABI 完成，见 [WEB_CUSTOM_INDICATORS.md](WEB_CUSTOM_INDICATORS.md)。

## 7. 自定义指标注册

第一版不开放运行时脚本，避免 XSS/任意代码执行。建议：

```ts
// src/indicators/registry.ts
registerIndicator({
  type: "MOM",
  title: "动量",
  pane: "sub",
  inputs: [{ key: "period", type: "int", default: 10 }],
  outputs: [{ key: "value", label: "MOM", style: "line", defaultColor: "#53b7ff" }],
  calculate(bars, params) { ... },
});
```

外部插件（后续）采用受信任构建期模块：

```text
indicator-plugins/
  momentum/
    manifest.json
    index.ts
```

`manifest.json` 声明参数、输出和入口；打包时静态校验，不执行远程 JS。

## 8. 主题规范

- `theme.ts` 提供 dark/light 语义色；
- 指标定义只提供默认语义，不直接写 `#...` 作为唯一颜色；
- 用户自定义颜色是覆盖值；
- 图表背景、网格、坐标轴、legend、tooltip 必须统一从主题读取；
- 浅色主题下所有输入框、select、suggestion、价格提示必须覆盖为浅背景。

## 9. 验收

- 修改 MA 周期后图表立即变化，刷新后保留；
- 修改颜色后 light/dark 都可读；
- 同一指标添加两个实例不冲突；
- MACD/RSI 副图可关闭并释放 pane；
- 10 万根 K 线切换指标不阻塞主线程（Worker 版本）；
- 指标配置损坏时回退默认值，不白屏。

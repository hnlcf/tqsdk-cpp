# Web 自定义指标：安全声明式方案

## 1. 选择结论

当前采用 **安全声明式指标（Safe Declarative Indicator）**，不执行用户上传的
JavaScript。

一个自定义指标只是一条可序列化的数据：

```ts
interface CustomIndicatorSpec {
  id: string;
  kind: "EMA" | "SMA";
  period: number;
  color: string;
  enabled: boolean;
}
```

默认配置包含一条：

```json
{
  "id": "ema-6",
  "kind": "EMA",
  "period": 6,
  "color": "#f0b90b",
  "enabled": true
}
```

因此打开 K 线即可看到 `EMA6`；也可以在「指标设置 → 自定义指标」中关闭、改周期、
改颜色或添加更多条。

## 2. 为什么不用任意 JS

任意 JS 插件即使放在 Web Worker 里，也可能：

- 访问网络（`fetch`/WebSocket）；
- 读取/污染全局状态；
- 消耗无限 CPU；
- 通过持久化配置注入不可审计代码；
- 在 CSP/权限配置不完整时获得超出预期的能力。

声明式方案把可执行面限制在应用自身的白名单函数中：

```text
spec JSON -> normalize -> EMA/SMA whitelist -> ValuePoint[]
```

没有 `eval`、`Function`、远程脚本加载或 DOM 访问。

## 3. 抽象与易用性

- UI 只需要选择 `EMA`/`SMA`、周期、颜色和显示开关；
- 配置可安全写入 `localStorage`；
- 后端 schema 不参与指标计算，指标保持为前端展示层；
- 后续要增加指标，只需扩展白名单和纯函数，不需要开放代码执行。

当前白名单：

```text
EMA -> ema(bars, period)
SMA -> sma(bars, period)
```

## 4. 安全与边界

- 周期范围 `1..500`；
- 最多 12 条自定义指标；
- 颜色必须是 `#rrggbb`；
- 非法/旧配置在 `normalizeIndicatorConfig()` 中丢弃；
- 不使用 `eval` / `new Function` / 动态 import；
- 不执行用户提供的 URL 或脚本；
- 目前只支持主图叠加线，不开放自定义副图坐标轴。

## 5. 性能

- 每条序列计算是 O(n)，与现有 MA/BOLL/MACD 同一复杂度；
- 上限 12 条，避免无界配置导致重绘过多；
- K 线更新时复用现有 Series，不重建图表实例；
- 后续大数据量可把白名单计算迁到 Web Worker，协议保持 `ValuePoint[]` 不变。

## 6. 试用步骤

1. 打开 K 线终端；
2. 点击「指标设置」；
3. 在「自定义指标」中确认 `EMA 6` 已启用；
4. 修改周期为 6、颜色为黄色；
5. 点击「完成」；
6. 图表主图出现一条 `EMA6` 均线。

## 7. 测试

```bash
cd web
pnpm typecheck
pnpm test
pnpm build
```

覆盖：

- 默认包含 `ema-6`；
- `customIndicator(EMA6)` 与 `ema(bars, 6)` 一致；
- 非法周期/颜色/kind 被过滤；
- 现有 MA/BOLL/MACD/RSI 与本地时间逻辑不回归。

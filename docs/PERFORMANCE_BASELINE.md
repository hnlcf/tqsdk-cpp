# 性能基线与 P7.5 优化

本文记录 2026-11-10 在开发机上的基线，用于后续回归比较；不是跨机器 SLA。
命令都可以重复执行，结果写入 CI/本地 artifacts。

## 1. 环境

```text
Linux fedora 7.2.11 x86_64
Python 3.14.8 / Node v26.13.0 / pnpm
clang++ C++20
构建：build-ui（RelWithDebInfo，未开 sanitizer）
```

## 2. WASM 指标 ABI

脚本直接加载 `web/public/plugins/*/*.wasm`，5000 根 K 线预热后执行 100 次：

```bash
node scripts/bench_wasm_plugins.mjs
```

| 插件 | ms/次（5000 根） | 次/秒 |
|---|---:|---:|
| atr | 0.0432 | 23,162 |
| boll | 0.0232 | 43,039 |
| cci | 0.0851 | 11,746 |
| donchian | 0.2074 | 4,821 |
| ema6 | 0.0677 | 14,779 |
| kdj | 0.0901 | 11,102 |
| ma | 0.0475 | 21,071 |
| macd | 0.0373 | 26,778 |
| obv | 0.0112 | 89,622 |
| rsi | 0.0273 | 36,695 |
| vwap | 0.0150 | 66,736 |

结论：11 个插件全量计算约 0.65ms/5000 根，WASM 数值计算不是当前前端瓶颈。
瓶颈更可能在 Worker 消息传递、图表 setData、DOM/网络序列化和行情频率。

## 3. HTTP / WebSocket

对一个运行中的 node 执行：

```bash
python3 scripts/bench_http_ws.py \
  --base http://127.0.0.1:18081 \
  --symbol SHFE.rb2701 \
  --requests 30 \
  --ws-seconds 2
```

2026-11-10 结果（本地回环，30 次/接口，复用 HTTP keep-alive）：

| 接口 | p50 | p95 | max |
|---|---:|---:|---:|
| `/healthz` | 0.059ms | 0.315ms | 1.23ms |
| `/quote/{symbol}` | 0.073ms | 0.102ms | 0.20ms |
| `/kline/{symbol}/{duration}` | 2.56ms | 4.09ms | 4.22ms |
| `/trading-snapshot` | 0.99ms | 1.52ms | 1.68ms |

WebSocket 首帧 snapshot：15.09ms；静默行情下 2s 收 10 条消息（约 5 条/s）。首帧包含订阅 +
快照构建，后续 diff 频率主要取决于行情活跃度。

## 4. 两进程短 soak

```bash
./scripts/soak_long_job.sh --duration 5 --out-dir /tmp/soak_ws_test
```

5 秒结果：

```json
{
  "duration_s": 5.0,
  "http_ok": 34739,
  "http_err": 0,
  "ws_messages": 5406,
  "ws_err": 0,
  "order_ok": 1,
  "order_err": 0,
  "reconnects": 0,
  "node_max_rss_kb": 34008,
  "node_max_fds": 19,
  "node_max_threads": 5,
  "gw_max_rss_kb": 26036,
  "gw_max_fds": 10,
  "gw_max_threads": 4,
  "fatal": ""
}
```

该脚本同时压 REST、WS 订阅/ping、周期性下单/撤单和网关重启；`--duration 86400`
或 `259200` 即 24h/72h。长稳必须用专用环境执行，不能把 5s 结果当成 24h 结论。

## 5. 已实施的优化（P7.5 增量）

- WASM 指标：每个插件独立 Worker，输入输出 transferable `Float64Array`；
- 图表计算 120ms debounce，避免每个 tick 全量重算；
- **全输出隐藏的插件跳过计算**，减少不可见指标 CPU；
- **按 chart + bars + 参数的输出缓存**：只改颜色/线型/显隐时复用上次数值，
  不重新执行 WASM；
- 多账户 WS 每个账户独立 hub，避免一个慢账户拖住其他账户；
- 自选/右侧栏/持仓表尺寸持久化，避免反复布局抖动。

## 6. 后续评估

- IPC MessagePack / 共享内存：先以 24h soak 的 CPU/内存/延迟分布为依据，不先改协议；
- WS 二进制帧：K 线和 tick 的 JSON 解析是下一候选；
- 指标增量计算：当前 WASM 全量已足够快，优先优化 Worker 传输/图表更新；
- 前端虚拟列表：当自选/持仓行数达到数千行再引入；
- `scripts/bench_wasm_plugins.mjs` / `scripts/bench_http_ws.py` 作为回归记录，
  不设成波形不稳定的硬性 CI 门槛。

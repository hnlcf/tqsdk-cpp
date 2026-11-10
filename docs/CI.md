# 本地 CI 与 sanitizer

## 1. 回归门禁

```bash
./scripts/ci_local.sh
```

顺序：

1. 构建 `tqsdk_tests` / `tqsdk-gw` / `tqsdk-node`；
2. CTest 全量；
3. Web `pnpm typecheck`、`pnpm test`、`pnpm build`；
4. `node scripts/check_wasm_plugins.mjs` 检查所有 WASM ABI。

可选：

```bash
./scripts/ci_local.sh --asan
./scripts/ci_local.sh --tsan
./scripts/ci_local.sh --ubsan
./scripts/ci_local.sh --e2e
```

## 2. ASan / TSan

```bash
./scripts/p6_asan_job.sh   # build-p6-asan + ctest
./scripts/tsan_job.sh      # artifacts/tsan/build + ctest
```

日志：

```text
artifacts/p6_campaign_20261109/p63_asan_build.log
artifacts/tsan/tsan.log
```

2026-11-09 当前结果：ASan 338/338、TSan 338/338、UBSan 338/338（2 项 TqMdNetwork 网络测试按环境跳过；
新增 Tick UI 与 WASM ABI 测试后 Web 为 16 项，另有 WASM 插件 ABI 脚本）。

## 3. 短 soak

```bash
python3 scripts/soak_two_process.py \
  --duration 60 \
  --reconnect-interval 10 \
  --order-interval 1 \
  --load-threads 2 \
  --gw-bin ./build-ui/tqsdk-gw \
  --node-bin ./build-ui/tqsdk-node \
  --out artifacts/soak_short/soak.csv
```

短 soak 会覆盖并发 REST、WS、SimTd 下单/撤单和 gw 重启重连；24h soak 使用默认
`--duration 86400`。CI 只跑短版本，长稳在专用环境执行。

## 4. 说明

- sanitizer 构建独立目录，不污染 `build-ui`；
- CI 脚本不包含凭据；真实 SimNow/CTP 测试另行使用 `run/live` 配置；
- Playwright 使用系统 Chrome（`TQSDK_CHROME_PATH` 可覆盖），`--e2e` 需要 node
  18081 已启动；测试只读，不下单。
- 版本/包名由 CMake `version.h.in` 注入，`tqsdk-node --version` 和
  `/healthz` 可快速确认构建版本。

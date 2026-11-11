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

2026-11-11 当前结果：认证/审计/部署检查已加入 CI；ASan/TSan/UBSan 358/358（本轮末重跑）；新增动态账户 hub 移除、基础画线、28 项 Web 测试和多账户 WS、
账户级 task/risk、IPC/WS fuzz 后普通 CTest 为 359 项通过（含 Python 绑定）（2 项 TqMdNetwork
网络测试按环境跳过）；Web Vitest 28 项，Playwright E2E 1 项，另有 WASM 插件 ABI 脚本、日线回测 helper 单测和 Python 策略/绑定单测（12 项）。

### GitHub Actions

`.github/workflows/ci.yml` 提供两条独立流水线：

- `cpp`：Ubuntu + vcpkg（`ci/vcpkg.json` 只装核心依赖，不拉 Qt）构建并跑 CTest；
- `web`：pnpm/typecheck/test/build + WASM ABI 脚本。

`ci/vcpkg.json` 是 CI 专用 manifest，避免 CI 为了核心测试安装完整 Qt/UI 依赖；
UI/桌面构建仍在本地/发布环境执行。GitHub 流水线尚未在本机实际触发，不能把“workflow
文件已存在”写成“远程 CI 已通过”。

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

## 4. Fuzz 与长稳

- `tests/ipc/protocol_fuzz_test.cpp`：确定性随机 IPC body/JSON/MessagePack 输入；
- `tests/serve/ws_fuzz_test.cpp`：随机 WS 客户端/服务端文本和深嵌套输入；
- 两者在普通 ctest 和 sanitizer 构建中都会执行；
- 24h/72h 两进程长稳：

```bash
./scripts/soak_long_job.sh --duration 86400
./scripts/soak_long_job.sh --duration 259200
```

输出包含 `soak_*.json`、CSV 和进程日志；真实 24h/72h 必须在专用环境执行并归档，
短 soak 不能替代。

### 双账户进程级验收（可选）

```bash
./scripts/multi_account_acceptance.py --bin-dir build-ui
```

需要 `websockets` Python 包；脚本自动启动两个 Sim gw + node 并清理进程。它不是
真实 CTP 柜台验收，边界见 [MULTI_ACCOUNT_ACCEPTANCE.md](MULTI_ACCOUNT_ACCEPTANCE.md)。

### 24h/72h 自动化

长稳 workflow 见 [SOAK_AUTOMATION.md](SOAK_AUTOMATION.md)：必须使用带
`tqsdk-soak` 标签的 self-hosted runner，GitHub 托管 runner 不支持 24h/72h。

## 5. 说明

- sanitizer 构建独立目录，不污染 `build-ui`；
- CI 脚本不包含凭据；真实 SimNow/CTP 测试另行使用 `run/live` 配置；
- Playwright 使用系统 Chrome（`TQSDK_CHROME_PATH` 可覆盖），`--e2e` 需要 node
  18081 已启动；测试只读，不下单。
- 版本/包名由 CMake `version.h.in` 注入，`tqsdk-node --version` 和
  `/healthz` 可快速确认构建版本。

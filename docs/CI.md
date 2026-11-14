# CI 与 Sanitizer

## 1. 本地回归门禁

```bash
./scripts/ci_local.sh
```

顺序：

1. 构建 `tqsdk_tests` / `tqsdk-gw` / `tqsdk-node`；
2. CTest 全量；
3. Web `pnpm typecheck`、`pnpm test`、`pnpm build`；
4. `node scripts/check_wasm_plugins.mjs`；
5. `./scripts/verify_deploy.sh`。

可选：

```bash
./scripts/ci_local.sh --asan
./scripts/ci_local.sh --tsan
./scripts/ci_local.sh --ubsan
./scripts/ci_local.sh --e2e
```

## 2. Sanitizer

```bash
./scripts/asan_job.sh
./scripts/tsan_job.sh
./scripts/ubsan_job.sh
```

默认输出：

```text
artifacts/asan/asan.log
artifacts/tsan/tsan.log
artifacts/ubsan/ubsan.log
```

可用 `TQSDK_ASAN_BUILD_DIR` / `TQSDK_TSAN_BUILD_DIR` /
`TQSDK_UBSAN_BUILD_DIR` 覆盖构建目录。sanitizer 构建使用独立目录，不污染
`build-ui`。

## 3. 当前本地结果

- CTest：384/384（2 项 TqMdNetwork 网络测试按环境跳过）；
- Python：21/21；
- Web：typecheck/test/build 通过；Playwright E2E 1 项；
- 本地 Clang 与 GCC 构建均通过；
- `check_secrets.py`：PASS。

远程 CI 结果以 GitHub Actions 实际运行状态为准。

## 4. GitHub Actions

`.github/workflows/ci.yml`：

- `cpp`：Ubuntu 24.04 + GCC 14 + vcpkg 核心依赖，构建并跑 CTest；
- `web`：pnpm/typecheck/test/build + WASM ABI 检查；
- `python`：uv sync、策略注册表和 Python 绑定测试。

GCC 14 直接提供 C++23 `std::expected`，避免旧 clang/libstdc++ 组合的兼容问题。
CI 不安装 Qt/UI 依赖，Qt UI 不属于当前前端路线。

## 5. 短 soak、Fuzz 与多账户

短 soak：

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

Fuzz：

- `tests/ipc/protocol_fuzz_test.cpp`
- `tests/serve/ws_fuzz_test.cpp`

两者在普通 CTest 和 sanitizer 构建中执行。

多账户进程级测试：

```bash
./scripts/multi_account_acceptance.py --bin-dir build-ui
```

长稳和 CTP 交易时段 soak 见 [SOAK_PERF.md](SOAK_PERF.md)；真实 CTP 验收见
[SIMNOW_ACCEPTANCE.md](SIMNOW_ACCEPTANCE.md)。

## 6. 边界

- CI 脚本不包含凭据；真实 SimNow/CTP 测试使用本地 `run/live` 配置；
- Playwright 使用系统 Chrome（`TQSDK_CHROME_PATH` 可覆盖），`--e2e` 需要 node
  18081 已启动；测试只读，不下单；
- 24h/72h 必须使用带 `tqsdk-soak` 标签的 self-hosted runner，短 soak 不能替代；
- 版本由 CMake 注入，`tqsdk-node --version` 和 `/healthz` 可确认构建版本。

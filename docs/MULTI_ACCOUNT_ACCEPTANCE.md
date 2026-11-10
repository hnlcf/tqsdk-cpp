# 双账户进程级验收

## 1. 目的

`tests/serve/multi_account_http_test.cpp` 验证同一进程内的多账户路由；本文脚本进一步
验证**两个独立 `tqsdk-gw` 进程 + 一个 `tqsdk-node`** 的真实进程拓扑：

```text
gw A (Sim) ──IPC──┐
                  ├── tqsdk-node ── HTTP/WS ── client
gw B (Sim) ──IPC──┘
```

覆盖：

- `/accounts`：A 为默认，A/B 都注册；
- `/order?account=A|B` 下单到各自 Sim 网关；
- `/positions?account=A|B` 持仓隔离；
- `/task/target?account=B` 创建任务，`/tasks` 只在该账户可见；
- `/risk/rules?account=B` 只影响 B，A 的规则数仍为 0；
- `/stream?account=A|B` 各自返回本账户持仓快照。

## 2. 运行

```bash
./scripts/multi_account_acceptance.py --bin-dir build-ui
```

要求：

- `build-ui/tqsdk-gw`、`build-ui/tqsdk-node` 已构建；
- Python 3.10+；
- `websockets` 包（仅 WS 验收需要；`python3 -m pip install websockets`）。

脚本自动分配端口、写临时 TOML、启动/清理进程；`--keep` 保留日志和配置。

## 3. 2026-11-10 结果

```json
{
  "ok": true,
  "accounts": ["A", "B"],
  "default": "A"
}
```

验收同时发现并修复了一个真实的生命周期缺陷：

- `MultiAccountManager::close()` 原先立即销毁额外账户的 `TqApi`；
- 此时 node 尚未 drain `io_context`，排队回调仍持有该对象，进程退出出现
  `double free or corruption`；
- 现在关闭的 `TqApi` 会 retired 到 manager 内部容器，直到 manager 析构
  （调用方 drain io 之后）才释放；新增
  `MultiAccount.CloseRetiresApisUntilManagerDestruction` 单测。

## 4. 边界

本脚本使用 Sim 网关，不涉及真实 CTP 账号/柜台；真实双 SimNow/CTP 账户验收仍需
在具备账户和交易时段的环境单独执行，不能把本脚本结果当作真实柜台验收。

# SimNow 双账户验收

本流程使用两个独立 SimNow/CTP 账户验证：

```text
gw A (CTP TD + Tq MD) ──IPC──┐
                              ├── tqsdk-node ── HTTP/WS
gw B (CTP TD + Tq MD) ──IPC──┘
```

默认只做**只读验收**，不会下单、撤单或修改账户状态。

## 1. 凭据文件

不要把密码写进 TOML 或提交到仓库。创建两个 0600 文件：

```text
config/simnow_a.env
config/simnow_b.env
```

格式：

```text
CTP_BROKER_ID=...
CTP_USER_ID=...
CTP_PASSWORD=...
CTP_APP_ID=...
CTP_AUTH_CODE=...
CTP_TD_FRONT=tcp://...
CTP_MD_FRONT=tcp://...
```

`config/*.env` 已在 `.gitignore` 中忽略。

## 2. 只读验收

```bash
python3 scripts/simnow_dual_acceptance.py
```

脚本会：

- 生成临时 0700 工作目录和 0600 TOML；
- 启动两个独立 `tqsdk-gw`：
  - A：`14331`
  - B：`14332`
- 启动一个 `tqsdk-node`：`18082`；
- 默认账户为 A，额外账户为 B；
- 等待两个账户都 ready 且账户资金字段可用；
- 检查：
  - `/accounts` 包含 A/B；
  - `/account?account=A|B`
  - `/positions?account=A|B`
  - `/orders?account=A|B`
  - `/trades?account=A|B`
  - `/stream?account=A|B` 返回账户快照；
- 退出时清理临时配置和进程；
- 把日志复制到 `artifacts/simnow_dual/<stamp>_logs/`。

报告写入：

```text
artifacts/simnow_dual/simnow_dual_<stamp>.json
```

## 3. 交易时段

SimNow 柜台在非交易/午休/结算窗口可能拒绝连接或登录。此时脚本会等待到超时，
应保留 failure JSON 和日志，等柜台恢复后重跑。不要把“ID 出现但 ready=false”
记作 PASS。

## 4. 写验收

本脚本默认不写单。真实下单/撤单必须另行确认并明确：

- 账户；
- 合约；
- 方向/开平；
- 价格类型；
- 手数。

不要用双账户脚本的“账户存在”结果替代真实下单路由、持仓隔离和风控验收。

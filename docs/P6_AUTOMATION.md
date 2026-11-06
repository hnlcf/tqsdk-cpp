# P6 自动测试编排

> 本文件描述 2026-11-09（周一）09:00 自动启动的 P6.1/P6.2/P6.3 测试。
> 真实交易日验证一旦开始，脚本不再需要人工操作；结果写入
> `artifacts/p6_campaign_20261109/`。

## 1. 时间表

```text
2026-11-09 09:00  P6 总启动
                  ├─ P6.1 5 日 CTP session soak（只读，后台）
                  ├─ P6.3 ctest
                  ├─ P6.3 ASan build + ctest（后台）
                  ├─ P6.3 72h SimTd soak（后台）
                  └─ P6.2 SimNow 验收 + 写测试
2026-11-09 13:35  P6.2 部分成交探针
2026-11-10 09:35  P6.2 平昨验收
2026-11-13 15:20  最后一个每日对账（循环任务）
2026-11-14 10:00  收尾：恢复测试 + 总报告
```

`atd` 已启用。所有后台任务用 `setsid` 脱离当前会话；机器重启后需要重新
执行第 5 节的重新排期命令。

## 2. 组件

| 脚本 | 作用 |
|---|---|
| `scripts/p6_campaign_start.sh` | 周一总启动：P6.1/P6.2/P6.3 |
| `scripts/ctp_week_soak.py` | 5 个日历日的只读 CTP session soak |
| `scripts/p6_daily_reconcile_loop.py` | 每日 09:10/15:20 对账 |
| `scripts/p6_partial_fill_job.sh` | 13:35 期权部分成交探针 |
| `scripts/p6_close_yesterday_job.sh` | 周二平昨 |
| `scripts/p6_asan_job.sh` | ASan 构建 + core ctest |
| `scripts/p6_week_followup.sh` | 周末恢复测试 + 汇总 |
| `scripts/p6_report.py` | 生成 `report.json` / `report.md` |

## 3. 每次运行留下的产物

```text
artifacts/p6_campaign_20261109/
  controller.log
  p61_week/
    ctp_week_*.jsonl
    ctp_week_*.summary.json
    ctp_week_*.report.json
    ctp_week_*.gw.log
    ctp_week_*.node.log
  p62_acceptance_readonly.json
  p62_acceptance_write.json
  p62_reconcile/
  p62_partial_fill.txt
  p62_close_yesterday.log
  p63_soak_72h.out / .csv
  p63_asan_build.log
  p63_fuzz_skip.txt
  p6_recovery.log
  report.md / report.json
```

`artifacts/` 已在 `.gitignore` 中，不提交账号、订单和流量文件。

## 4. 已知跳过/边界

- **P6.3 fuzz**：当前没有独立 libFuzzer target；已运行 malformed protocol/
  frame ctest，但不会把 fuzz 里程碑伪造成 PASS，记录在
  `p63_fuzz_skip.txt`。
- **P6.3 72h soak**：周一启动，周四结束；24h 节点可查看同一 CSV 的前 24h。
- **P6.2 order-in-flight fault**：需要真实 quote，且会替换 P6.1 的 gateway；
  安排在下一个独立交易日，执行说明见
  `p62_order_fault_deferred.txt`，不在周 soak 期间强行运行。
- **P6.2 平昨**：如果周一开盘没有可用历史仓，周二脚本返回非零并记录，
  不会伪造 PASS。
- **P6.1 结算断线**：`expected_disconnect=true` 的 gap 计入 expected，不算故障。

## 5. 查看/取消

```bash
atq
atrm <job-id>

tail -f artifacts/p6_campaign_20261109/controller.log
cat artifacts/p6_campaign_20261109/report.md
```

重新排期（如果需要）：

```bash
./scripts/p6_schedule.sh
```

## 6. 人工确认点

这些是**测试结束后**查看结果，不需要在周一 09:00 值守：

1. `report.md` 是否 PASS/FAIL；
2. `p61_week/*.summary.json` 的 `unexpected_gaps` 是否为 0；
3. `p62_acceptance_write.json` 是否完成并清理；
4. `p62_partial_fill.txt` 是否得到 PARTIAL/FULL/NO_FILL（都不是产品失败，
   但必须如实记录）；
5. `p62_close_yesterday.log` 是否在次日完成；
6. ASan / 72h soak 是否完成；
7. 若失败，按日志保留现场，不重跑覆盖原产物。

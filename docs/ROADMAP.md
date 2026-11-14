# Roadmap

本文只列当前仍需要推进的工作。历史路线和已删除方案用 Git 历史追溯。

## 1. 当前基线

已具备：

- Tq 行情 / CTP 真实交易 / Sim 交易；
- `tqsdk-gw` + `tqsdk-node` 两进程、IPC seq/replay；
- REST/WS 数据面、多账户路由、账户级 task/risk；
- TQSDK Board Web（SolidJS/pnpm/Lightweight Charts）；
- WASM 指标、K 线缓存、合约目录、节假日、连续合约；
- C++ BacktestEngine、Python `tqsdk_py`、实盘策略适配器；
- 高级报单、TargetPosTask/InsertOrderTask、RiskManager；
- 认证/审计/systemd/TLS 部署模板、DEB/RPM/TGZ/容器；
- TQ OTG 全部 aid 的结构化消息与 TqTdGateway；
- error handling 已统一到 `std::expected` / noexcept 边界。

## 2. 当前优先级

1. **TQ TD 真实交易验收**：交易时段完成 TqTdGateway 登录后的 account/positions
   截面、报单、撤单、成交回报和断线恢复；只读握手已通过，真实报单待执行。
2. **CTP 连续交易日验证**：完成跨结算日的 5 日 session soak、每日对账和
   平今/平昨矩阵；当前 7 天 soak 仍在运行，完成前不宣称通过。
3. **前端 E2E/视觉回归**：补齐账户切换、断线重连和视觉基线；现有 E2E 覆盖
   搜索/K 线/下单板等基础路径。
4. **Windows CTP**：编译、SDK 打包和 SimNow 验证。
5. **性能/协议**：WS binary/MessagePack、虚拟列表按 profile 结果决定；
   IPC 共享内存暂缓。

## 3. 已完成的重点能力

- TQ 协议消息层与 TqTdGateway：`req_login`、`confirm_settlement`、
  `peek_message`、order/cancel、trade diff/snapshot、重连；
- WebSocket 多账户过滤、账户级 task/risk；
- 多账户 TqApi/回测、Tick/DataSeries/期权分析；
- WASM 指标插件和 Worker 沙箱；
- 生产化入口：token 认证、日志脱敏、JSONL 审计、systemd、TLS 反向代理模板；
- GitHub Actions cpp/web/python 三个 job 与本地 sanitizer 回归。

## 4. 验收定义

每项完成必须同时满足：

1. 代码边界清晰，依赖方向不逆转；
2. 单元/集成/e2e 回归通过；
3. 关键失败路径有测试或可重复脚本；
4. canonical docs 同步更新；
5. 不把预期断线/撮合限制伪装成 PASS。

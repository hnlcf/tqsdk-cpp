# Roadmap

本文只列当前仍然有意义的工作；历史路线已归档到
[archive/ROADMAP_LEGACY.md](archive/ROADMAP_LEGACY.md)。

## 1. 当前基线

已具备：

- Tq 行情 / CTP 交易 / Sim 交易；
- gw/node 两进程 + IPC seq/replay；
- REST/WS 数据面；
- Qt UI + Python 指标 sidecar；
- 合约目录、节假日、K 线缓存；
- SimNow CTP 验收、24h SimTd soak、CTP session monitor；
- 订单价格归一化、平今/平昨、命令幂等、故障恢复；
- 本轮模块重构：WsEntityFilter、SnapshotService、GatewaySubscriptionBook、
  CommandAckCache、DiffMerge/CTP/gateway/http 分文件。

## 2. 下一优先级（P6）

### P6.1 CTP 交易会话可靠性

- `CtpQueryPipeline` 从 `CtpTdGateway` 独立：
  - query generation/watchdog；
  - 查询队列；
  - timeout/retry；
  - callback -> pipeline -> snapshot。
- 连续 5 个交易日 CTP session soak（真实交易日，不压缩）。
- 跨结算重启对账、盘中 CTP front 断线、查询延迟分布。

### P6.2 交易验收矩阵

- SHFE/CZCE/DCE 平今/平昨交叉矩阵；
- 部分成交/部分撤单（需低流动性窗口）；
- 开盘/收盘/午休边界；
- 订单在途断线、ack 丢失、重复 rid；
- 多账户/多 node 共享 gw。

### P6.3 长稳与故障

- 24h/72h soak 归档自动化；
- ASan/TSan/UBSan CI；
- IPC/WS fuzz：
  - 半帧/超大长度/非法 JSON/重复 seq/epoch 回绕；
- CTP 查询 watchdog 抖动注入；
- 内存/FD/线程基线告警。

## 3. UI 与可观测性

- `WebSocketHub::Client` reader/writer 与 Hub 文件拆分；
- UI 真实 4K Wayland 自动交互测试；
- 高 DPI 图标/字体/表格回归；
- 订单部分成交/撤单失败更完整提示；
- 图表指标副图、图例编辑；
- 指标 sidecar 打包、依赖检查、插件权限边界。

## 4. 架构与性能

- `CtpQueryPipeline`、`MarketSnapshotService` 已完成的服务边界继续收敛；
- HTTP 路由按 catalog/market/trading 分组，handler 不再直接读 DataManager；
- IPC 可选 MessagePack/共享内存快路径；
- WS 二进制行情帧评估；
- KlineStore 仅 node 构建依赖；
- core pch 与 UI pch 分离；
- multi-account/路由策略。

## 5. 平台

- Windows CTP 编译、SDK 打包、SimNow 实盘验证；
- Linux 打包/安装目录；
- 服务化部署（systemd/supervisor）；
- TLS + 认证（当前 node 只能 loopback）；
- 生产日志脱敏和审计。

## 6. 暂缓/不做

- Web 前端：当前暂停，Qt UI 优先；
- 完整交易所官方交易日历服务：当前使用 shinny 节假日 + 可配置 session；
- 共享内存 IPC：先完成稳定性和多账户路由再评估。

## 7. 验收定义

每项完成必须同时满足：

1. 代码边界清晰，依赖方向不逆转；
2. 单元/集成/e2e 回归通过；
3. 关键失败路径有测试或可重复脚本；
4. canonical docs 同步更新；
5. 不把预期断线/撮合限制伪装成 PASS。

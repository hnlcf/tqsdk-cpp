# 高可用与多网关部署方案

> 目标：避免“两个 gateway 同时向同一 CTP 账户发单”，同时让 node（Web 客户端后端）
> 在 gateway 进程重启、机器切换时尽快恢复。

## 1. 现状与硬约束

- `tqsdk-gw` 是 CTP 会话的唯一持有者；CTP 同一账户/同一会话的报单序号
  （`FrontID + SessionID + OrderRef`）必须由单一写入者维护。
- 不能让两个 active gateway 同时对同一 CTP 账户报单/撤单。
- node 可以同时连接多个 gateway，但交易写入必须只路由到一个 leader。
- CTP 本身是交易状态的权威来源；node 的 `TradeSnapshot` 用于断线后的对账，
  不是独立账本。

## 2. 推荐部署形态

### 2.1 单活 gateway + supervisor（当前推荐）

```text
              +------------------+
              |  supervisor      |
              |  (systemd/        |
              |   k8s/liveness)  |
              +--------+---------+
                       |
              +--------v---------+
              | tqsdk-gw (CTP)   |  <--- 唯一 active
              +--------+---------+
                       | IPC
              +--------v---------+
              | tqsdk-node       |  <--- Web/REST 入口
              +------------------+
```

- supervisor 只启动一个 gw；
- gw 崩溃后由 supervisor 重启，node 通过 IPC 自动重连并发送 Resume；
- node `/readyz` 在交易快照完成前返回 503；
- 订单幂等映射落盘在 `flow_path/ctp_orders_<user>.json`，gw 重启后相同 rid
  不会再次报单。
- 这是当前代码已经支持的形态。

### 2.2 冷备 gateway

```text
gw-active  --- CTP session A
gw-standby --- 已启动但不登录/不报单
```

- standby 只做进程预热；只有 leader lease 获得后才登录 CTP；
- 切换步骤：
  1. active 停止接单（node 将命令返回“正在切换”）；
  2. active 释放 CTP 会话；
  3. standby 获取 lease，登录/结算/查询；
  4. node 重连到新 active，收到 TradeSnapshot；
  5. 恢复接单。
- 需要额外的 **lease/选主**：systemd、etcd、Consul、或简单的文件锁 + 心跳。
- 不建议两个 gw 同时登录同一 CTP 账户：即使一个只查询，也可能在断线重连时
  产生不可控的私有流恢复。

### 2.3 多账户/多经纪商

- 一个 gateway 只绑定一个 CTP 账户；
- 不同账户启动不同 gw，node 按账户路由；
- 未来可在 node 增加 `account -> gateway` 路由表，但不要在一个进程里混多个
  CTP Trader API 会话而不做隔离。

### 2.4 多 node

- 多个 node 可以共享一个 gw（当前 `GatewayIpcServer` 支持多连接/广播）；
- 每个 node 有独立 seq/epoch、Resume 和订阅释放；
- node 是无状态读服务时可直接水平扩容；写入口需要明确唯一客户端或幂等 rid。

## 3. 故障检测

建议组合使用：

- **进程存活**：supervisor / k8s liveness；
- **业务就绪**：node `/readyz`，要求：
  - IPC 已连接；
  - 首笔 `TradeSnapshot` 已应用；
  - 行情侧已收到数据（有 md 时）；
- **行情新鲜度**：记录最后 tick 时间，超过阈值视为 stale；
- **命令健康**：ack 超时率、拒单率、查询超时率。

## 4. 切换时的数据一致性

1. 新 gateway 登录后先结算确认；
2. 查询 account/position/order/trade；
3. node 收到 snapshot 后 `clear_trading()` 再应用；
4. 订单/成交 ID 通过持久化 `rid -> OrderRef/OrderSysID` 映射与 CTP 查询结果对齐；
5. 旧 gateway 的未完成命令：
   - 有 ack 缓存则重放 ack；
   - 无缓存但有持久化 rid 则返回 accepted 并触发对账；
   - 无映射则返回明确失败，不自动重发。

## 5. 未来可落地的代码改造

- 配置支持 gateway endpoint 列表和优先级；
- 增加轻量 lease 文件/租约接口；
- `GatewayIpcServer` 增加 `GetStatus`，暴露 CTP 登录、查询、最后 tick 时间；
- node `/metrics` 增加 gateway 维度标签；
- 发布脚本/helm chart/systemd unit。

## 6. 验收清单

- [ ] kill -9 active gw，node 在 N 秒内 `/readyz` 恢复 200；
- [ ] 重启窗口内重复 rid 不产生重复报单；
- [ ] 切换后订单/成交/持仓与 CTP 查询一致；
- [ ] 两个 node 同时连接，订阅/退订互不影响；
- [ ] 24h soak 无 fd/线程/RSS 线性增长；
- [ ] 主备切换演练有 runbook 和回滚步骤。

# 执行任务：TargetPosTask / InsertOrderTask

## 1. 设计

执行任务建立在 `TqApi` 的 update hook 上：

```text
md_loop / td_loop / wait_update
        │
        └─ run_update_hooks()
              ├─ TargetPosTask::on_update
              └─ InsertOrderTask::on_update
```

任务不直接持有网关，也不自行阻塞等待；它只通过 `TqApi`：

- `get_quote()` 读取行情；
- `data().positions` 读取净持仓；
- `insert_order()` 提交任务单；
- `cancel_order()` 撤销任务单；
- risk manager 在执行前统一检查。

因此任务和普通策略、node serve 共用同一个核心 strand，保持单线程数据所有权。

## 2. TargetPosTask

头文件：

```cpp
#include "tqsdk/trading/order_tasks.h"
```

用法：

```cpp
TargetPosTask task(api, "SHFE.rb2701",
                   TargetPosPriceMode::ACTIVE,
                   TargetPosOptions{.max_volume = 0});
task.set_target_volume(2);   // 目标净多 2 手
// 之后持续调用 api.wait_update()
task.set_target_volume(0);   // 目标净持仓 0
task.cancel();
```

### 2.1 语义

- target 是**净持仓**：正数多、负数空。
- 自动根据当前 long/short 和在途订单计算 delta。
- 需要增加净持仓时优先买平空头，再买开；需要减少时优先卖平多头，再卖开。
- 平仓使用 `CLOSE`，由网关按 SHFE/INE/GFEX 规则拆平今/平昨。
- 同一任务最多一笔在途报单，避免重复下单。
- `max_volume` 非 0 时拆单，每笔不超过该值。

### 2.2 价格模式

| 模式 | 买价 | 卖价 |
|---|---|---|
| `ACTIVE` | 卖一 | 买一 |
| `PASSIVE` | 买一 | 卖一 |
| `CUSTOM` | `price_provider` | `price_provider` |

行情不足或价格无效时，任务等待下一次更新，不发出错误价格。

### 2.3 生命周期

- 构造函数注册 update hook；
- `set_target_volume()` 会 `notify_update()` 唤醒等待；
- `cancel()` 注销 hook、撤销本任务活动委托；
- 析构自动 `cancel()`；
- 任务必须在 `TqApi` 生命周期内创建和销毁；API close 前应先销毁任务。

### 2.4 追价与边界

行情价格变化时，任务会撤销旧价单，并在旧单终态后的下一轮更新按新价重报
（Cancel/Replace）。撤单在途时暂停新开仓，避免重复委托。

当前边界：

- 追价以 quote 的 price_tick 为最小偏离判断；没有 quote 时不报单；
- 平今/平昨优先级由网关的 CLOSE 规则处理；
- 交易所最小开仓手数尚未强制校验；
- 后续增加任务执行统计（提交/成交/撤单）和可配置追价间隔。

## 3. InsertOrderTask

`InsertOrderTask` 持续补单，直到目标数量全部成交或重试次数耗尽。

```cpp
InsertOrderReq req;
req.instrument_id = "SHFE.rb2701";
req.direction     = Direction::BUY;
req.offset        = Offset::OPEN;
req.price_type    = PriceType::LIMIT;
req.limit_price   = 3000;
req.volume        = 3;

InsertOrderTask task(api, req, 20);
// 任务自动 start；也可显式 task.start()
```

- 订单终态后累计已成交量；
- 未全部成交则在后续 update 中补发剩余手数；
- `cancel()` 注销任务并撤销活动委托；
- `InsertOrderUntilAllTradedTask` 是对齐官方命名的别名类型。

## 4. REST 验收接口

node 额外提供 P0 验收接口（不是官方 TqApi JSON 协议）：

```text
POST /task/target
{
  "symbol": "SHFE.rb2701",
  "target_volume": 0,
  "mode": "ACTIVE",
  "max_volume": 0
}

DELETE /task/target/{task_id}

POST /task/order
{
  "order": { ...InsertOrderReq... }
}

DELETE /task/order/{task_id}

GET /tasks
```

这些接口主要用于 SimNow 验收和前端/运维探针。正式策略应优先使用 C++ `TqApi` + task 对象。

## 5. 测试

```bash
ctest --test-dir build-ui -R OrderTasks --output-on-failure
```

覆盖：

- TargetPosTask 开多/平多；
- 在途委托不重复下单；
- InsertOrderTask 补单至全成；
- RiskRule 在网关前拒绝订单。

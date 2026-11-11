# 连续合约与主连复权

## 1. 支持的符号

天勤行情中：

```text
KQ.i@DCE.jm    焦煤指数（加权指数连续）
KQ.m@DCE.jm    焦煤主连（换月时会产生跳空）
```

`node` 对这两类符号和其他合约一样订阅、缓存和提供 K 线。Web 自选可以直接输入
完整代码 `KQ.m@DCE.jm`。

## 2. 换月表

node 默认从：

```text
https://files.shinnytech.com/continuous_table.json
```

加载并缓存：

```json
{
  "DCE.jm": [
    ["20130322", "DCE.jm1309"],
    ["20130701", "DCE.jm1401"]
  ]
}
```

配置：

```toml
[continuous]
enabled = true
url = "https://files.shinnytech.com/continuous_table.json"
file = "continuous_table.json"
refresh_interval_sec = 86400
initial_delay_sec = 1
```

- 文件存在时先加载本地缓存；
- 后台线程按间隔刷新；
- 网络失败不阻塞 node 行情主链路。

## 3. K 线复权 API

只对：

- `KQ.m@...` 主连；
- `duration_ns = 86400000000000`（1d）；

支持：

```text
GET /kline/KQ.m@DCE.jm/86400000000000                # 原始
GET /kline/KQ.m@DCE.jm/86400000000000?adjust=forward # 前复权
GET /kline/KQ.m@DCE.jm/86400000000000?adjust=back    # 后复权
```

返回额外字段：

```json
{
  "adjust": "forward",
  "adjusted": true,
  "adjust_warnings": [],
  "bars": []
}
```

非 1d、非 `KQ.m@`、或换月表未准备好时返回原始 bars，不伪造复权结果。

## 4. 复权算法

对应换月日：

1. 从换月表找到 `from` / `to` 合约；
2. 分别加载两个合约的日线；
3. 取 `from` 在换月日前最后一个收盘价与 `to` 在换月日后第一个收盘价；
4. 比例 `ratio = to_close / from_close`；

- **前复权**：最新价格不变，换月之前的 OHLC 乘以 `ratio`；
- **后复权**：历史价格不变，换月及之后的 OHLC 除以 `ratio`；
- 合约日线缺失时跳过该换月，返回 warning，不猜测价格。

实现和单元测试：

```text
include/tqsdk/data/continuous_table.h
src/data/continuous_table.cpp
tests/data/continuous_table_test.cpp
```

## 5. Web 行为

前端对 `KQ.m@` 且 `1d`：

- 加载原始 K 线；
- 同一轮请求前复权和后复权；
- 图表工具栏出现“主连”选择：

```text
原始 / 前复权 / 后复权
```

- 切换时直接使用已取回的三份数据，不重复请求；
- 1d 以外周期不显示该选项；
- `KQ.i@` 指数不显示该选项（指数本身没有主连换月跳空）。

## 6. 已知边界

- 复权需要具体换月合约的日线；Tq MD 当前单次返回的历史条数有限，较长历史
  可能需要本地 K 线缓存预热；
- 若某个换月合约已退市且远端拿不到历史，返回 warning，图表仍显示原始 bars；
- 复权基于收盘价比例，不等同于交易所官方前复权因子；策略研究应记录所用
  mode 和数据版本。

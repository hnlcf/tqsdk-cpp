# 常量与配置中心

## 1. 原则

- 运行期可调参数放 TOML/env，不放头文件；
- 代码内共享的默认值、容量、协议上限集中到
  `include/tqsdk/constants.h`；
- 常量使用 `inline constexpr`，避免 ODR 和重复定义；
- 新增默认值先判断是否有跨模块共享价值，再决定是否进入 constants。

## 2. 当前分组

| 分组 | 常量 |
|---|---|
| 数据窗口 | `kDefaultTickCapacity`、`kDefaultKlineViewWidth` |
| K 线缓存 | `kDefaultKlineMaxBars`、`kDefaultKlineFlushMs`、`kDefaultKlinePendingBars`、`kDefaultKlineFreshSec` |
| Tick 缓存 | `kDefaultTickMaxPerSeries`、`kDefaultTickFlushMs`、`kDefaultTickPending` |
| HTTP/WS | `kDefaultWsMaxClients`、`kDefaultWsMaxQueue`、`kMaxHttpBodyBytes`、`kDefaultWsHeartbeatSec` |
| live 端点 | `kDefaultGwPort`、`kDefaultHttpPort` |
| 行情缓存 | `kDefaultMarketCacheFile` |
| 天勤地址 | `kTqAuthUrl`、`kTqNameServiceUrl`、`kTqSymbolUrl`、`kTqHolidayUrl`、`kTqMdDefaultHost/Port/Path` |
| 包名/版本 | `tqsdk::kPackageName`（`tqsdk-cpp`）、`kPackageSlug`（`tqsdk_cpp`）、`kPackageAliases`、`kVersion`、`kVersionMajor/Minor/Patch` |

包名和版本由 `include/tqsdk/version.h.in` 在 CMake configure 阶段生成到
`build/generated/tqsdk/version.h`；`tqsdk-node --version` / `tqsdk-gw --version`
和 `/healthz` 使用同一份值。

## 3. 使用示例

```cpp
#include "tqsdk/constants.h"

TickSeries series("SHFE.rb2701", tqsdk::constants::kDefaultTickCapacity);
```

配置中的 `[market_cache].file` 优先决定 SQLite 文件；只有显式设置
`[kline_cache].file` / `[tick_cache].file` 时才覆盖共享文件。

## 4. 边界

`constants.h` 只放“默认值/上限”，不放用户凭据、合约代码、运行目录、
交易时段等业务数据；这些仍由 TOML、环境变量或 TradingCalendar 管理。

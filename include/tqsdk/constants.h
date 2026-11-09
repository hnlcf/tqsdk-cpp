#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace tqsdk::constants {

// ---- 数据窗口/历史 ----
inline constexpr int         kDefaultTickCapacity     = 200;
inline constexpr int         kDefaultKlineViewWidth   = 200;
inline constexpr int         kDefaultKlineMaxBars     = 2'000;
inline constexpr int         kDefaultKlineFlushMs     = 250;
inline constexpr std::size_t kDefaultKlinePendingBars = 50'000;
inline constexpr int         kDefaultKlineFreshSec    = 900;

// ---- tick 持久化 ----
inline constexpr std::size_t kDefaultTickMaxPerSeries = 1'000'000;
inline constexpr int         kDefaultTickFlushMs      = 250;
inline constexpr std::size_t kDefaultTickPending      = 50'000;

// ---- HTTP / WebSocket ----
inline constexpr std::size_t kDefaultWsMaxClients   = 256;
inline constexpr std::size_t kDefaultWsMaxQueue     = 1'024;
inline constexpr std::size_t kMaxHttpBodyBytes      = 1u << 20; // 1 MiB
inline constexpr int         kDefaultWsHeartbeatSec = 30;

// ---- live 默认端点（脚本/配置文档共用；C++ 服务不强制） ----
inline constexpr std::uint16_t    kDefaultGwPort          = 14'330;
inline constexpr std::uint16_t    kDefaultHttpPort        = 18'081;
inline constexpr std::string_view kDefaultMarketCacheFile = "market_cache.sqlite3";

} // namespace tqsdk::constants

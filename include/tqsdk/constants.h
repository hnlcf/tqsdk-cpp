#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "tqsdk/version.h"

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

// ---- 天勤固定服务地址/协议默认值 ----
inline constexpr std::string_view kTqAuthUrl         = "https://auth.shinnytech.com";
inline constexpr std::string_view kTqNameServiceUrl  = "https://api.shinnytech.com/ns";
inline constexpr std::string_view kTqBrokerUrlPrefix = "https://files.shinnytech.com";
inline constexpr std::string_view kTqSymbolUrl =
    "https://openmd.shinnytech.com/t/md/symbols/symbol.json";
inline constexpr std::string_view kTqHolidayUrl =
    "https://files.shinnytech.com/shinny_chinese_holiday.json";
inline constexpr std::string_view kTqMdDefaultHost = "free-openmd.shinnytech.com";
inline constexpr std::string_view kTqMdDefaultPort = "443";
inline constexpr std::string_view kTqMdDefaultPath = "/t/md/front/mobile";
inline constexpr std::string_view kTqClientId      = "shinny_tq";
inline constexpr std::string_view kTqClientSecret  = "be30b9f4-6862-488a-99ad-21bde0400081";
inline constexpr std::string_view kTqDefaultMode   = "real";

// 由 CMake version.h.in 注入的版本/包名常量。
inline constexpr std::string_view kTqUserAgent          = tqsdk::kUserAgent;
inline constexpr std::string_view kDefaultNodeUserAgent = tqsdk::kNodeUserAgent;

} // namespace tqsdk::constants

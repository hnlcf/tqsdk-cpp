#pragma once

#include "tqsdk/pch.h"
#include "tqsdk/types.h"

#include "sentinel.h"

namespace tqsdk {

/// Kline 的变更语义
namespace KlineChangeDetail {

/// 整体重置
inline constexpr uint64_t Reset = 1ull << 0;

/// 最后一根更新
inline constexpr uint64_t LastUpdated = 1ull << 1;

/// 新 bar 追加
inline constexpr uint64_t Appended = 1ull << 2;

} // namespace KlineChangeDetail

struct KlineBar {
    TimeStamp datetime;

    /// 开盘价
    double open = kUnknownDouble;

    /// 最高价
    double high = kUnknownDouble;

    /// 最低价
    double low = kUnknownDouble;

    /// 收盘价
    double close = kUnknownDouble;

    /// 成交量
    uint64_t volume = 0;

    /// 起始持仓量
    uint64_t open_oi = 0;

    /// 结束持仓量
    uint64_t close_oi = 0;
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(
    KlineBar, datetime, open, high, low, close, volume, open_oi, close_oi)

struct KlineStream {
    /// 合约代码
    InstrumentId instrumnet_id;

    /// K线周期
    uint64_t duration_ns = 0;

    /// K线数量
    uint64_t capacity = 200;

    /// K线数据
    std::deque<KlineBar> bars;

    /// 字段更新标志位图
    uint64_t changed_fields = 0;

    /// 本地历史缓存已注入。为 true 时远端历史帧按 datetime 合并，
    /// 不能整体替换（远端可能只请求了最新 2 根）。
    bool cached_history = false;
};

} // namespace tqsdk

#pragma once

#include "tqsdk/pch.h"
#include "tqsdk/types.h"

#include "sentinel.h"

namespace tqsdk {

enum class PositionField : uint64_t {
    // TODO
};

struct Position {
    /// 交易所代码
    ExchangeId exchange_id;

    /// 合约代码
    InstrumentId instrument_id;

    //////////////////////////////////////////////// 持仓手数与冻结手数

    /// 多头今仓持仓手数
    uint64_t volume_long_today = 0;

    /// 多头老仓持仓手数
    uint64_t volume_long_his = 0;

    /// 多头持仓手数
    uint64_t volume_long = 0;

    /// 多头今仓冻结手数
    uint64_t volume_long_frozen_today = 0;

    /// 多头老仓冻结手数
    uint64_t volume_long_frozen_his = 0;

    /// 空头今仓持仓手数
    uint64_t volume_short_today = 0;

    /// 空头老仓持仓手数
    uint64_t volume_short_his = 0;

    /// 空头持仓手数
    uint64_t volume_short = 0;

    /// 空头今仓冻结手数
    uint64_t volume_short_frozen_today = 0;

    /// 空头老仓冻结手数
    uint64_t volume_short_frozen_his = 0;

    /// 买开仓挂单手数
    uint64_t order_volume_buy_open = 0;

    /// 买平仓挂单手数
    uint64_t order_volume_buy_close = 0;

    /// 卖开仓挂单手数
    uint64_t order_volume_sell_open = 0;

    /// 卖平仓挂单手数
    uint64_t order_volume_sell_close = 0;

    //////////////////////////////////////////////// 成本, 现价与盈亏
    /// 多头开仓均价
    double open_price_long = kUnknownDouble;

    /// 空头开仓均价
    double open_price_short = kUnknownDouble;

    /// 多头开仓成本
    double open_cost_long = kUnknownDouble;

    /// 空头开仓成本
    double open_cost_short = kUnknownDouble;

    /// 多头持仓均价
    double position_price_long = kUnknownDouble;

    /// 空头持仓均价
    double position_price_short = kUnknownDouble;

    /// 多头持仓成本
    double position_cost_long = kUnknownDouble;

    /// 空头持仓成本
    double position_cost_short = kUnknownDouble;

    /// 最新价
    double last_price = kUnknownDouble;

    /// 多头浮动盈亏
    double float_profit_long = kUnknownDouble;

    /// 空头浮动盈亏
    double float_profit_short = kUnknownDouble;

    /// 浮动盈亏 = float_profit_long + float_profit_short
    double float_profit = kUnknownDouble;

    /// 多头持仓盈亏
    double position_profit_long = kUnknownDouble;

    /// 空头持仓盈亏
    double position_profit_short = kUnknownDouble;

    /// 持仓盈亏 = position_profit_long + position_profit_short
    double position_profit = kUnknownDouble;

    //////////////////////////////////////////////// 保证金占用

    /// 多头持仓占用保证金
    double margin_long = kUnknownDouble;

    /// 空头持仓占用保证金
    double margin_short = kUnknownDouble;

    /// 持仓占用保证金 = margin_long + margin_short
    double margin = kUnknownDouble;

    /// 字段更新标志位图
    uint64_t changed_fields = 0;
};

} // namespace tqsdk

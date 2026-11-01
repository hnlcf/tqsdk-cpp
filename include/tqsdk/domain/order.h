#pragma once

#include "tqsdk/pch.h"
#include "tqsdk/types.h"

#include "sentinel.h"

namespace tqsdk {

enum class OrderField : uint64_t {
    order_type        = 1ull << 0,
    exchange_id       = 1ull << 1,
    instrument_id     = 1ull << 2,
    direction         = 1ull << 3,
    offset            = 1ull << 4,
    volume_orign      = 1ull << 5,
    volume_left       = 1ull << 6,
    trade_type        = 1ull << 7,
    price_type        = 1ull << 8,
    limit_price       = 1ull << 9,
    time_cond         = 1ull << 10,
    volume_cond       = 1ull << 11,
    min_volume        = 1ull << 12,
    hedge_flag        = 1ull << 13,
    status            = 1ull << 14,
    last_msg          = 1ull << 15,
    insert_date_time  = 1ull << 16,
    exchange_order_id = 1ull << 17,
    trade_price       = 1ull << 18,
    is_error          = 1ull << 19,
    is_dead           = 1ull << 20,
};

struct Order {
    /// 委托单代码
    OrderId order_id;

    /// 指令类型
    OrderType order_type;

    /// 交易所代码
    ExchangeId exchange_id;

    /// 合约代码
    InstrumentId instrument_id;

    /// 买卖方向
    Direction direction = Direction::BUY;

    /// 开平标志
    Offset offset = Offset::OPEN;

    /// 总报单手数
    uint64_t volume_orign = 0;

    /// 未成交手数
    uint64_t volume_left = 0;

    /// 指令类型
    TradeType trade_type;

    /// 指令类型
    PriceType price_type;

    /// 委托价格, 仅当 price_type = LIMIT 时有效
    double limit_price = kUnknownDouble;

    /// 时间条件
    TimeCondition time_cond = TimeCondition::GFD;

    /// 数量条件
    VolumeCondition volume_cond = VolumeCondition::ANY;

    uint64_t min_volume = 0;

    /// 保值标志
    std::string hedge_flag;

    /// 委托单状态, ALIVE=有效, FINISHED=已完
    OrderStatus status = OrderStatus::ALIVE;

    /// 最后操作信息
    std::string last_msg;

    /// 下单时间
    TimeStamp insert_date_time;

    /// 交易所单号
    ExchangeOrderId exchange_order_id;

    /// 成交价格
    double trade_price = kUnknownDouble;

    bool is_error = false; // ?
    bool is_dead  = false; // ?终态

    /// 字段更新标志位图
    uint64_t changed_fields = 0;
};

} // namespace tqsdk

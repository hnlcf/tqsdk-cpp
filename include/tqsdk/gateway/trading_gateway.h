#pragma once

#include "tqsdk/pch.h"

#include "tqsdk/channel/channel.h"
#include "tqsdk/domain/sentinel.h"
#include "tqsdk/types.h"

namespace tqsdk {

/// 报单请求（TqApi -> TradingGateway）。
/// 所有字段都是"用户意图"，网关负责翻译成各家柜台的具体报单参数。
struct InsertOrderReq {
    InstrumentId instrument_id;
    ExchangeId   exchange_id; // 可留空，由网关按合约补齐

    Direction direction = Direction::BUY;
    Offset    offset    = Offset::OPEN;

    PriceType price_type  = PriceType::LIMIT;
    double    limit_price = kUnknownDouble; // price_type == LIMIT 时有效

    TimeCondition   time_cond   = TimeCondition::GFD;
    VolumeCondition volume_cond = VolumeCondition::ANY;

    uint64_t volume     = 0; // 手数
    uint64_t min_volume = 0; // volume_cond == MIN 时有效
};

/// 撤单请求。
struct CancelOrderReq {
    OrderId     order_id;          // TqApi 本地 rid
    std::string exchange_order_id; // 部分柜台需要
};

/// 命令回执（网关 -> TqApi），仅表示"命令是否被网关接受"，
/// 不代表订单终态；订单状态推进一律走 TradeEvent::Diff。
struct CommandAck {
    bool        success = false;
    OrderId     order_id;  // 网关确认的订单号（通常等于 rid）
    std::string error_msg; // success == false 时有效
};

/// 交易网关向上游(TqApi)推送的统一事件。
struct TradeEvent {
    enum class Kind : uint8_t {
        Diff,         // payload 是与行情同构的 trade diff JSON (含 user.orders/trades/...)
        Ack,          // payload 忽略，ack 字段有效（命令级别的即时回执）
        Error,        // error_msg 有效
        Disconnected, // 与柜台断开
    } kind;

    nlohmann::json payload;   // Kind::Diff 时有效
    CommandAck     ack;       // Kind::Ack 时有效
    std::string    error_msg; // Kind::Error 时有效
};

/// TradingGateway 是交易网关的抽象接口。
///
/// 契约：
///   - insert_order / cancel_order 只负责"把命令发往柜台"，成功即返回 ack，
///     不阻塞等待成交/终态；订单状态变化一律通过 event_output() 以 diff 形式推送。
///   - TqApi 持有 rid（本地订单号），网关必须用 rid 作为回执关联键，
///     保证 mock / 天勤 / CTP 三者的关联语义一致。
///   - stop() 应关闭 event_output() 通道。
class TradingGateway {
public:
    virtual ~TradingGateway() = default;

    virtual asio::awaitable<void> start() = 0;
    virtual asio::awaitable<void> stop()  = 0;

    // rid 由 TqApi 生成并作为本地订单号；网关据此产出后续 diff。
    virtual asio::awaitable<CommandAck> insert_order(const InsertOrderReq& req,
                                                     const OrderId&        rid) = 0;

    virtual asio::awaitable<CommandAck> cancel_order(const CancelOrderReq& req) = 0;

    // 交易事件推送通道；TqApi 的后台 td_loop 从此消费。
    virtual Channel<TradeEvent>& event_output() = 0;
};

} // namespace tqsdk

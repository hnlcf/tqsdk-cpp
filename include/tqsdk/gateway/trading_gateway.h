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

// JSON 序列化（供 HTTP 层 / IPC 复用）。limit_price 用"未知价=NaN"语义：
// 编码为非有限时写 null，解码时 null/缺省 -> kUnknownDouble，避免 null->double 抛错。
inline void to_json(nlohmann::json& j, const InsertOrderReq& r) {
    j = nlohmann::json{
        {"instrument_id", r.instrument_id},
        {"exchange_id", r.exchange_id},
        {"direction", r.direction},
        {"offset", r.offset},
        {"price_type", r.price_type},
        {"limit_price",
         std::isfinite(r.limit_price) ? nlohmann::json(r.limit_price) : nlohmann::json(nullptr)},
        {"time_cond", r.time_cond},
        {"volume_cond", r.volume_cond},
        {"volume", r.volume},
        {"min_volume", r.min_volume}};
}
inline void from_json(const nlohmann::json& j, InsertOrderReq& r) {
    r.instrument_id = j.at("instrument_id").get<InstrumentId>();
    r.exchange_id   = j.value("exchange_id", ExchangeId{});
    r.direction     = j.value("direction", Direction::BUY);
    r.offset        = j.value("offset", Offset::OPEN);
    r.price_type    = j.value("price_type", PriceType::LIMIT);
    if (auto it = j.find("limit_price"); it != j.end() && !it->is_null()) {
        r.limit_price = it->get<double>();
    } else {
        r.limit_price = kUnknownDouble;
    }
    r.time_cond   = j.value("time_cond", TimeCondition::GFD);
    r.volume_cond = j.value("volume_cond", VolumeCondition::ANY);
    r.volume      = j.value("volume", uint64_t{0});
    r.min_volume  = j.value("min_volume", uint64_t{0});
}

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
        Snapshot,     // payload 是完整 trade 截面；消费端先清空交易实体再应用
        Ack,          // payload 忽略，ack 字段有效（命令级别的即时回执）
        Error,        // error_msg 有效
        Disconnected, // 与柜台断开
    } kind;

    nlohmann::json payload;   // Kind::Diff / Snapshot 时有效
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

    /// 完整交易截面（trade diff 同构，含 orders/trades/positions/accounts）。
    /// 用于断线重连对账；不支持快照的网关返回空 object，上层需报错或降级。
    virtual nlohmann::json snapshot() const { return nlohmann::json::object(); }

    // 交易事件推送通道；TqApi 的后台 td_loop 从此消费。
    virtual Channel<TradeEvent>& event_output() = 0;

    // 可选指标；无指标的网关返回空 object。
    virtual nlohmann::json metrics() const { return nlohmann::json::object(); }

    // 远端网关需要先收到一次完整 Snapshot 才算就绪；本地/mock 网关默认无需。
    virtual bool requires_initial_trade_snapshot() const { return false; }
};

} // namespace tqsdk

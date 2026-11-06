#include "tqsdk/tqapi.h"

#include "tqsdk/trading/order_normalizer.h"

namespace tqsdk {

asio::awaitable<Order*> TqApi::insert_order(const InsertOrderReq& req) {
    // 1) 本地造占位单，返回其指针（生命周期到 close 前，发单 ack 前即可见）。
    std::string rid = next_order_id();
    auto&       o   = data_.orders.get_or_create(rid);
    o.order_id      = rid;
    o.instrument_id = req.instrument_id;
    o.exchange_id   = req.exchange_id;
    o.direction     = req.direction;
    o.offset        = req.offset;
    o.price_type    = req.price_type;
    o.limit_price   = req.limit_price;
    o.time_cond     = req.time_cond;
    o.volume_cond   = req.volume_cond;
    o.min_volume    = req.min_volume;
    o.volume_orign  = req.volume;
    o.volume_left   = req.volume;
    o.status        = OrderStatus::ALIVE;
    o.is_dead       = false;
    o.is_error      = false;

    // SHFE 不接受 CTP 的 AnyPrice/BestPrice：在 node 侧利用已合并的 quote
    // 转换成可报的限价单，避免柜台直接返回“报单字段/类型不支持”。
    const bool   normalize_shfe = td_ != nullptr && td_->needs_shfe_price_normalization();
    const Quote* quote          = normalize_shfe ? data_.quotes.find(req.instrument_id) : nullptr;
    const OrderPriceNormalization normalized = normalize_shfe
                                                   ? normalize_shfe_order_price(req, quote)
                                                   : OrderPriceNormalization{req, false, {}};
    if (!normalized.ok()) {
        o.is_error = true;
        o.is_dead  = true;
        o.status   = OrderStatus::FINISHED;
        o.last_msg = normalized.error;
        o.changed_fields |= (uint64_t)OrderField::is_error | (uint64_t)OrderField::is_dead |
                            (uint64_t)OrderField::status | (uint64_t)OrderField::last_msg;
        co_return &o;
    }
    const InsertOrderReq& effective = normalized.request;
    if (normalized.converted) {
        o.price_type  = effective.price_type;
        o.limit_price = effective.limit_price;
        o.time_cond   = effective.time_cond;
        o.changed_fields |= (uint64_t)OrderField::price_type | (uint64_t)OrderField::limit_price |
                            (uint64_t)OrderField::time_cond;
    }

    // 2) 发给网关（异步发命令，不等终态）。
    if (td_) {
        try {
            CommandAck ack = co_await td_->insert_order(effective, rid);
            if (!ack.success) {
                // 网关当场拒绝：本地立即标记为错误终态，无需再等 Diff。
                o.is_error = true;
                o.is_dead  = true;
                o.status   = OrderStatus::FINISHED;
                o.last_msg = ack.error_msg;
                o.changed_fields |= (uint64_t)OrderField::is_error | (uint64_t)OrderField::is_dead |
                                    (uint64_t)OrderField::status | (uint64_t)OrderField::last_msg;
            }
        } catch (const std::exception& e) {
            // IPC/ACK 超时或 transport 断开：不能把异常抛给 UI；本地订单必须
            // 进入明确的错误终态，后续重连对账再校正柜台侧状态。
            o.is_error = true;
            o.is_dead  = true;
            o.status   = OrderStatus::FINISHED;
            o.last_msg = std::string("trading gateway error: ") + e.what();
            o.changed_fields |= (uint64_t)OrderField::is_error | (uint64_t)OrderField::is_dead |
                                (uint64_t)OrderField::status | (uint64_t)OrderField::last_msg;
        }
    } else {
        // 无交易网关：无法发单，直接标记为错误终态。
        o.is_error = true;
        o.is_dead  = true;
        o.status   = OrderStatus::FINISHED;
        o.last_msg = "no trading gateway";
        o.changed_fields |= (uint64_t)OrderField::is_error | (uint64_t)OrderField::is_dead |
                            (uint64_t)OrderField::status | (uint64_t)OrderField::last_msg;
    }

    co_return &o;
}

asio::awaitable<CommandAck> TqApi::cancel_order(const CancelOrderReq& req) {
    if (!td_) {
        co_return CommandAck{
            .success = false, .order_id = req.order_id, .error_msg = "no trading gateway"};
    }
    try {
        co_return co_await td_->cancel_order(req);
    } catch (const std::exception& e) {
        co_return CommandAck{.success   = false,
                             .order_id  = req.order_id,
                             .error_msg = std::string("trading gateway error: ") + e.what()};
    }
}

} // namespace tqsdk

#include "tqsdk/gateway/ctp/ctp_td_gateway.h"

#include "tqsdk/gateway/ctp/ctp_convert.h"
#include "tqsdk/util/spawn_logged.h"

namespace tqsdk {

using namespace ctp;

void CtpTdGateway::load_persisted_orders() {
    if (!order_store_) {
        return;
    }
    order_store_->load();
    order_store_->set_trading_day(trading_day_);

    orders_.clear();
    ref_to_rid_.clear();
    rid_to_ref_.clear();
    for (auto& stored : order_store_->orders()) {
        if (!stored.ref.empty()) {
            ref_to_rid_[stored.ref] = stored.rid;
            rid_to_ref_[stored.rid] = stored.ref;
        }
        CachedOrder c;
        c.front_id      = stored.front_id;
        c.session_id    = stored.session_id;
        c.exchange_id   = stored.exchange_id;
        c.order_sys_id  = stored.order_sys_id;
        c.instrument_id = stored.instrument_id;
        c.json          = stored.order_json;
        if (c.json.empty()) {
            c.json = nlohmann::json{
                {"order_id", stored.rid},
                {"instrument_id", stored.instrument_id},
                {"exchange_id", stored.exchange_id},
                {"exchange_order_id", stored.order_sys_id},
                {"status", stored.status.empty() ? "UNKNOWN" : stored.status},
                {"last_msg", stored.message},
                {"volume_left", 0},
                {"volume_orign", 0},
            };
        }
        orders_[stored.rid] = std::move(c);
    }
    SPDLOG_INFO("[CtpTd] restored {} persisted order(s)", orders_.size());
}

void CtpTdGateway::persist_order(const OrderId&     rid,
                                 const CachedOrder& order,
                                 std::string        status,
                                 std::string        message) {
    if (!order_store_ || rid.empty()) {
        return;
    }
    StoredOrder stored;
    stored.rid           = rid;
    stored.ref           = rid_to_ref_.count(rid) ? rid_to_ref_[rid] : rid;
    stored.instrument_id = order.instrument_id;
    stored.exchange_id   = order.exchange_id;
    stored.order_sys_id  = order.order_sys_id;
    stored.status        = std::move(status);
    stored.message       = std::move(message);
    stored.trading_day   = trading_day_;
    stored.front_id      = order.front_id;
    stored.session_id    = order.session_id;
    stored.ack_success   = true;
    stored.order_json    = order.json;
    order_store_->upsert(std::move(stored));
}

void CtpTdGateway::persist_cached_order(const OrderId& rid) {
    auto it = orders_.find(rid);
    if (it == orders_.end()) {
        return;
    }
    const auto& j      = it->second.json;
    std::string status = j.value("status", std::string{"UNKNOWN"});
    std::string msg    = j.value("last_msg", std::string{});
    persist_order(rid, it->second, std::move(status), std::move(msg));
}

char CtpTdGateway::ctp_offset_for_order(std::string_view exchange,
                                        std::string_view instrument,
                                        Direction        direction,
                                        Offset           offset) const {
    if (offset != Offset::CLOSE) {
        return ctp_offset(offset);
    }
    std::string ex{exchange};
    std::transform(ex.begin(), ex.end(), ex.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    // SHFE/INE/GFEX 需要区分平今/平昨；有今仓时优先平今。
    if (ex != "SHFE" && ex != "INE" && ex != "GFEX") {
        return ctp_offset(offset);
    }
    const std::string key = tq_symbol(ex, instrument);
    auto              it  = positions_.find(key);
    if (it == positions_.end()) {
        return ctp_offset(offset);
    }
    const auto& p     = it->second;
    const auto  today = (direction == Direction::BUY) ? p.value("volume_short_today", uint64_t{0})
                                                      : p.value("volume_long_today", uint64_t{0});
    return today > 0 ? THOST_FTDC_OF_CloseToday : THOST_FTDC_OF_Close;
}

std::string CtpTdGateway::make_order_ref(const OrderId& rid) {
    if (auto it = rid_to_ref_.find(rid); it != rid_to_ref_.end()) {
        return it->second;
    }

    auto digits = [](std::string_view s) {
        if (s.empty()) {
            return false;
        }
        for (char c : s) {
            if (c < '0' || c > '9') {
                return false;
            }
        }
        return true;
    };
    auto numeric_value = [&](std::string_view s) -> std::optional<int64_t> {
        if (!digits(s) || s.size() > 10) {
            return std::nullopt;
        }
        try {
            return std::stoll(std::string{s});
        } catch (...) {
            return std::nullopt;
        }
    };

    // CTP 的 OrderRef 会被按 int32 解析并做递增检查；只发十进制数字。
    std::string ref;
    if (rid.size() <= 12 && digits(rid)) {
        ref = rid;
    } else if (rid.size() <= 12 && rid[0] == 'r' && digits(std::string_view{rid}.substr(1))) {
        ref = rid.substr(1);
    }

    auto collision = [&](const std::string& candidate) {
        auto it = ref_to_rid_.find(candidate);
        return it != ref_to_rid_.end() && it->second != rid;
    };
    if (ref.empty() || collision(ref)) {
        do {
            ref = std::to_string(++ref_seq_);
        } while (collision(ref));
    }
    if (auto n = numeric_value(ref); n.has_value()) {
        ref_seq_ = std::max(ref_seq_, static_cast<int>(std::min<int64_t>(*n, 2'000'000'000LL)));
    }

    ref_to_rid_[ref] = rid;
    rid_to_ref_[rid] = ref;
    return ref;
}

std::string CtpTdGateway::rid_for_ref(std::string_view ref) const {
    auto it = ref_to_rid_.find(std::string{ref});
    return it == ref_to_rid_.end() ? std::string{ref} : it->second;
}

CommandAck CtpTdGateway::send_insert(const InsertOrderReq& req, const OrderId& rid) {
    if (!logged_in_ || api_ == nullptr) {
        return CommandAck{false, rid, "CTP not logged in"};
    }
    StoredOrder existing;
    if (order_store_ && order_store_->find(rid, existing) && existing.ack_success) {
        SPDLOG_INFO("[CtpTd] duplicate insert rid='{}' replayed from persisted store", rid);
        return CommandAck{true, rid, {}};
    }
    // node 使用 "SHFE.rb2601"；CTP 报单字段拆成 ExchangeID/InstrumentID。
    auto [exchange, instrument] = split_tq_symbol(req.instrument_id);
    if (!req.exchange_id.empty()) {
        exchange = req.exchange_id;
    }
    CThostFtdcInputOrderField field{};
    set_field(field.BrokerID, sizeof(field.BrokerID), cfg_.broker_id);
    set_field(field.InvestorID, sizeof(field.InvestorID), cfg_.user_id);
    set_field(field.UserID, sizeof(field.UserID), cfg_.user_id);
    set_field(field.InstrumentID, sizeof(field.InstrumentID), instrument);
    set_field(field.ExchangeID, sizeof(field.ExchangeID), exchange);
    set_field(field.OrderRef, sizeof(field.OrderRef), make_order_ref(rid));
    field.OrderPriceType    = ctp_price_type(req.price_type);
    field.Direction         = ctp_direction(req.direction);
    field.CombOffsetFlag[0] = ctp_offset_for_order(exchange, instrument, req.direction, req.offset);
    field.CombHedgeFlag[0]  = THOST_FTDC_HF_Speculation;
    if (!req.offset_leg2.empty()) {
        if (auto leg2 = magic_enum::enum_cast<Offset>(req.offset_leg2); leg2.has_value()) {
            field.CombOffsetFlag[1] =
                ctp_offset_for_order(exchange, instrument, req.direction, *leg2);
            field.CombHedgeFlag[1] = THOST_FTDC_HF_Speculation;
        } else {
            return CommandAck{false, rid, "invalid offset_leg2: " + req.offset_leg2};
        }
    }
    field.LimitPrice          = ctp_limit_price(req.price_type, req.limit_price);
    field.VolumeTotalOriginal = static_cast<int>(req.volume);
    field.TimeCondition       = ctp_time_cond(req.time_cond);
    field.VolumeCondition     = ctp_volume_cond(req.volume_cond);
    field.MinVolume           = static_cast<int>(req.min_volume);
    field.ContingentCondition = THOST_FTDC_CC_Immediately;
    field.StopPrice           = DBL_MAX;
    field.ForceCloseReason    = THOST_FTDC_FCC_NotForceClose;
    field.IsAutoSuspend       = 0;
    const int req_id          = ++request_id_;
    field.RequestID           = req_id;
    field.UserForceClose      = 0;

    SPDLOG_INFO("[CtpTd] insert rid='{}' {} {} vol={} price={}",
                rid,
                instrument,
                exchange,
                req.volume,
                req.limit_price);
    int ret = api_->ReqOrderInsert(&field, req_id);
    if (ret != 0) {
        return CommandAck{false, rid, "CTP ReqOrderInsert ret=" + std::to_string(ret)};
    }

    CachedOrder cached;
    cached.front_id      = front_id_;
    cached.session_id    = session_id_;
    cached.exchange_id   = exchange;
    cached.instrument_id = instrument;
    cached.json          = nlohmann::json{
        {"order_id", rid},
        {"instrument_id", req.instrument_id},
        {"exchange_id", exchange},
        {"status", "SUBMITTED"},
        {"last_msg", ""},
        {"volume_left", req.volume},
        {"volume_orign", req.volume},
    };
    orders_[rid] = cached;
    persist_order(rid, cached, "SUBMITTED", "");
    return CommandAck{true, rid, {}};
}

CommandAck CtpTdGateway::send_cancel(const CancelOrderReq& req) {
    if (!logged_in_ || api_ == nullptr) {
        return CommandAck{false, req.order_id, "CTP not logged in"};
    }
    CThostFtdcInputOrderActionField field{};
    set_field(field.BrokerID, sizeof(field.BrokerID), cfg_.broker_id);
    set_field(field.InvestorID, sizeof(field.InvestorID), cfg_.user_id);
    set_field(field.UserID, sizeof(field.UserID), cfg_.user_id);
    set_field(field.OrderRef, sizeof(field.OrderRef), make_order_ref(req.order_id));
    field.FrontID    = front_id_;
    field.SessionID  = session_id_;
    field.ActionFlag = THOST_FTDC_AF_Delete;
    const int req_id = ++request_id_;
    field.RequestID  = req_id;

    if (auto it = orders_.find(req.order_id); it != orders_.end()) {
        const auto& c = it->second;
        set_field(field.ExchangeID, sizeof(field.ExchangeID), c.exchange_id);
        set_field(field.OrderSysID, sizeof(field.OrderSysID), c.order_sys_id);
        set_field(field.InstrumentID, sizeof(field.InstrumentID), c.instrument_id);
        field.FrontID   = c.front_id;
        field.SessionID = c.session_id;
    } else if (!req.exchange_order_id.empty()) {
        set_field(field.OrderSysID, sizeof(field.OrderSysID), req.exchange_order_id);
    }

    int ret = api_->ReqOrderAction(&field, req_id);
    if (ret != 0) {
        return CommandAck{false, req.order_id, "CTP ReqOrderAction ret=" + std::to_string(ret)};
    }
    return CommandAck{true, req.order_id, {}};
}

asio::awaitable<void> CtpTdGateway::acquire_order_slot() {
    auto guard = co_await AsyncMutex::acquire(order_mutex_);
    if (stopped_) {
        throw std::runtime_error("CTP trading gateway stopped");
    }
    const auto now = std::chrono::steady_clock::now();
    if (next_order_at_ > now) {
        order_timer_.expires_at(next_order_at_);
        boost::system::error_code ec;
        co_await order_timer_.async_wait(asio::redirect_error(asio::use_awaitable, ec));
        if (stopped_) {
            throw std::runtime_error("CTP trading gateway stopped");
        }
    }
    // 报单/撤单统一节流，避免交易所/柜台频率限制导致拒绝。
    next_order_at_ =
        std::max(std::chrono::steady_clock::now(), next_order_at_) + cfg_.order_interval;
}

asio::awaitable<CommandAck> CtpTdGateway::insert_order(const InsertOrderReq& req,
                                                       const OrderId&        rid) {
    co_await acquire_order_slot();
    co_return send_insert(req, rid);
}

asio::awaitable<CommandAck> CtpTdGateway::cancel_order(const CancelOrderReq& req) {
    co_await acquire_order_slot();
    co_return send_cancel(req);
}

void CtpTdGateway::reject_order(std::string_view ref, std::string message) {
    const std::string rid = rid_for_ref(ref);
    nlohmann::json    order{
        {"order_id", rid},
        {"status", "FINISHED"},
        {"is_error", true},
        {"is_dead", true},
        {"last_msg", message},
    };
    auto it = orders_.find(rid);
    if (it != orders_.end()) {
        it->second.json = order;
        persist_cached_order(rid);
    } else {
        CachedOrder c;
        c.json       = order;
        orders_[rid] = std::move(c);
        persist_cached_order(rid);
    }
    emit(TradeEvent{TradeEvent::Kind::Diff,
                    nlohmann::json{{"trade", {{"", {{"user", {{"orders", {{rid, order}}}}}}}}}},
                    {},
                    {}});
}

void CtpTdGateway::handle_order_insert(const CThostFtdcInputOrderField& input,
                                       const CThostFtdcRspInfoField*    info,
                                       int                              request_id,
                                       bool                             is_last) {
    (void)request_id;
    (void)is_last;
    if (info != nullptr && info->ErrorID != 0) {
        const std::string message =
            "CTP " + std::to_string(info->ErrorID) + ": " + cstr(info->ErrorMsg);
        SPDLOG_WARN("[CtpTd] insert rejected ref={} err={}", cstr(input.OrderRef), message);
        reject_order(cstr(input.OrderRef), message);
    }
}

void CtpTdGateway::handle_order_action(const CThostFtdcInputOrderActionField& input,
                                       const CThostFtdcRspInfoField*          info,
                                       int                                    request_id,
                                       bool                                   is_last) {
    (void)input;
    (void)request_id;
    (void)is_last;
    if (info != nullptr && info->ErrorID != 0) {
        emit(TradeEvent{TradeEvent::Kind::Error,
                        nlohmann::json{},
                        {},
                        "CTP cancel failed: " + cstr(info->ErrorMsg)});
    }
}

void CtpTdGateway::handle_err_order_insert(const CThostFtdcInputOrderField& input,
                                           const CThostFtdcRspInfoField&    info) {
    reject_order(cstr(input.OrderRef),
                 "CTP " + std::to_string(info.ErrorID) + ": " + cstr(info.ErrorMsg));
}

void CtpTdGateway::handle_err_order_action(const CThostFtdcOrderActionField& action,
                                           const CThostFtdcRspInfoField&     info) {
    (void)action;
    emit(TradeEvent{
        TradeEvent::Kind::Error, nlohmann::json{}, {}, "CTP cancel error: " + cstr(info.ErrorMsg)});
}

void CtpTdGateway::handle_rtn_order(const CThostFtdcOrderField& order) {
    if (stopped_) {
        return;
    }
    SPDLOG_INFO("[CtpTd] rtn order ref={} status={} exch_order={} msg={}",
                cstr(order.OrderRef),
                order.OrderStatus,
                cstr(order.OrderSysID),
                cstr(order.StatusMsg));
    const std::string ref = cstr(order.OrderRef);
    const std::string rid = rid_for_ref(ref);
    // 记录柜台历史 OrderRef，防止新 rid 生成 rN 时与历史订单撞号。
    if (!ref.empty()) {
        if (!ref_to_rid_.count(ref)) {
            ref_to_rid_[ref] = rid;
        }
        if (!rid_to_ref_.count(rid)) {
            rid_to_ref_[rid] = ref;
        }
    }
    CachedOrder c;
    c.json          = order_json(order, rid);
    c.front_id      = order.FrontID;
    c.session_id    = order.SessionID;
    c.exchange_id   = cstr(order.ExchangeID);
    c.order_sys_id  = cstr(order.OrderSysID);
    c.instrument_id = cstr(order.InstrumentID);
    orders_[rid]    = std::move(c);
    persist_cached_order(rid);
    schedule_refresh();
}

void CtpTdGateway::handle_rtn_trade(const CThostFtdcTradeField& trade) {
    if (stopped_) {
        return;
    }
    const std::string ref = cstr(trade.OrderRef);
    const std::string rid = rid_for_ref(ref);
    if (!ref.empty()) {
        if (!ref_to_rid_.count(ref)) {
            ref_to_rid_[ref] = rid;
        }
        if (!rid_to_ref_.count(rid)) {
            rid_to_ref_[rid] = ref;
        }
    }
    const std::string tid = cstr(trade.TradeID);
    trades_[tid]          = trade_json(trade, rid);
    schedule_refresh();
}

} // namespace tqsdk

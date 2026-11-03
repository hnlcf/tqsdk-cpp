#include "tqsdk/gateway/ctp/ctp_td_gateway.h"

#include "tqsdk/gateway/ctp/ctp_convert.h"
#include "tqsdk/util/spawn_logged.h"

#include <filesystem>

namespace asio = boost::asio;

namespace tqsdk {

using namespace ctp;

CtpTdGateway::CtpTdGateway(asio::any_io_executor ex, CtpConfig cfg)
    : ex_(std::move(ex)),
      cfg_(std::move(cfg)),
      ch_(ex_),
      state_(std::make_shared<TdState>(ex_)),
      refresh_timer_(ex_),
      query_timer_(ex_) {}

CtpTdGateway::~CtpTdGateway() {
    stop_impl();
}

void CtpTdGateway::install_callbacks() {
    auto cb                = std::make_shared<TdCallbacks>();
    cb->front_connected    = [this]() { handle_front_connected(); };
    cb->front_disconnected = [this](int reason) { handle_front_disconnected(reason); };
    cb->authenticate       = [this](const CThostFtdcRspAuthenticateField* rsp,
                                    const CThostFtdcRspInfoField*         info,
                                    int                                   req,
                                    bool last) { handle_authenticate(rsp, info, req, last); };
    cb->login              = [this](const CThostFtdcRspUserLoginField& r,
                                    const CThostFtdcRspInfoField*      info,
                                    int                                req,
                                    bool last) { handle_login(r, info, req, last); };
    cb->settlement         = [this](const CThostFtdcSettlementInfoConfirmField* rsp,
                                    const CThostFtdcRspInfoField*               info,
                                    int                                         req,
                                    bool last) { handle_settlement(rsp, info, req, last); };
    cb->order_insert       = [this](const CThostFtdcInputOrderField& input,
                                    const CThostFtdcRspInfoField*    info,
                                    int                              req,
                                    bool last) { handle_order_insert(input, info, req, last); };
    cb->order_action       = [this](const CThostFtdcInputOrderActionField& input,
                                    const CThostFtdcRspInfoField*          info,
                                    int                                    req,
                                    bool last) { handle_order_action(input, info, req, last); };
    cb->err_order_insert   = [this](const CThostFtdcInputOrderField& input,
                                    const CThostFtdcRspInfoField&    info) {
        handle_err_order_insert(input, info);
    };
    cb->err_order_action = [this](const CThostFtdcOrderActionField& action,
                                  const CThostFtdcRspInfoField&     info) {
        handle_err_order_action(action, info);
    };
    cb->rtn_order      = [this](const CThostFtdcOrderField& order) { handle_rtn_order(order); };
    cb->rtn_trade      = [this](const CThostFtdcTradeField& trade) { handle_rtn_trade(trade); };
    cb->qry_order      = [this](const CThostFtdcOrderField*   order,
                                const CThostFtdcRspInfoField* info,
                                int                           req,
                                bool last) { handle_qry_order(order, info, req, last); };
    cb->qry_trade      = [this](const CThostFtdcTradeField*   trade,
                                const CThostFtdcRspInfoField* info,
                                int                           req,
                                bool last) { handle_qry_trade(trade, info, req, last); };
    cb->qry_position   = [this](const CThostFtdcInvestorPositionField* pos,
                                const CThostFtdcRspInfoField*          info,
                                int                                    req,
                                bool last) { handle_qry_position(pos, info, req, last); };
    cb->qry_account    = [this](const CThostFtdcTradingAccountField* acct,
                                const CThostFtdcRspInfoField*        info,
                                int                                  req,
                                bool last) { handle_qry_account(acct, info, req, last); };
    cb->qry_instrument = [this](const CThostFtdcInstrumentField* inst,
                                const CThostFtdcRspInfoField*    info,
                                int                              req,
                                bool last) { handle_qry_instrument(inst, info, req, last); };
    {
        std::lock_guard<std::mutex> lock(state_->cb_mu);
        state_->callbacks = std::move(cb);
    }
}

asio::awaitable<void> CtpTdGateway::start() {
    if (started_ || stopped_) {
        co_return;
    }
    started_ = true;
    if (cfg_.td_front.empty()) {
        throw std::runtime_error("CTP td_front is empty");
    }
    std::error_code ec;
    std::filesystem::create_directories(cfg_.flow_path, ec);

    install_callbacks();
    state_->alive.store(true, std::memory_order_release);
    spi_ = std::make_unique<CtpTdSpi>(state_);
    api_ = CThostFtdcTraderApi::CreateFtdcTraderApi(cfg_.flow_path.c_str());
    if (api_ == nullptr) {
        throw std::runtime_error("CreateFtdcTraderApi failed");
    }
    api_->RegisterSpi(spi_.get());
    api_->SubscribePrivateTopic(THOST_TERT_QUICK);
    api_->SubscribePublicTopic(THOST_TERT_QUICK);
    api_->RegisterFront(const_cast<char*>(cfg_.td_front.c_str()));
    api_->Init();
    SPDLOG_INFO("[CtpTd] connecting to {}", cfg_.td_front);
    co_return;
}

void CtpTdGateway::stop_impl() noexcept {
    if (stopped_) {
        return;
    }
    stopped_ = true;
    state_->alive.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(state_->cb_mu);
        state_->callbacks.reset();
    }
    state_->queue->stop();
    refresh_timer_.cancel();
    query_timer_.cancel();
    query_queue_.clear();
    if (api_ != nullptr) {
        api_->RegisterSpi(nullptr);
        api_->Release();
        api_ = nullptr;
    }
    ch_.close();
}

asio::awaitable<void> CtpTdGateway::stop() {
    stop_impl();
    co_return;
}

void CtpTdGateway::emit(TradeEvent ev) {
    if (stopped_) {
        return;
    }
    co_spawn_logged(
        ex_,
        [this, e = std::move(ev)]() mutable -> asio::awaitable<void> {
            try {
                co_await ch_.async_send(std::move(e));
            } catch (const std::exception& ex) {
                SPDLOG_DEBUG("[CtpTd] emit after close: {}", ex.what());
            }
        },
        "CtpTd.emit");
}

void CtpTdGateway::do_login() {
    if (api_ == nullptr || stopped_) {
        return;
    }
    CThostFtdcReqUserLoginField req{};
    set_field(req.BrokerID, sizeof(req.BrokerID), cfg_.broker_id);
    set_field(req.UserID, sizeof(req.UserID), cfg_.user_id);
    set_field(req.Password, sizeof(req.Password), cfg_.password);
    int ret = api_->ReqUserLogin(&req, ++request_id_);
    if (ret != 0) {
        emit(TradeEvent{TradeEvent::Kind::Error,
                        nlohmann::json{},
                        {},
                        "CTP ReqUserLogin ret=" + std::to_string(ret)});
    }
}

void CtpTdGateway::do_settlement() {
    if (api_ == nullptr || stopped_) {
        return;
    }
    CThostFtdcSettlementInfoConfirmField req{};
    set_field(req.BrokerID, sizeof(req.BrokerID), cfg_.broker_id);
    set_field(req.InvestorID, sizeof(req.InvestorID), cfg_.user_id);
    int ret = api_->ReqSettlementInfoConfirm(&req, ++request_id_);
    if (ret != 0) {
        emit(TradeEvent{TradeEvent::Kind::Error,
                        nlohmann::json{},
                        {},
                        "CTP ReqSettlementInfoConfirm ret=" + std::to_string(ret)});
    }
}

void CtpTdGateway::handle_front_connected() {
    if (stopped_) {
        return;
    }
    SPDLOG_INFO("[CtpTd] front connected");
    logged_in_     = false;
    authenticated_ = false;
    if (!cfg_.app_id.empty() && !cfg_.auth_code.empty()) {
        CThostFtdcReqAuthenticateField req{};
        set_field(req.BrokerID, sizeof(req.BrokerID), cfg_.broker_id);
        set_field(req.UserID, sizeof(req.UserID), cfg_.user_id);
        set_field(req.AppID, sizeof(req.AppID), cfg_.app_id);
        set_field(req.AuthCode, sizeof(req.AuthCode), cfg_.auth_code);
        int ret = api_ != nullptr ? api_->ReqAuthenticate(&req, ++request_id_) : -1;
        if (ret != 0) {
            emit(TradeEvent{TradeEvent::Kind::Error,
                            nlohmann::json{},
                            {},
                            "CTP ReqAuthenticate ret=" + std::to_string(ret)});
        }
    } else {
        do_login();
    }
}

void CtpTdGateway::handle_front_disconnected(int reason) {
    logged_in_       = false;
    authenticated_   = false;
    query_in_flight_ = false;
    query_waiting_   = false;
    pending_queries_ = 0;
    refresh_pending_ = false;
    query_queue_.clear();
    query_timer_.cancel();
    if (stopped_) {
        return;
    }
    SPDLOG_WARN("[CtpTd] front disconnected reason={}", reason);
    emit(TradeEvent{TradeEvent::Kind::Disconnected,
                    nlohmann::json{},
                    {},
                    "CTP td disconnected reason=" + std::to_string(reason)});
}

void CtpTdGateway::handle_authenticate(const CThostFtdcRspAuthenticateField* rsp,
                                       const CThostFtdcRspInfoField*         info,
                                       int                                   request_id,
                                       bool                                  is_last) {
    (void)rsp;
    (void)request_id;
    (void)is_last;
    if (stopped_) {
        return;
    }
    if (info != nullptr && info->ErrorID != 0) {
        emit(TradeEvent{TradeEvent::Kind::Error,
                        nlohmann::json{},
                        {},
                        "CTP authenticate failed: " + cstr(info->ErrorMsg)});
        return;
    }
    authenticated_ = true;
    do_login();
}

void CtpTdGateway::handle_login(const CThostFtdcRspUserLoginField& rsp,
                                const CThostFtdcRspInfoField*      info,
                                int                                request_id,
                                bool                               is_last) {
    (void)request_id;
    (void)is_last;
    if (stopped_) {
        return;
    }
    if (info != nullptr && info->ErrorID != 0) {
        emit(TradeEvent{TradeEvent::Kind::Error,
                        nlohmann::json{},
                        {},
                        "CTP login failed: " + cstr(info->ErrorMsg)});
        return;
    }
    front_id_    = rsp.FrontID;
    session_id_  = rsp.SessionID;
    trading_day_ = cstr(rsp.TradingDay);
    logged_in_   = true;
    SPDLOG_INFO("[CtpTd] login ok front={} session={} trading_day={}",
                front_id_,
                session_id_,
                trading_day_);
    do_settlement();
}

void CtpTdGateway::handle_settlement(const CThostFtdcSettlementInfoConfirmField* rsp,
                                     const CThostFtdcRspInfoField*               info,
                                     int                                         request_id,
                                     bool                                        is_last) {
    (void)rsp;
    (void)request_id;
    (void)is_last;
    if (stopped_) {
        return;
    }
    if (info != nullptr && info->ErrorID != 0) {
        emit(TradeEvent{TradeEvent::Kind::Error,
                        nlohmann::json{},
                        {},
                        "CTP settlement confirm failed: " + cstr(info->ErrorMsg)});
        return;
    }
    SPDLOG_INFO("[CtpTd] settlement confirmed");
    query_in_flight_ = false;
    query_waiting_   = false;
    pending_queries_ = 0;
    refresh_pending_ = false;
    query_queue_.clear();
    query_timer_.cancel();
    start_queries(true);
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
    field.OrderPriceType      = ctp_price_type(req.price_type);
    field.Direction           = ctp_direction(req.direction);
    field.CombOffsetFlag[0]   = ctp_offset(req.offset);
    field.CombHedgeFlag[0]    = THOST_FTDC_HF_Speculation;
    field.LimitPrice          = req.price_type == PriceType::LIMIT ? req.limit_price : DBL_MAX;
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

asio::awaitable<CommandAck> CtpTdGateway::insert_order(const InsertOrderReq& req,
                                                       const OrderId&        rid) {
    co_return send_insert(req, rid);
}

asio::awaitable<CommandAck> CtpTdGateway::cancel_order(const CancelOrderReq& req) {
    co_return send_cancel(req);
}

void CtpTdGateway::reject_order(std::string_view ref, std::string message) {
    const std::string rid = rid_for_ref(ref);
    nlohmann::json    order{
        {"order_id", rid},
        {"status", "FINISHED"},
        {"is_error", true},
        {"is_dead", true},
        {"last_msg", std::move(message)},
    };
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
        SPDLOG_WARN(
            "[CtpTd] insert rejected ref={} err={}", cstr(input.OrderRef), cstr(info->ErrorMsg));
        reject_order(cstr(input.OrderRef), cstr(info->ErrorMsg));
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
    reject_order(cstr(input.OrderRef), cstr(info.ErrorMsg));
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

void CtpTdGateway::handle_qry_order(const CThostFtdcOrderField*   order,
                                    const CThostFtdcRspInfoField* info,
                                    int                           request_id,
                                    bool                          is_last) {
    (void)request_id;
    if (order != nullptr) {
        const std::string ref = cstr(order->OrderRef);
        const std::string rid = rid_for_ref(ref);
        if (!ref.empty()) {
            if (!ref_to_rid_.count(ref)) {
                ref_to_rid_[ref] = rid;
            }
            if (!rid_to_ref_.count(rid)) {
                rid_to_ref_[rid] = ref;
            }
        }
        CachedOrder c;
        c.json          = order_json(*order, rid);
        c.front_id      = order->FrontID;
        c.session_id    = order->SessionID;
        c.exchange_id   = cstr(order->ExchangeID);
        c.order_sys_id  = cstr(order->OrderSysID);
        c.instrument_id = cstr(order->InstrumentID);
        orders_[rid]    = std::move(c);
    }
    if (is_last) {
        SPDLOG_DEBUG("[CtpTd] qry order done (info={})", info ? info->ErrorID : 0);
        finish_queries();
    }
}

void CtpTdGateway::handle_qry_trade(const CThostFtdcTradeField*   trade,
                                    const CThostFtdcRspInfoField* info,
                                    int                           request_id,
                                    bool                          is_last) {
    (void)info;
    (void)request_id;
    if (trade != nullptr) {
        const std::string ref = cstr(trade->OrderRef);
        const std::string rid = rid_for_ref(ref);
        if (!ref.empty()) {
            if (!ref_to_rid_.count(ref)) {
                ref_to_rid_[ref] = rid;
            }
            if (!rid_to_ref_.count(rid)) {
                rid_to_ref_[rid] = ref;
            }
        }
        trades_[cstr(trade->TradeID)] = trade_json(*trade, rid);
    }
    if (is_last) {
        SPDLOG_DEBUG("[CtpTd] qry trade done (info={})", info ? info->ErrorID : 0);
        finish_queries();
    }
}

void CtpTdGateway::handle_qry_position(const CThostFtdcInvestorPositionField* position,
                                       const CThostFtdcRspInfoField*          info,
                                       int                                    request_id,
                                       bool                                   is_last) {
    (void)info;
    (void)request_id;
    if (position != nullptr) {
        const std::string symbol =
            tq_symbol(cstr(position->ExchangeID), cstr(position->InstrumentID));
        auto                 spec_it = instruments_.find(symbol);
        const InstrumentSpec spec =
            spec_it == instruments_.end() ? InstrumentSpec{} : spec_it->second;
        add_position(pos_agg_[symbol], *position, spec);
    }
    if (is_last) {
        SPDLOG_DEBUG("[CtpTd] qry position done (info={})", info ? info->ErrorID : 0);
        positions_ready_ = true;

        // 只为实际持仓合约查规格，避免全市场合约查询把报单/成交回调挤到后面。
        std::set<std::string> missing_specs;
        for (const auto& [symbol, agg] : pos_agg_) {
            (void)agg;
            if (instruments_.find(symbol) == instruments_.end()) {
                missing_specs.insert(symbol);
            }
        }
        if (!missing_specs.empty()) {
            instruments_ready_ = false;
            pending_instrument_queries_ += static_cast<int>(missing_specs.size());
            pending_queries_ += static_cast<int>(missing_specs.size());
            for (const auto& symbol : missing_specs) {
                query_queue_.push_back(QueryJob{QueryKind::InstrumentSymbol, symbol});
            }
        } else {
            instruments_ready_ = true;
            rebuild_positions();
        }
        finish_queries();
    }
}

void CtpTdGateway::handle_qry_account(const CThostFtdcTradingAccountField* account,
                                      const CThostFtdcRspInfoField*        info,
                                      int                                  request_id,
                                      bool                                 is_last) {
    (void)info;
    (void)request_id;
    if (account != nullptr) {
        account_ = account_json(*account);
        SPDLOG_DEBUG(
            "[CtpTd] account balance={} available={}", account->Balance, account->Available);
    }
    if (is_last) {
        SPDLOG_DEBUG("[CtpTd] qry account done (info={}, record={})",
                     info ? info->ErrorID : 0,
                     account != nullptr);
        finish_queries();
    }
}

void CtpTdGateway::handle_qry_instrument(const CThostFtdcInstrumentField* instrument,
                                         const CThostFtdcRspInfoField*    info,
                                         int                              request_id,
                                         bool                             is_last) {
    (void)info;
    (void)request_id;
    if (instrument != nullptr) {
        instruments_[tq_symbol(cstr(instrument->ExchangeID), cstr(instrument->InstrumentID))] =
            parse_instrument(*instrument);
    }
    if (is_last) {
        SPDLOG_DEBUG("[CtpTd] qry instrument done (info={}, loaded={})",
                     info ? info->ErrorID : 0,
                     instruments_.size());
        finish_instrument_query();
        finish_queries();
    }
}

void CtpTdGateway::finish_instrument_query() {
    if (pending_instrument_queries_ > 0) {
        --pending_instrument_queries_;
    }
    if (pending_instrument_queries_ == 0) {
        instruments_ready_ = true;
        if (positions_ready_) {
            rebuild_positions();
        }
    }
}

void CtpTdGateway::rebuild_positions() {
    positions_.clear();
    for (const auto& [symbol, agg] : pos_agg_) {
        auto                 spec_it = instruments_.find(symbol);
        const InstrumentSpec spec =
            spec_it == instruments_.end() ? InstrumentSpec{} : spec_it->second;
        positions_[symbol] = position_json(symbol, agg, spec);
    }
}

void CtpTdGateway::start_queries(bool initial) {
    if (!logged_in_ || api_ == nullptr || stopped_) {
        return;
    }
    if (query_in_flight_) {
        refresh_pending_ = !initial;
        return;
    }

    query_in_flight_ = true;
    query_initial_   = initial;
    query_waiting_   = false;
    query_queue_.clear();
    pending_instrument_queries_ = 0;

    // 不先清空缓存：查询完成前 snapshot() 仍返回上一份可用截面，
    // 避免 node 在 CTP 重连/查询窗口内被空快照清空交易状态。
    // 不再一次性查询全市场合约（1.7 万条回调会淹没报单/成交回调）；
    // 仅在有持仓时按合约逐个查规格。
    instruments_ready_ = true;
    positions_ready_   = false;
    pos_agg_.clear();

    query_queue_.push_back(QueryJob{QueryKind::Account});
    query_queue_.push_back(QueryJob{QueryKind::Position});
    query_queue_.push_back(QueryJob{QueryKind::Order});
    query_queue_.push_back(QueryJob{QueryKind::Trade});
    pending_queries_ = static_cast<int>(query_queue_.size());

    if (initial) {
        SPDLOG_INFO("[CtpTd] query snapshot: {} request(s)", pending_queries_);
    } else {
        SPDLOG_DEBUG("[CtpTd] refresh snapshot: {} request(s)", pending_queries_);
    }
    pump_queries();
}

int CtpTdGateway::send_query(const QueryJob& job) {
    const int req_id = ++request_id_;
    switch (job.kind) {
    case QueryKind::InstrumentSymbol: {
        CThostFtdcQryInstrumentField q{};
        auto [exchange, instrument] = split_tq_symbol(job.symbol);
        (void)exchange;
        set_field(q.InstrumentID, sizeof(q.InstrumentID), instrument);
        return api_->ReqQryInstrument(&q, req_id);
    }
    case QueryKind::Account: {
        CThostFtdcQryTradingAccountField q{};
        set_field(q.BrokerID, sizeof(q.BrokerID), cfg_.broker_id);
        set_field(q.InvestorID, sizeof(q.InvestorID), cfg_.user_id);
        return api_->ReqQryTradingAccount(&q, req_id);
    }
    case QueryKind::Position: {
        CThostFtdcQryInvestorPositionField q{};
        set_field(q.BrokerID, sizeof(q.BrokerID), cfg_.broker_id);
        set_field(q.InvestorID, sizeof(q.InvestorID), cfg_.user_id);
        return api_->ReqQryInvestorPosition(&q, req_id);
    }
    case QueryKind::Order: {
        CThostFtdcQryOrderField q{};
        set_field(q.BrokerID, sizeof(q.BrokerID), cfg_.broker_id);
        set_field(q.InvestorID, sizeof(q.InvestorID), cfg_.user_id);
        return api_->ReqQryOrder(&q, req_id);
    }
    case QueryKind::Trade: {
        CThostFtdcQryTradeField q{};
        set_field(q.BrokerID, sizeof(q.BrokerID), cfg_.broker_id);
        set_field(q.InvestorID, sizeof(q.InvestorID), cfg_.user_id);
        return api_->ReqQryTrade(&q, req_id);
    }
    }
    return -1;
}

void CtpTdGateway::pump_queries() {
    if (!query_in_flight_ || query_waiting_ || stopped_ || !logged_in_) {
        return;
    }
    if (query_queue_.empty()) {
        return;
    }

    QueryJob job = query_queue_.front();
    query_queue_.pop_front();
    active_query_kind_ = job.kind;
    const int ret      = send_query(job);
    SPDLOG_DEBUG("[CtpTd] query send kind={} ret={}", static_cast<int>(job.kind), ret);
    if (ret == 0) {
        query_waiting_ = true;
        return;
    }

    // -2/-3: 未处理请求超过许可数 / 每秒请求数超过许可数。
    // 保持同一 job 重排队，退避后再发；绝不并发轰炸 CTP 查询接口。
    if ((ret == -2 || ret == -3) && job.attempts < 8) {
        ++job.attempts;
        SPDLOG_WARN("[CtpTd] query kind={} throttled ret={}, retry {}/8",
                    static_cast<int>(job.kind),
                    ret,
                    job.attempts);
        query_queue_.push_front(job);
        schedule_query_after(std::chrono::seconds(1));
        return;
    }

    SPDLOG_WARN("[CtpTd] query kind={} failed ret={}", static_cast<int>(job.kind), ret);
    query_failed();
}

void CtpTdGateway::schedule_query_after(std::chrono::milliseconds delay) {
    if (!query_in_flight_ || stopped_) {
        return;
    }
    query_timer_.expires_after(delay);
    query_timer_.async_wait([this](boost::system::error_code ec) {
        if (!ec && !stopped_ && query_in_flight_) {
            pump_queries();
        }
    });
}

void CtpTdGateway::query_failed() {
    if (active_query_kind_ == QueryKind::InstrumentSymbol) {
        finish_instrument_query();
    }
    if (pending_queries_ > 0) {
        --pending_queries_;
    }
    if (pending_queries_ == 0) {
        complete_queries();
    } else {
        // 给 CTP 一点喘息时间，再发下一条查询。
        schedule_query_after(std::chrono::milliseconds(300));
    }
}

void CtpTdGateway::finish_queries() {
    if (!query_in_flight_) {
        return;
    }
    query_waiting_ = false;
    if (pending_queries_ > 0) {
        --pending_queries_;
    }
    if (pending_queries_ == 0) {
        complete_queries();
    } else {
        schedule_query_after(std::chrono::milliseconds(300));
    }
}

void CtpTdGateway::complete_queries() {
    const bool initial = query_initial_;
    if (initial) {
        SPDLOG_INFO("[CtpTd] initial query snapshot complete");
    } else {
        SPDLOG_DEBUG("[CtpTd] refresh query snapshot complete");
    }
    query_in_flight_ = false;
    query_initial_   = false;
    query_waiting_   = false;
    query_queue_.clear();
    emit_diff(initial);
    if (refresh_pending_) {
        refresh_pending_ = false;
        schedule_refresh();
    }
}

void CtpTdGateway::emit_diff(bool initial_snapshot) {
    nlohmann::json orders = nlohmann::json::object();
    for (const auto& [rid, cached] : orders_) {
        orders[rid] = cached.json;
    }
    nlohmann::json trades = nlohmann::json::object();
    for (const auto& [tid, t] : trades_) {
        trades[tid] = t;
    }
    auto payload          = user_diff(std::move(orders), std::move(trades), positions_, account_);
    TradeEvent::Kind kind = initial_snapshot ? TradeEvent::Kind::Snapshot : TradeEvent::Kind::Diff;
    if (initial_snapshot) {
        initial_loaded_ = true;
    }
    emit(TradeEvent{kind, std::move(payload), {}, {}});
}

void CtpTdGateway::schedule_refresh() {
    if (stopped_ || !logged_in_) {
        return;
    }
    if (query_in_flight_) {
        refresh_pending_ = true;
        return;
    }
    refresh_timer_.expires_after(std::chrono::milliseconds(200));
    refresh_timer_.async_wait([this](boost::system::error_code ec) {
        if (ec || stopped_ || !logged_in_) {
            return;
        }
        start_queries(false);
    });
}

nlohmann::json CtpTdGateway::snapshot() const {
    nlohmann::json orders = nlohmann::json::object();
    for (const auto& [rid, cached] : orders_) {
        orders[rid] = cached.json;
    }
    nlohmann::json trades = nlohmann::json::object();
    for (const auto& [tid, t] : trades_) {
        trades[tid] = t;
    }
    return user_diff(std::move(orders), std::move(trades), positions_, account_);
}

} // namespace tqsdk

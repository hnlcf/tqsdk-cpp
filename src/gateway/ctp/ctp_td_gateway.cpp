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
      refresh_timer_(ex_) {}

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
    pending_queries_ = 0;
    refresh_pending_ = false;
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
    pending_queries_ = 0;
    refresh_pending_ = false;
    start_queries(true);
}

std::string CtpTdGateway::make_order_ref(const OrderId& rid) {
    if (auto it = rid_to_ref_.find(rid); it != rid_to_ref_.end()) {
        return it->second;
    }
    std::string ref;
    if (rid.size() <= 12) {
        ref = rid;
        if (auto it = ref_to_rid_.find(ref); it != ref_to_rid_.end() && it->second != rid) {
            ref = "r" + std::to_string(++ref_seq_);
        }
    } else {
        ref = "r" + std::to_string(++ref_seq_);
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
    CThostFtdcInputOrderField field{};
    set_field(field.BrokerID, sizeof(field.BrokerID), cfg_.broker_id);
    set_field(field.InvestorID, sizeof(field.InvestorID), cfg_.user_id);
    set_field(field.UserID, sizeof(field.UserID), cfg_.user_id);
    set_field(field.InstrumentID, sizeof(field.InstrumentID), req.instrument_id);
    set_field(field.ExchangeID, sizeof(field.ExchangeID), req.exchange_id);
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
    const std::string ref = cstr(order.OrderRef);
    const std::string rid = rid_for_ref(ref);
    CachedOrder       c;
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
    const std::string rid = rid_for_ref(cstr(trade.OrderRef));
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
        const std::string rid = rid_for_ref(cstr(order->OrderRef));
        CachedOrder       c;
        c.json          = order_json(*order, rid);
        c.front_id      = order->FrontID;
        c.session_id    = order->SessionID;
        c.exchange_id   = cstr(order->ExchangeID);
        c.order_sys_id  = cstr(order->OrderSysID);
        c.instrument_id = cstr(order->InstrumentID);
        orders_[rid]    = std::move(c);
    }
    if (is_last) {
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
        const std::string rid         = rid_for_ref(cstr(trade->OrderRef));
        trades_[cstr(trade->TradeID)] = trade_json(*trade, rid);
    }
    if (is_last) {
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
        const std::string    symbol  = cstr(position->InstrumentID);
        auto                 spec_it = instruments_.find(symbol);
        const InstrumentSpec spec =
            spec_it == instruments_.end() ? InstrumentSpec{} : spec_it->second;
        add_position(pos_agg_[symbol], *position, spec);
    }
    if (is_last) {
        positions_ready_ = true;
        if (instruments_ready_) {
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
    }
    if (is_last) {
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
        instruments_[cstr(instrument->InstrumentID)] = parse_instrument(*instrument);
    }
    if (is_last) {
        instruments_ready_ = true;
        if (positions_ready_) {
            rebuild_positions();
        }
        finish_queries();
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
    pending_queries_ = 0;

    // 不先清空缓存：查询完成前 snapshot() 仍返回上一份可用截面，
    // 避免 node 在 CTP 重连/查询窗口内被空快照清空交易状态。
    if (initial) {
        instruments_ready_ = false;
    } else {
        instruments_ready_ = true;
    }
    positions_ready_ = false;
    pos_agg_.clear();

    auto count = [this](int ret) {
        if (ret == 0) {
            ++pending_queries_;
        } else {
            SPDLOG_WARN("[CtpTd] query request ret={}", ret);
        }
    };

    if (initial) {
        CThostFtdcQryInstrumentField q{};
        count(api_->ReqQryInstrument(&q, ++request_id_));
    }
    {
        CThostFtdcQryTradingAccountField q{};
        set_field(q.BrokerID, sizeof(q.BrokerID), cfg_.broker_id);
        set_field(q.InvestorID, sizeof(q.InvestorID), cfg_.user_id);
        count(api_->ReqQryTradingAccount(&q, ++request_id_));
    }
    {
        CThostFtdcQryInvestorPositionField q{};
        set_field(q.BrokerID, sizeof(q.BrokerID), cfg_.broker_id);
        set_field(q.InvestorID, sizeof(q.InvestorID), cfg_.user_id);
        count(api_->ReqQryInvestorPosition(&q, ++request_id_));
    }
    {
        CThostFtdcQryOrderField q{};
        set_field(q.BrokerID, sizeof(q.BrokerID), cfg_.broker_id);
        set_field(q.InvestorID, sizeof(q.InvestorID), cfg_.user_id);
        count(api_->ReqQryOrder(&q, ++request_id_));
    }
    {
        CThostFtdcQryTradeField q{};
        set_field(q.BrokerID, sizeof(q.BrokerID), cfg_.broker_id);
        set_field(q.InvestorID, sizeof(q.InvestorID), cfg_.user_id);
        count(api_->ReqQryTrade(&q, ++request_id_));
    }

    if (pending_queries_ == 0) {
        query_in_flight_ = false;
        emit_diff(initial);
    }
}

void CtpTdGateway::finish_queries() {
    if (!query_in_flight_) {
        return;
    }
    if (pending_queries_ > 0) {
        --pending_queries_;
    }
    if (pending_queries_ == 0) {
        const bool initial = query_initial_;
        query_in_flight_   = false;
        query_initial_     = false;
        emit_diff(initial);
        if (refresh_pending_) {
            refresh_pending_ = false;
            schedule_refresh();
        }
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

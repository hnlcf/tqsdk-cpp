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
      query_timer_(ex_),
      query_watchdog_(ex_),
      order_mutex_(ex_),
      order_timer_(ex_) {}

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

    std::string safe_user = cfg_.user_id;
    for (char& c : safe_user) {
        if (!std::isalnum(static_cast<unsigned char>(c))) {
            c = '_';
        }
    }
    order_store_ = std::make_unique<CtpOrderStore>(
        (std::filesystem::path(cfg_.flow_path) / ("ctp_orders_" + safe_user + ".json")).string());

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
    snapshot_ready_.store(false, std::memory_order_release);
    state_->alive.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(state_->cb_mu);
        state_->callbacks.reset();
    }
    state_->queue->stop();
    refresh_timer_.cancel();
    order_timer_.cancel();
    reset_query_state();
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
    logged_in_     = false;
    authenticated_ = false;
    snapshot_ready_.store(false, std::memory_order_release);
    query_in_flight_ = false;
    query_waiting_   = false;
    pending_queries_ = 0;
    refresh_pending_ = false;
    reset_query_state();
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
    snapshot_ready_.store(false, std::memory_order_release);
    load_persisted_orders();
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
    reset_query_state();
    start_queries(true);
}

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
    field.LimitPrice        = req.price_type == PriceType::LIMIT ? req.limit_price : DBL_MAX;
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

void CtpTdGateway::handle_qry_order(const CThostFtdcOrderField*   order,
                                    const CThostFtdcRspInfoField* info,
                                    int                           request_id,
                                    bool                          is_last) {
    if (!active_query_matches(QueryKind::Order, request_id)) {
        return;
    }
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
        persist_cached_order(rid);
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
    if (!active_query_matches(QueryKind::Trade, request_id)) {
        return;
    }
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
    if (!active_query_matches(QueryKind::Position, request_id)) {
        return;
    }
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
    if (!active_query_matches(QueryKind::Account, request_id)) {
        return;
    }
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
    if (!active_query_matches(QueryKind::InstrumentSymbol, request_id)) {
        return;
    }
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

    ++query_generation_;
    active_query_.reset();
    query_watchdog_.cancel();
    if (initial) {
        snapshot_ready_.store(false, std::memory_order_release);
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

int CtpTdGateway::send_query(QueryJob& job) {
    const int req_id = ++request_id_;
    job.request_id   = req_id;
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

bool CtpTdGateway::active_query_matches(QueryKind kind, int request_id) const {
    return active_query_.has_value() && active_query_->kind == kind &&
           active_query_->request_id == request_id;
}

void CtpTdGateway::arm_query_watchdog() {
    if (!active_query_ || !query_in_flight_ || stopped_) {
        return;
    }
    const uint64_t  gen  = query_generation_;
    const int       req  = active_query_->request_id;
    const QueryKind kind = active_query_->kind;
    query_watchdog_.expires_after(query_timeout_);
    query_watchdog_.async_wait([this, gen, req, kind](boost::system::error_code ec) {
        if (ec || stopped_ || !query_in_flight_ || gen != query_generation_) {
            return;
        }
        if (!active_query_ || active_query_->request_id != req) {
            return;
        }
        SPDLOG_WARN("[CtpTd] query kind={} req={} timeout; skip", static_cast<int>(kind), req);
        active_query_.reset();
        query_waiting_ = false;
        query_failed(kind);
    });
}

void CtpTdGateway::reset_query_state() {
    ++query_generation_;
    active_query_.reset();
    query_waiting_ = false;
    query_queue_.clear();
    query_timer_.cancel();
    query_watchdog_.cancel();
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
    const int ret = send_query(job);
    SPDLOG_DEBUG("[CtpTd] query send kind={} req={} ret={}",
                 static_cast<int>(job.kind),
                 job.request_id,
                 ret);
    if (ret == 0) {
        query_waiting_ = true;
        active_query_  = ActiveQuery{job.kind, job.symbol, job.request_id};
        arm_query_watchdog();
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
    query_failed(job.kind);
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

void CtpTdGateway::query_failed(QueryKind kind) {
    if (kind == QueryKind::InstrumentSymbol) {
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
    active_query_.reset();
    query_watchdog_.cancel();
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
    snapshot_ready_.store(true, std::memory_order_release);
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

bool CtpTdGateway::snapshot_ready() const {
    return snapshot_ready_.load(std::memory_order_acquire);
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

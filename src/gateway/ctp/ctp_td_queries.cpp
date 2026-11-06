#include "tqsdk/gateway/ctp/ctp_td_gateway.h"

#include "tqsdk/gateway/ctp/ctp_convert.h"
#include "tqsdk/util/spawn_logged.h"

namespace tqsdk {

using namespace ctp;

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

TradingCalendar::Phase CtpTdGateway::session_phase() const {
    const auto phase = calendar_.phase_at(std::chrono::system_clock::now());
    // 处于交易窗口但尚未登录/结算确认，说明仍在重连或恢复查询。
    if ((phase == TradingCalendar::Phase::Open || phase == TradingCalendar::Phase::Break ||
         phase == TradingCalendar::Phase::PreOpen) &&
        !logged_in_.load(std::memory_order_relaxed)) {
        return TradingCalendar::Phase::Reconnecting;
    }
    return phase;
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

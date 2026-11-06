#include "tqsdk/gateway/ctp/ctp_td_gateway.h"

#include "tqsdk/gateway/ctp/ctp_convert.h"
#include "tqsdk/util/spawn_logged.h"

namespace tqsdk {

using namespace ctp;

void CtpTdGateway::handle_qry_order(const CThostFtdcOrderField*   order,
                                    const CThostFtdcRspInfoField* info,
                                    int                           request_id,
                                    bool                          is_last) {
    if (!query_pipeline_.active_matches(QueryKind::Order, request_id)) {
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
        query_pipeline_.finish();
    }
}

void CtpTdGateway::handle_qry_trade(const CThostFtdcTradeField*   trade,
                                    const CThostFtdcRspInfoField* info,
                                    int                           request_id,
                                    bool                          is_last) {
    (void)info;
    if (!query_pipeline_.active_matches(QueryKind::Trade, request_id)) {
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
        query_pipeline_.finish();
    }
}

void CtpTdGateway::handle_qry_position(const CThostFtdcInvestorPositionField* position,
                                       const CThostFtdcRspInfoField*          info,
                                       int                                    request_id,
                                       bool                                   is_last) {
    (void)info;
    if (!query_pipeline_.active_matches(QueryKind::Position, request_id)) {
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
            query_pipeline_.enqueue_specs(
                std::vector<std::string>(missing_specs.begin(), missing_specs.end()));
        } else {
            instruments_ready_ = true;
            rebuild_positions();
        }
        query_pipeline_.finish();
    }
}

void CtpTdGateway::handle_qry_account(const CThostFtdcTradingAccountField* account,
                                      const CThostFtdcRspInfoField*        info,
                                      int                                  request_id,
                                      bool                                 is_last) {
    (void)info;
    if (!query_pipeline_.active_matches(QueryKind::Account, request_id)) {
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
        query_pipeline_.finish();
    }
}

void CtpTdGateway::handle_qry_instrument(const CThostFtdcInstrumentField* instrument,
                                         const CThostFtdcRspInfoField*    info,
                                         int                              request_id,
                                         bool                             is_last) {
    (void)info;
    if (!query_pipeline_.active_matches(QueryKind::InstrumentSymbol, request_id)) {
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
        query_pipeline_.finish();
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
    if (query_pipeline_.running()) {
        if (!initial) {
            query_pipeline_.schedule_restart();
        }
        return;
    }

    // 不先清空缓存：查询完成前 snapshot() 仍返回上一份可用截面，
    // 避免 node 在 CTP 重连/查询窗口内被空快照清空交易状态。
    // 不一次性查询全市场合约；仅在有持仓时按合约逐个查规格。
    instruments_ready_          = true;
    positions_ready_            = false;
    pending_instrument_queries_ = 0;
    pos_agg_.clear();

    std::vector<QueryJob> jobs{
        QueryJob{QueryKind::Account},
        QueryJob{QueryKind::Position},
        QueryJob{QueryKind::Order},
        QueryJob{QueryKind::Trade},
    };
    query_pipeline_.start(initial, std::move(jobs));
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
    query_pipeline_.schedule_restart();
}

bool CtpTdGateway::snapshot_ready() const {
    return query_pipeline_.snapshot_ready();
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

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
      query_pipeline_(
          ex_,
          ctp::CtpQueryPipeline::Callbacks{
              .send      = [this](ctp::CtpQueryPipeline::Job& job) { return send_query(job); },
              .completed = [this](bool initial) { emit_diff(initial); },
              .failed =
                  [this](ctp::CtpQueryPipeline::Kind kind) {
                      if (kind == ctp::CtpQueryPipeline::Kind::InstrumentSymbol) {
                          finish_instrument_query();
                      }
                  },
              .restart = [this]() { start_queries(false); },
          },
          std::chrono::seconds(10)),
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
    // 供 TradingCalendar 判断节假日/节前夜盘；后台线程低频刷新。
    HolidayCalendar::start_global();
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
    query_pipeline_.force_snapshot_ready(false);
    state_->alive.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(state_->cb_mu);
        state_->callbacks.reset();
    }
    state_->queue->stop();
    order_timer_.cancel();
    query_pipeline_.reset();
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
    query_pipeline_.force_snapshot_ready(false);
    query_pipeline_.reset();
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
    query_pipeline_.force_snapshot_ready(false);
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
    query_pipeline_.reset();
    start_queries(true);
}

} // namespace tqsdk

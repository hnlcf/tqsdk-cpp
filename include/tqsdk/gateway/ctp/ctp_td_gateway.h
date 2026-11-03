#pragma once

#include "tqsdk/gateway/ctp/ctp_config.h"
#include "tqsdk/gateway/ctp/ctp_convert.h"
#include "tqsdk/gateway/ctp/ctp_spi_impl.h"
#include "tqsdk/gateway/trading_gateway.h"

namespace tqsdk {

/// CTP 交易网关。
///
/// 设计要点：
///   - SPI 回调在 CTP 线程，统一经 CtpTaskQueue 切回 ex（core strand）。
///   - 报单/撤单只返回“柜台是否接受命令”的 ack；订单/成交状态走 TradeEvent::Diff。
///   - 登录/结算确认后查询 instruments/account/positions/orders/trades，发一次
///     TradeEvent::Snapshot；后续成交/报单变化用 200ms debounce 查询并推 diff。
///   - 断线后 CTP API 自动重连，重连成功会重新登录、确认结算、查询并重建快照。
class CtpTdGateway final : public TradingGateway {
public:
    CtpTdGateway(boost::asio::any_io_executor ex, CtpConfig cfg);
    ~CtpTdGateway() override;

    boost::asio::awaitable<void> start() override;
    boost::asio::awaitable<void> stop() override;

    boost::asio::awaitable<CommandAck> insert_order(const InsertOrderReq& req,
                                                    const OrderId&        rid) override;
    boost::asio::awaitable<CommandAck> cancel_order(const CancelOrderReq& req) override;

    nlohmann::json snapshot() const override;

    Channel<TradeEvent>& event_output() override { return ch_; }

    // SPI 回调入口（core strand）。
    void handle_front_connected();
    void handle_front_disconnected(int reason);
    void handle_authenticate(const CThostFtdcRspAuthenticateField* rsp,
                             const CThostFtdcRspInfoField*         info,
                             int                                   request_id,
                             bool                                  is_last);
    void handle_login(const CThostFtdcRspUserLoginField& rsp,
                      const CThostFtdcRspInfoField*      info,
                      int                                request_id,
                      bool                               is_last);
    void handle_settlement(const CThostFtdcSettlementInfoConfirmField* rsp,
                           const CThostFtdcRspInfoField*               info,
                           int                                         request_id,
                           bool                                        is_last);
    void handle_order_insert(const CThostFtdcInputOrderField& input,
                             const CThostFtdcRspInfoField*    info,
                             int                              request_id,
                             bool                             is_last);
    void handle_order_action(const CThostFtdcInputOrderActionField& input,
                             const CThostFtdcRspInfoField*          info,
                             int                                    request_id,
                             bool                                   is_last);
    void handle_err_order_insert(const CThostFtdcInputOrderField& input,
                                 const CThostFtdcRspInfoField&    info);
    void handle_err_order_action(const CThostFtdcOrderActionField& action,
                                 const CThostFtdcRspInfoField&     info);
    void handle_rtn_order(const CThostFtdcOrderField& order);
    void handle_rtn_trade(const CThostFtdcTradeField& trade);
    void handle_qry_order(const CThostFtdcOrderField*   order,
                          const CThostFtdcRspInfoField* info,
                          int                           request_id,
                          bool                          is_last);
    void handle_qry_trade(const CThostFtdcTradeField*   trade,
                          const CThostFtdcRspInfoField* info,
                          int                           request_id,
                          bool                          is_last);
    void handle_qry_position(const CThostFtdcInvestorPositionField* position,
                             const CThostFtdcRspInfoField*          info,
                             int                                    request_id,
                             bool                                   is_last);
    void handle_qry_account(const CThostFtdcTradingAccountField* account,
                            const CThostFtdcRspInfoField*        info,
                            int                                  request_id,
                            bool                                 is_last);
    void handle_qry_instrument(const CThostFtdcInstrumentField* instrument,
                               const CThostFtdcRspInfoField*    info,
                               int                              request_id,
                               bool                             is_last);

private:
    struct CachedOrder {
        nlohmann::json json;
        int            front_id   = 0;
        int            session_id = 0;
        std::string    exchange_id;
        std::string    order_sys_id;
        std::string    instrument_id;
    };

    enum class QueryKind {
        Account,
        Position,
        Order,
        Trade,
        InstrumentSymbol, ///< 仅按持仓合约查询规格，避免全市场合约洪峰
    };

    struct QueryJob {
        QueryKind   kind;
        std::string symbol;
        int         attempts = 0;
    };

    void        install_callbacks();
    void        stop_impl() noexcept;
    void        do_login();
    void        do_settlement();
    void        emit(TradeEvent ev);
    void        schedule_refresh();
    void        start_queries(bool initial);
    void        finish_queries();
    void        pump_queries();
    void        schedule_query_after(std::chrono::milliseconds delay);
    int         send_query(const QueryJob& job);
    void        query_failed();
    void        finish_instrument_query();
    void        complete_queries();
    std::string make_order_ref(const OrderId& rid);
    std::string rid_for_ref(std::string_view ref) const;
    CommandAck  send_insert(const InsertOrderReq& req, const OrderId& rid);
    CommandAck  send_cancel(const CancelOrderReq& req);
    void        emit_diff(bool initial_snapshot);
    void        reject_order(std::string_view ref, std::string message);
    void        rebuild_positions();

    boost::asio::any_io_executor   ex_;
    CtpConfig                      cfg_;
    Channel<TradeEvent>            ch_;
    std::shared_ptr<ctp::TdState>  state_;
    std::unique_ptr<ctp::CtpTdSpi> spi_;
    CThostFtdcTraderApi*           api_ = nullptr;

    bool started_        = false;
    bool stopped_        = false;
    bool authenticated_  = false;
    bool logged_in_      = false;
    bool initial_loaded_ = false;

    int         front_id_   = 0;
    int         session_id_ = 0;
    int         request_id_ = 0;
    int         ref_seq_    = 0;
    std::string trading_day_;

    std::map<OrderId, CachedOrder>             orders_;
    std::map<std::string, nlohmann::json>      trades_;
    std::map<std::string, nlohmann::json>      positions_;
    std::map<std::string, ctp::PositionAgg>    pos_agg_;
    std::map<std::string, ctp::InstrumentSpec> instruments_;
    nlohmann::json                             account_ = nlohmann::json::object();

    std::map<std::string, std::string> ref_to_rid_;
    std::map<std::string, std::string> rid_to_ref_;

    int                       pending_queries_            = 0;
    bool                      query_in_flight_            = false;
    bool                      query_waiting_              = false;
    bool                      query_initial_              = false;
    bool                      refresh_pending_            = false;
    bool                      positions_ready_            = false;
    bool                      instruments_ready_          = false;
    int                       pending_instrument_queries_ = 0;
    QueryKind                 active_query_kind_          = QueryKind::Account;
    std::deque<QueryJob>      query_queue_;
    boost::asio::steady_timer refresh_timer_;
    boost::asio::steady_timer query_timer_;
};

} // namespace tqsdk

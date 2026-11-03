#pragma once

#include "tqsdk/pch.h"

#include "tqsdk/gateway/ctp/ctp_task_queue.h"

#include <ThostFtdcMdApi.h>
#include <ThostFtdcTraderApi.h>
#include <ThostFtdcUserApiStruct.h>

namespace tqsdk {
namespace ctp {

struct MdCallbacks {
    std::function<void()>    front_connected;
    std::function<void(int)> front_disconnected;
    std::function<void(
        const CThostFtdcRspUserLoginField&, const CThostFtdcRspInfoField*, int, bool)>
        login;
    std::function<void(
        const CThostFtdcSpecificInstrumentField*, const CThostFtdcRspInfoField*, int, bool)>
                                                               sub_market;
    std::function<void(const CThostFtdcDepthMarketDataField&)> depth;
};

struct MdState {
    explicit MdState(boost::asio::any_io_executor ex)
        : queue(std::make_shared<CtpTaskQueue>(std::move(ex))) {}

    std::shared_ptr<CtpTaskQueue> queue;
    std::atomic<bool>             alive{true};
    std::mutex                    cb_mu;
    std::shared_ptr<MdCallbacks>  callbacks;
};

class CtpMdSpi final : public CThostFtdcMdSpi {
public:
    explicit CtpMdSpi(std::shared_ptr<MdState> state)
        : state_(std::move(state)) {}

    void OnFrontConnected() override;
    void OnFrontDisconnected(int nReason) override;
    void OnRspUserLogin(CThostFtdcRspUserLoginField* pRspUserLogin,
                        CThostFtdcRspInfoField*      pRspInfo,
                        int                          nRequestID,
                        bool                         bIsLast) override;
    void OnRspSubMarketData(CThostFtdcSpecificInstrumentField* pSpecificInstrument,
                            CThostFtdcRspInfoField*            pRspInfo,
                            int                                nRequestID,
                            bool                               bIsLast) override;
    void OnRtnDepthMarketData(CThostFtdcDepthMarketDataField* pDepthMarketData) override;

private:
    std::shared_ptr<MdCallbacks> callbacks() const;
    std::shared_ptr<MdState>     state_;
};

struct TdCallbacks {
    std::function<void()>    front_connected;
    std::function<void(int)> front_disconnected;
    std::function<void(
        const CThostFtdcRspAuthenticateField*, const CThostFtdcRspInfoField*, int, bool)>
        authenticate;
    std::function<void(
        const CThostFtdcRspUserLoginField&, const CThostFtdcRspInfoField*, int, bool)>
        login;
    std::function<void(
        const CThostFtdcSettlementInfoConfirmField*, const CThostFtdcRspInfoField*, int, bool)>
        settlement;
    std::function<void(const CThostFtdcInputOrderField&, const CThostFtdcRspInfoField*, int, bool)>
        order_insert;
    std::function<void(
        const CThostFtdcInputOrderActionField&, const CThostFtdcRspInfoField*, int, bool)>
        order_action;
    std::function<void(const CThostFtdcInputOrderField&, const CThostFtdcRspInfoField&)>
        err_order_insert;
    std::function<void(const CThostFtdcOrderActionField&, const CThostFtdcRspInfoField&)>
                                                     err_order_action;
    std::function<void(const CThostFtdcOrderField&)> rtn_order;
    std::function<void(const CThostFtdcTradeField&)> rtn_trade;
    std::function<void(const CThostFtdcOrderField*, const CThostFtdcRspInfoField*, int, bool)>
        qry_order;
    std::function<void(const CThostFtdcTradeField*, const CThostFtdcRspInfoField*, int, bool)>
        qry_trade;
    std::function<void(
        const CThostFtdcInvestorPositionField*, const CThostFtdcRspInfoField*, int, bool)>
        qry_position;
    std::function<void(
        const CThostFtdcTradingAccountField*, const CThostFtdcRspInfoField*, int, bool)>
        qry_account;
    std::function<void(const CThostFtdcInstrumentField*, const CThostFtdcRspInfoField*, int, bool)>
        qry_instrument;
};

struct TdState {
    explicit TdState(boost::asio::any_io_executor ex)
        : queue(std::make_shared<CtpTaskQueue>(std::move(ex))) {}

    std::shared_ptr<CtpTaskQueue> queue;
    std::atomic<bool>             alive{true};
    std::mutex                    cb_mu;
    std::shared_ptr<TdCallbacks>  callbacks;
};

class CtpTdSpi final : public CThostFtdcTraderSpi {
public:
    explicit CtpTdSpi(std::shared_ptr<TdState> state)
        : state_(std::move(state)) {}

    void OnFrontConnected() override;
    void OnFrontDisconnected(int nReason) override;
    void OnRspAuthenticate(CThostFtdcRspAuthenticateField* pRspAuthenticateField,
                           CThostFtdcRspInfoField*         pRspInfo,
                           int                             nRequestID,
                           bool                            bIsLast) override;
    void OnRspUserLogin(CThostFtdcRspUserLoginField* pRspUserLogin,
                        CThostFtdcRspInfoField*      pRspInfo,
                        int                          nRequestID,
                        bool                         bIsLast) override;
    void OnRspSettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField* pSettlementInfoConfirm,
                                    CThostFtdcRspInfoField*               pRspInfo,
                                    int                                   nRequestID,
                                    bool                                  bIsLast) override;
    void OnRspOrderInsert(CThostFtdcInputOrderField* pInputOrder,
                          CThostFtdcRspInfoField*    pRspInfo,
                          int                        nRequestID,
                          bool                       bIsLast) override;
    void OnRspOrderAction(CThostFtdcInputOrderActionField* pInputOrderAction,
                          CThostFtdcRspInfoField*          pRspInfo,
                          int                              nRequestID,
                          bool                             bIsLast) override;
    void OnErrRtnOrderInsert(CThostFtdcInputOrderField* pInputOrder,
                             CThostFtdcRspInfoField*    pRspInfo) override;
    void OnErrRtnOrderAction(CThostFtdcOrderActionField* pOrderAction,
                             CThostFtdcRspInfoField*     pRspInfo) override;
    void OnRtnOrder(CThostFtdcOrderField* pOrder) override;
    void OnRtnTrade(CThostFtdcTradeField* pTrade) override;
    void OnRspQryOrder(CThostFtdcOrderField*   pOrder,
                       CThostFtdcRspInfoField* pRspInfo,
                       int                     nRequestID,
                       bool                    bIsLast) override;
    void OnRspQryTrade(CThostFtdcTradeField*   pTrade,
                       CThostFtdcRspInfoField* pRspInfo,
                       int                     nRequestID,
                       bool                    bIsLast) override;
    void OnRspQryInvestorPosition(CThostFtdcInvestorPositionField* pInvestorPosition,
                                  CThostFtdcRspInfoField*          pRspInfo,
                                  int                              nRequestID,
                                  bool                             bIsLast) override;
    void OnRspQryTradingAccount(CThostFtdcTradingAccountField* pTradingAccount,
                                CThostFtdcRspInfoField*        pRspInfo,
                                int                            nRequestID,
                                bool                           bIsLast) override;
    void OnRspQryInstrument(CThostFtdcInstrumentField* pInstrument,
                            CThostFtdcRspInfoField*    pRspInfo,
                            int                        nRequestID,
                            bool                       bIsLast) override;

private:
    std::shared_ptr<TdCallbacks> callbacks() const;
    std::shared_ptr<TdState>     state_;
};

} // namespace ctp
} // namespace tqsdk

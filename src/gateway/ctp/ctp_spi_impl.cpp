#include "tqsdk/gateway/ctp/ctp_spi_impl.h"

namespace asio = boost::asio;

namespace tqsdk {
namespace ctp {

namespace {

template <typename T>
std::shared_ptr<T> copy_opt(const T* p) {
    return p == nullptr ? nullptr : std::make_shared<T>(*p);
}

} // namespace

std::shared_ptr<MdCallbacks> CtpMdSpi::callbacks() const {
    std::lock_guard<std::mutex> lock(state_->cb_mu);
    return state_->callbacks;
}

void CtpMdSpi::OnFrontConnected() {
    auto cb = callbacks();
    if (!cb) {
        return;
    }
    auto state = state_;
    state->queue->push([state, cb]() {
        if (state->alive.load(std::memory_order_acquire) && cb->front_connected) {
            cb->front_connected();
        }
    });
}

void CtpMdSpi::OnFrontDisconnected(int nReason) {
    auto cb = callbacks();
    if (!cb) {
        return;
    }
    auto state = state_;
    state->queue->push([state, cb, nReason]() {
        if (state->alive.load(std::memory_order_acquire) && cb->front_disconnected) {
            cb->front_disconnected(nReason);
        }
    });
}

void CtpMdSpi::OnRspUserLogin(CThostFtdcRspUserLoginField* pRspUserLogin,
                              CThostFtdcRspInfoField*      pRspInfo,
                              int                          nRequestID,
                              bool                         bIsLast) {
    auto cb = callbacks();
    if (!cb) {
        return;
    }
    auto state = state_;
    auto login = copy_opt(pRspUserLogin);
    auto info  = copy_opt(pRspInfo);
    state->queue->push([state, cb, login, info, nRequestID, bIsLast]() {
        if (state->alive.load(std::memory_order_acquire) && cb->login && login) {
            cb->login(*login, info.get(), nRequestID, bIsLast);
        }
    });
}

void CtpMdSpi::OnRspSubMarketData(CThostFtdcSpecificInstrumentField* pSpecificInstrument,
                                  CThostFtdcRspInfoField*            pRspInfo,
                                  int                                nRequestID,
                                  bool                               bIsLast) {
    auto cb = callbacks();
    if (!cb) {
        return;
    }
    auto state = state_;
    auto spec  = copy_opt(pSpecificInstrument);
    auto info  = copy_opt(pRspInfo);
    state->queue->push([state, cb, spec, info, nRequestID, bIsLast]() {
        if (state->alive.load(std::memory_order_acquire) && cb->sub_market) {
            cb->sub_market(spec.get(), info.get(), nRequestID, bIsLast);
        }
    });
}

void CtpMdSpi::OnRtnDepthMarketData(CThostFtdcDepthMarketDataField* pDepthMarketData) {
    auto cb = callbacks();
    if (!cb || pDepthMarketData == nullptr) {
        return;
    }
    auto state = state_;
    auto data  = std::make_shared<CThostFtdcDepthMarketDataField>(*pDepthMarketData);
    state->queue->push([state, cb, data]() {
        if (state->alive.load(std::memory_order_acquire) && cb->depth) {
            cb->depth(*data);
        }
    });
}

// ============================================================================
// Trader SPI
// ============================================================================

std::shared_ptr<TdCallbacks> CtpTdSpi::callbacks() const {
    std::lock_guard<std::mutex> lock(state_->cb_mu);
    return state_->callbacks;
}

void CtpTdSpi::OnFrontConnected() {
    auto cb = callbacks();
    if (!cb) {
        return;
    }
    auto state = state_;
    state->queue->push([state, cb]() {
        if (state->alive.load(std::memory_order_acquire) && cb->front_connected) {
            cb->front_connected();
        }
    });
}

void CtpTdSpi::OnFrontDisconnected(int nReason) {
    auto cb = callbacks();
    if (!cb) {
        return;
    }
    auto state = state_;
    state->queue->push([state, cb, nReason]() {
        if (state->alive.load(std::memory_order_acquire) && cb->front_disconnected) {
            cb->front_disconnected(nReason);
        }
    });
}

void CtpTdSpi::OnRspAuthenticate(CThostFtdcRspAuthenticateField* pRspAuthenticateField,
                                 CThostFtdcRspInfoField*         pRspInfo,
                                 int                             nRequestID,
                                 bool                            bIsLast) {
    auto cb = callbacks();
    if (!cb) {
        return;
    }
    auto state = state_;
    auto rsp   = copy_opt(pRspAuthenticateField);
    auto info  = copy_opt(pRspInfo);
    state->queue->push([state, cb, rsp, info, nRequestID, bIsLast]() {
        if (state->alive.load(std::memory_order_acquire) && cb->authenticate) {
            cb->authenticate(rsp.get(), info.get(), nRequestID, bIsLast);
        }
    });
}

void CtpTdSpi::OnRspUserLogin(CThostFtdcRspUserLoginField* pRspUserLogin,
                              CThostFtdcRspInfoField*      pRspInfo,
                              int                          nRequestID,
                              bool                         bIsLast) {
    auto cb = callbacks();
    if (!cb) {
        return;
    }
    auto state = state_;
    auto login = copy_opt(pRspUserLogin);
    auto info  = copy_opt(pRspInfo);
    state->queue->push([state, cb, login, info, nRequestID, bIsLast]() {
        if (state->alive.load(std::memory_order_acquire) && cb->login && login) {
            cb->login(*login, info.get(), nRequestID, bIsLast);
        }
    });
}

void CtpTdSpi::OnRspSettlementInfoConfirm(
    CThostFtdcSettlementInfoConfirmField* pSettlementInfoConfirm,
    CThostFtdcRspInfoField*               pRspInfo,
    int                                   nRequestID,
    bool                                  bIsLast) {
    auto cb = callbacks();
    if (!cb) {
        return;
    }
    auto state = state_;
    auto rsp   = copy_opt(pSettlementInfoConfirm);
    auto info  = copy_opt(pRspInfo);
    state->queue->push([state, cb, rsp, info, nRequestID, bIsLast]() {
        if (state->alive.load(std::memory_order_acquire) && cb->settlement) {
            cb->settlement(rsp.get(), info.get(), nRequestID, bIsLast);
        }
    });
}

void CtpTdSpi::OnRspOrderInsert(CThostFtdcInputOrderField* pInputOrder,
                                CThostFtdcRspInfoField*    pRspInfo,
                                int                        nRequestID,
                                bool                       bIsLast) {
    auto cb = callbacks();
    if (!cb) {
        return;
    }
    auto state = state_;
    auto input = copy_opt(pInputOrder);
    auto info  = copy_opt(pRspInfo);
    state->queue->push([state, cb, input, info, nRequestID, bIsLast]() {
        if (state->alive.load(std::memory_order_acquire) && cb->order_insert && input) {
            cb->order_insert(*input, info.get(), nRequestID, bIsLast);
        }
    });
}

void CtpTdSpi::OnRspOrderAction(CThostFtdcInputOrderActionField* pInputOrderAction,
                                CThostFtdcRspInfoField*          pRspInfo,
                                int                              nRequestID,
                                bool                             bIsLast) {
    auto cb = callbacks();
    if (!cb) {
        return;
    }
    auto state = state_;
    auto input = copy_opt(pInputOrderAction);
    auto info  = copy_opt(pRspInfo);
    state->queue->push([state, cb, input, info, nRequestID, bIsLast]() {
        if (state->alive.load(std::memory_order_acquire) && cb->order_action && input) {
            cb->order_action(*input, info.get(), nRequestID, bIsLast);
        }
    });
}

void CtpTdSpi::OnErrRtnOrderInsert(CThostFtdcInputOrderField* pInputOrder,
                                   CThostFtdcRspInfoField*    pRspInfo) {
    auto cb = callbacks();
    if (!cb || pInputOrder == nullptr || pRspInfo == nullptr) {
        return;
    }
    auto state = state_;
    auto input = std::make_shared<CThostFtdcInputOrderField>(*pInputOrder);
    auto info  = std::make_shared<CThostFtdcRspInfoField>(*pRspInfo);
    state->queue->push([state, cb, input, info]() {
        if (state->alive.load(std::memory_order_acquire) && cb->err_order_insert) {
            cb->err_order_insert(*input, *info);
        }
    });
}

void CtpTdSpi::OnErrRtnOrderAction(CThostFtdcOrderActionField* pOrderAction,
                                   CThostFtdcRspInfoField*     pRspInfo) {
    auto cb = callbacks();
    if (!cb || pOrderAction == nullptr || pRspInfo == nullptr) {
        return;
    }
    auto state  = state_;
    auto action = std::make_shared<CThostFtdcOrderActionField>(*pOrderAction);
    auto info   = std::make_shared<CThostFtdcRspInfoField>(*pRspInfo);
    state->queue->push([state, cb, action, info]() {
        if (state->alive.load(std::memory_order_acquire) && cb->err_order_action) {
            cb->err_order_action(*action, *info);
        }
    });
}

void CtpTdSpi::OnRtnOrder(CThostFtdcOrderField* pOrder) {
    auto cb = callbacks();
    if (!cb || pOrder == nullptr) {
        return;
    }
    auto state = state_;
    auto order = std::make_shared<CThostFtdcOrderField>(*pOrder);
    state->queue->push([state, cb, order]() {
        if (state->alive.load(std::memory_order_acquire) && cb->rtn_order) {
            cb->rtn_order(*order);
        }
    });
}

void CtpTdSpi::OnRtnTrade(CThostFtdcTradeField* pTrade) {
    auto cb = callbacks();
    if (!cb || pTrade == nullptr) {
        return;
    }
    auto state = state_;
    auto trade = std::make_shared<CThostFtdcTradeField>(*pTrade);
    state->queue->push([state, cb, trade]() {
        if (state->alive.load(std::memory_order_acquire) && cb->rtn_trade) {
            cb->rtn_trade(*trade);
        }
    });
}

void CtpTdSpi::OnRspQryOrder(CThostFtdcOrderField*   pOrder,
                             CThostFtdcRspInfoField* pRspInfo,
                             int                     nRequestID,
                             bool                    bIsLast) {
    auto cb = callbacks();
    if (!cb) {
        return;
    }
    auto state = state_;
    auto order = copy_opt(pOrder);
    auto info  = copy_opt(pRspInfo);
    state->queue->push([state, cb, order, info, nRequestID, bIsLast]() {
        if (state->alive.load(std::memory_order_acquire) && cb->qry_order) {
            cb->qry_order(order.get(), info.get(), nRequestID, bIsLast);
        }
    });
}

void CtpTdSpi::OnRspQryTrade(CThostFtdcTradeField*   pTrade,
                             CThostFtdcRspInfoField* pRspInfo,
                             int                     nRequestID,
                             bool                    bIsLast) {
    auto cb = callbacks();
    if (!cb) {
        return;
    }
    auto state = state_;
    auto trade = copy_opt(pTrade);
    auto info  = copy_opt(pRspInfo);
    state->queue->push([state, cb, trade, info, nRequestID, bIsLast]() {
        if (state->alive.load(std::memory_order_acquire) && cb->qry_trade) {
            cb->qry_trade(trade.get(), info.get(), nRequestID, bIsLast);
        }
    });
}

void CtpTdSpi::OnRspQryInvestorPosition(CThostFtdcInvestorPositionField* pInvestorPosition,
                                        CThostFtdcRspInfoField*          pRspInfo,
                                        int                              nRequestID,
                                        bool                             bIsLast) {
    auto cb = callbacks();
    if (!cb) {
        return;
    }
    auto state = state_;
    auto pos   = copy_opt(pInvestorPosition);
    auto info  = copy_opt(pRspInfo);
    state->queue->push([state, cb, pos, info, nRequestID, bIsLast]() {
        if (state->alive.load(std::memory_order_acquire) && cb->qry_position) {
            cb->qry_position(pos.get(), info.get(), nRequestID, bIsLast);
        }
    });
}

void CtpTdSpi::OnRspQryTradingAccount(CThostFtdcTradingAccountField* pTradingAccount,
                                      CThostFtdcRspInfoField*        pRspInfo,
                                      int                            nRequestID,
                                      bool                           bIsLast) {
    auto cb = callbacks();
    if (!cb) {
        return;
    }
    auto state = state_;
    auto acct  = copy_opt(pTradingAccount);
    auto info  = copy_opt(pRspInfo);
    state->queue->push([state, cb, acct, info, nRequestID, bIsLast]() {
        if (state->alive.load(std::memory_order_acquire) && cb->qry_account) {
            cb->qry_account(acct.get(), info.get(), nRequestID, bIsLast);
        }
    });
}

void CtpTdSpi::OnRspQryInstrument(CThostFtdcInstrumentField* pInstrument,
                                  CThostFtdcRspInfoField*    pRspInfo,
                                  int                        nRequestID,
                                  bool                       bIsLast) {
    auto cb = callbacks();
    if (!cb) {
        return;
    }
    auto state = state_;
    auto inst  = copy_opt(pInstrument);
    auto info  = copy_opt(pRspInfo);
    state->queue->push([state, cb, inst, info, nRequestID, bIsLast]() {
        if (state->alive.load(std::memory_order_acquire) && cb->qry_instrument) {
            cb->qry_instrument(inst.get(), info.get(), nRequestID, bIsLast);
        }
    });
}

} // namespace ctp
} // namespace tqsdk

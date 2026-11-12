#pragma once

#include "tqsdk/gateway/ctp/ctp_config.h"
#include "tqsdk/gateway/ctp/ctp_spi_impl.h"
#include "tqsdk/gateway/market_data_gateway.h"

#include <boost/asio/steady_timer.hpp>

namespace tqsdk {

/// CTP 行情网关。
///
/// - CTP MdApi 自带断线重连；OnFrontConnected 会再次触发 login + 重放订阅。
/// - CTP 不提供 K 线序列，subscribe_kline 为 no-op；K 线由 TqMdGateway 提供。
/// - 所有 SPI 回调经 CtpTaskQueue 切回 ex（core strand）后执行。
class CtpMdGateway final : public MarketDataGateway {
public:
    CtpMdGateway(boost::asio::any_io_executor ex, CtpConfig cfg);
    ~CtpMdGateway() override;

    boost::asio::awaitable<void> start() override;
    boost::asio::awaitable<void> stop() override;
    boost::asio::awaitable<void> subscribe(const std::vector<std::string>& symbols) override;
    boost::asio::awaitable<void>
    release(const std::vector<std::string>&                     symbols,
            const std::vector<std::pair<std::string, int64_t>>& klines) override;

    Channel<MarketDataEvent>& output() override { return ch_; }

    // SPI 回调入口（运行在 core strand 上）。
    void handle_front_connected();
    void handle_front_disconnected(int reason);
    void handle_login(const CThostFtdcRspUserLoginField& rsp,
                      const CThostFtdcRspInfoField*      info,
                      int                                request_id,
                      bool                               is_last);
    void handle_sub_market(const CThostFtdcSpecificInstrumentField* spec,
                           const CThostFtdcRspInfoField*            info,
                           int                                      request_id,
                           bool                                     is_last);
    void handle_depth(const CThostFtdcDepthMarketDataField& depth);

private:
    void install_callbacks();
    void emit(MarketDataEvent ev);
    void stop_impl() noexcept;
    void resubscribe_desired();
    void arm_watchdog();
    void disarm_watchdog();
    void handle_watchdog();
    void restart_api();

    boost::asio::any_io_executor   ex_;
    CtpConfig                      cfg_;
    Channel<MarketDataEvent>       ch_;
    std::shared_ptr<ctp::MdState>  state_;
    std::unique_ptr<ctp::CtpMdSpi> spi_;
    CThostFtdcMdApi*               api_             = nullptr;
    bool                           started_         = false;
    bool                           stopped_         = false;
    bool                           logged_in_       = false;
    bool                           front_connected_ = false;
    std::map<std::string, int>     desired_;

    // CTP MD 同样可能在“front connected 后登录回调不返回”时永久卡住。
    boost::asio::steady_timer          watchdog_timer_;
    std::shared_ptr<std::atomic<bool>> watchdog_alive_ = std::make_shared<std::atomic<bool>>(true);
    std::uint64_t                      watchdog_generation_ = 0;
};

} // namespace tqsdk

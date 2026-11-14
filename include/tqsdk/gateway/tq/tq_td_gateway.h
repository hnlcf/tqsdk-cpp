#pragma once

#include "tqsdk/pch.h"

#include "tqsdk/gateway/tq/tq_protocol.h"
#include "tqsdk/gateway/trading_gateway.h"
#include "tqsdk/util/async_mutex.h"

#include <boost/asio/ssl.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

namespace tqsdk {

/// 把 OTG `rtn_data` 中 trade 外层柜台 user_id 映射成本地 account_key，
/// 并补齐 DiffMergeEngine 要求的 `trade[account_key].user` 层。
nlohmann::json normalize_td_trade_slice(const nlohmann::json& slice, std::string_view account_key);

/// 判断一个 TD slice 是否已收到完整交易截面（trade_more_data == false）。
bool td_trade_complete(const nlohmann::json& slice) noexcept;

/// 天勤 OTG 交易网关（WebSocket JSON 协议）。
///
/// 协议参考官方 tqsdk-python：
///   - TqAccount 发送 req_login + confirm_settlement；
///   - BaseOtg 将 insert_order/cancel_order/pre_insert_order/
///     set_risk_management_rule 路由到 TD 通道；
///   - TdReconnectHandler 在重连后重发登录并等待 trade_more_data=false。
///
/// 本实现只负责传输、登录、peek、交易报文和 diff/snapshot 翻译；
/// 订单状态推进仍由 TqApi::td_loop -> DiffMergeEngine 完成。
class TqTdGateway final : public TradingGateway {
public:
    struct Options {
        /// wss://host[:port]/path；TQ OTG 当前使用 wss。
        std::string url;
        /// "Bearer <access_token>"；由 TqAuth::authorization_header() 提供。
        std::string               authorization;
        std::string               broker_id;
        std::string               account_id;
        std::string               password;
        std::string               front_broker;
        std::string               front_url;
        std::string               account_key;
        std::string               client_app_id = "SHINNY_TQ_1.0";
        std::string               client_mac_address;
        std::chrono::milliseconds reconnect_delay{500};
        std::chrono::milliseconds max_reconnect_delay{10'000};
    };

    TqTdGateway(asio::any_io_executor ex, Options options);
    ~TqTdGateway() override;

    TqTdGateway(const TqTdGateway&)            = delete;
    TqTdGateway& operator=(const TqTdGateway&) = delete;

    asio::awaitable<void>       start() override;
    asio::awaitable<void>       stop() override;
    asio::awaitable<CommandAck> insert_order(const InsertOrderReq& req,
                                             const OrderId&        rid) override;
    asio::awaitable<CommandAck> cancel_order(const CancelOrderReq& req) override;

    Channel<TradeEvent>& event_output() override { return ch_; }
    nlohmann::json       snapshot() const override { return snapshot_; }
    bool                 snapshot_ready() const override { return snapshot_ready_; }
    bool                 requires_initial_trade_snapshot() const override { return true; }
    nlohmann::json       metrics() const override;
    bool                 is_open() const noexcept;

private:
    struct Url {
        std::string host;
        std::string port   = "443";
        std::string target = "/";
    };

    static Url parse_url(std::string_view url);

    asio::awaitable<void> connect_loop();
    asio::awaitable<void> connect_once();
    asio::awaitable<void> read_loop();
    asio::awaitable<bool> send_json(const nlohmann::json& message);
    asio::awaitable<void> send_peek();

    /// 把 rtn_data 的 trade 外层 user_id 换成 account_key。
    nlohmann::json normalize_trade_keys(const nlohmann::json& slice);
    /// 判断 slice 中 trade[*].trade_more_data 是否已为 false。
    bool trade_complete(const nlohmann::json& slice) const;
    /// 把 pending snapshot diffs 深度 merge 成单个 diff。
    static void           merge_json(nlohmann::json& dst, const nlohmann::json& src);
    asio::awaitable<void> flush_snapshot();

    asio::any_io_executor ex_;
    Options               opts_;
    Channel<TradeEvent>   ch_;

    using tcp_t     = asio::ip::tcp;
    using ssl_ctx_t = asio::ssl::context;
    using ws_t      = boost::beast::websocket::stream<asio::ssl::stream<tcp_t::socket>>;

    ssl_ctx_t                      ssl_ctx_;
    std::optional<ws_t>            ws_;
    std::optional<tcp_t::resolver> resolver_;
    boost::beast::flat_buffer      read_buf_;
    AsyncMutex                     write_mutex_;

    std::atomic<bool>           stopped_{false};
    std::atomic<bool>           connected_{false};
    bool                        snapshot_ready_ = false;
    nlohmann::json              snapshot_       = nlohmann::json::object();
    std::vector<nlohmann::json> pending_diffs_;
    nlohmann::json              capabilities_ = nlohmann::json::object();
};

} // namespace tqsdk

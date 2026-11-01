#pragma once

#include "tqsdk/pch.h"

#include <boost/asio/ssl.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include <cstdint>
#include <random>

#include "tqsdk/gateway/market_data_gateway.h"
#include "tqsdk/util/async_mutex.h"

namespace tqsdk {

/// TqMdGateway：对接天勤（TqSdk）免费行情 WebSocket 服务器。
///
/// 目标地址（默认）：wss://free-openmd.shinnytech.com/t/md/front/mobile
/// 协议要点（经真实连接探测确认）：
///   - 无需登录即可取行情；服务器会主动下发 rsp_login（匿名会话）。
///   - 订阅：{"aid":"subscribe_quote","ins_list":"SHFE.rb2610,SHFE.rb"}
///   - 拉取：每发一次 {"aid":"peek_message"}，服务器回一帧
///           {"aid":"rtn_data","data":[patch,...]}；读完一帧需再发 peek，
///           否则停推（流控）。网关内部用一个 peek 循环协程处理。
///   - 翻译：data[] 逐条合并成单条 quotes diff（amount->turnover、裁 10->5 档），
///           作为一个 MarketDataEvent{Diff} 投递（一次 rtn_data = 一个事务）。
///
/// 仅为行情网关；交易网关（TqTdGateway）在后续阶段实现，二者共享同一
/// MarketDataGateway / TradingGateway 抽象，便于将来平行接入 CTP。
class TqMdGateway : public MarketDataGateway {
public:
    /// host + ":" + port 形式的目标地址解析已由构造参数给出 path 前缀。
    /// 默认构造使用官方免费行情地址。
    explicit TqMdGateway(asio::any_io_executor ex,
                         std::string           host = "free-openmd.shinnytech.com",
                         std::string           port = "443",
                         std::string           path = "/t/md/front/mobile");

    ~TqMdGateway() override;

    asio::awaitable<void> start() override;
    asio::awaitable<void> stop() override;

    asio::awaitable<void> subscribe(const std::vector<std::string>& symbols) override;

    asio::awaitable<void>
    subscribe_kline(const std::string& symbol, int64_t duration_ns, int view_width) override;

    Channel<MarketDataEvent>& output() override { return ch_; }

private:
    // 一条 K 线订阅（用于连接就绪 / 重连后补发 set_chart）。
    struct KlineSub {
        std::string chart_id;
        std::string symbol;
        int64_t     duration_ns = 0;
        int         view_width  = 200;
    };

    // 发送 set_chart 指令并登记/更新订阅。
    asio::awaitable<void> do_set_chart(const KlineSub& sub);

    // 建立 wss 连接 + TLS 握手 + websocket 握手，并启动读写循环。
    asio::awaitable<void> do_connect_and_run();

    // peek 循环：发 peek / 收帧 / 翻译 / 投递 / 再 peek，直到断开或关闭。
    asio::awaitable<void> read_loop();

    // 发送一条控制指令（subscribe_quote / peek_message 等）。
    asio::awaitable<void> send_cmd(nlohmann::json cmd);

    // 把一条 rtn_data 的 data[] 翻译并作为 Diff 投递；返回投递是否成功（未关闭）。
    asio::awaitable<void> deliver_rtn_data(const nlohmann::json& data_array);

    asio::any_io_executor    ex_;
    Channel<MarketDataEvent> ch_;

    std::string host_;
    std::string port_;
    std::string path_;

    // 生成符合服务器要求的 chart_id（32 位小写十六进制，无特殊字符）。
    // 对齐官方 TqSdk：_generate_uuid() -> f"{getrandbits(128):032x}"。
    std::string make_chart_id();

    // 已请求订阅的标的（连接就绪后统一发送；重连后重发）。
    std::set<std::string> pending_subs_;

    // 已请求订阅的 K 线（chart_id -> 订阅参数；连接就绪后统一补发 set_chart）。
    // 额外维护 (symbol,duration) -> chart_id 映射，使同一序列复用稳定 chart_id。
    std::map<std::string, KlineSub>    pending_klines_;
    std::map<std::string, std::string> kline_key_to_chart_;

    // chart_id 随机源。
    std::mt19937_64 rng_{std::random_device{}()};

    // websocket over TLS。
    using tcp_t     = boost::asio::ip::tcp;
    using ssl_ctx_t = boost::asio::ssl::context;
    using ws_t      = boost::beast::websocket::stream<boost::asio::ssl::stream<tcp_t::socket>>;

    ssl_ctx_t                      ssl_ctx_;
    std::optional<ws_t>            ws_;
    std::optional<tcp_t::resolver> resolver_;
    boost::beast::flat_buffer      read_buf_;

    std::atomic<bool> stopped_{false};

    // 串行化所有 ws 写操作（Beast 不允许并发 async_write）。
    AsyncMutex write_mutex_;
};

} // namespace tqsdk

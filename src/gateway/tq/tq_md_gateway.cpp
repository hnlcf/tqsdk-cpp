#include "tqsdk/gateway/tq/tq_md_gateway.h"

#include "tqsdk/gateway/tq/tq_md_protocol.h"
#include "tqsdk/util/spawn_logged.h"

#include <cstdio>

namespace tqsdk {

namespace asio  = boost::asio;
namespace beast = boost::beast;
namespace ssl   = boost::asio::ssl;
using boost::asio::use_awaitable;

TqMdGateway::TqMdGateway(asio::any_io_executor ex,
                         std::string           host,
                         std::string           port,
                         std::string           path)
    : ex_(ex),
      ch_(ex),
      host_(std::move(host)),
      port_(std::move(port)),
      path_(std::move(path)),
      ssl_ctx_(ssl::context::tls_client),
      write_mutex_(ex_) {
    ssl_ctx_.set_default_verify_paths();
    ssl_ctx_.set_verify_mode(ssl::verify_peer);
}

TqMdGateway::~TqMdGateway() {
    // 契约：析构前应已 stop() 且无挂起协程。这里兜底关闭通道。
    ch_.close();
}

asio::awaitable<void> TqMdGateway::start() {
    // 启动连接 + 读循环（异步进行，不阻塞调用方）。
    co_spawn_logged(
        ex_, [this]() -> asio::awaitable<void> { co_await do_connect_and_run(); }, "TqMd.connect");
    co_return;
}

asio::awaitable<void> TqMdGateway::stop() {
    stopped_ = true;
    // 关闭 websocket（若已连接），中断挂起的读写。
    if (ws_) {
        boost::system::error_code ec;
        co_await ws_->async_close(beast::websocket::close_code::normal,
                                  asio::redirect_error(use_awaitable, ec));
    }
    // 关闭输出通道，使 TqApi 的 md_loop 以 nullopt 退出。
    ch_.close();
    co_return;
}

asio::awaitable<void> TqMdGateway::subscribe(const std::vector<std::string>& symbols) {
    bool added = false;
    for (const auto& s : symbols) {
        if (pending_subs_.insert(s).second) {
            added = true;
        }
    }
    // 若已连接，立即发送订阅；否则等连接建立时统一发送。
    if (added && ws_ && ws_->is_open()) {
        std::string ins_list;
        for (const auto& s : pending_subs_) {
            if (!ins_list.empty())
                ins_list.push_back(',');
            ins_list += s;
        }
        co_await send_cmd(nlohmann::json{{"aid", "subscribe_quote"}, {"ins_list", ins_list}});
    }
    co_return;
}

asio::awaitable<void> TqMdGateway::send_cmd(nlohmann::json cmd) {
    if (!ws_ || !ws_->is_open()) {
        co_return;
    }
    auto payload = cmd.dump();
    SPDLOG_TRACE("[TqMd] sent: {}", payload);

    // 串行化写入：Beast 不允许并发 async_write。
    auto guard = co_await AsyncMutex::acquire(write_mutex_);
    if (!ws_ || !ws_->is_open()) {
        co_return;
    }
    co_await ws_->async_write(asio::buffer(payload), use_awaitable);
    co_return;
}

asio::awaitable<void> TqMdGateway::do_set_chart(const KlineSub& sub) {
    co_await send_cmd(nlohmann::json{{"aid", "set_chart"},
                                     {"chart_id", sub.chart_id},
                                     {"ins_list", sub.symbol},
                                     {"duration", sub.duration_ns},
                                     {"view_width", sub.view_width}});
    co_return;
}

std::string TqMdGateway::make_chart_id() {
    // 128 位随机数格式化为 32 位小写十六进制，对齐官方 TqSdk _generate_uuid()。
    uint64_t hi = rng_();
    uint64_t lo = rng_();
    char     buf[33];
    std::snprintf(buf,
                  sizeof(buf),
                  "%016llx%016llx",
                  static_cast<unsigned long long>(hi),
                  static_cast<unsigned long long>(lo));
    return std::string(buf, 32);
}

asio::awaitable<void>
TqMdGateway::subscribe_kline(const std::string& symbol, int64_t duration_ns, int view_width) {
    if (view_width <= 0) {
        view_width = 200;
    }
    // 同一 (symbol, duration) 序列复用稳定 chart_id；新序列才生成 UUID。
    std::string key = std::string(symbol) + "|" + std::to_string(duration_ns);
    auto        it  = kline_key_to_chart_.find(key);
    std::string chart_id;
    if (it != kline_key_to_chart_.end()) {
        chart_id = it->second;
    } else {
        chart_id                 = make_chart_id();
        kline_key_to_chart_[key] = chart_id;
    }

    KlineSub sub{chart_id, symbol, duration_ns, view_width};
    pending_klines_[chart_id] = sub;

    // K 线依赖合约已加载，先确保该合约的行情订阅存在。
    bool need_quote = pending_subs_.insert(symbol).second;

    if (ws_ && ws_->is_open()) {
        if (need_quote) {
            std::string ins_list;
            for (const auto& s : pending_subs_) {
                if (!ins_list.empty())
                    ins_list.push_back(',');
                ins_list += s;
            }
            co_await send_cmd(nlohmann::json{{"aid", "subscribe_quote"}, {"ins_list", ins_list}});
        }
        // 发 set_chart（首次新增或刷新 view_width）。
        co_await do_set_chart(sub);
    }
    co_return;
}

asio::awaitable<void> TqMdGateway::do_connect_and_run() {
    std::string fatal_error; // 在 catch 中记录，try/catch 之外再投递（co_await 不能在 catch 内）
    try {
        resolver_.emplace(ex_);
        auto results = co_await resolver_->async_resolve(host_, port_, use_awaitable);
        if (results.empty()) {
            SPDLOG_ERROR("[TqMd] resolve {}:{} returned empty", host_, port_);
            co_return;
        }

        ws_.emplace(asio::ssl::stream<tcp_t::socket>(ex_, ssl_ctx_));

        // TCP 连接。
        co_await asio::async_connect(beast::get_lowest_layer(*ws_), results, use_awaitable);

        // SNI + 证书主机名校验。
        if (!SSL_set_tlsext_host_name(ws_->next_layer().native_handle(), host_.c_str())) {
            SPDLOG_ERROR("[TqMd] failed to set SNI hostname");
            co_return;
        }
        ws_->next_layer().set_verify_callback(ssl::host_name_verification(host_));

        // TLS 握手。
        co_await ws_->next_layer().async_handshake(ssl::stream_base::client, use_awaitable);

        // WebSocket 握手（指定 host 头）。
        ws_->set_option(
            beast::websocket::stream_base::timeout::suggested(beast::role_type::client));
        // 服务器强制要求 permessage-deflate 扩展，否则握手返回 400。
        beast::websocket::permessage_deflate pmd;
        pmd.client_enable          = true;
        pmd.server_enable          = true;
        pmd.client_max_window_bits = 15;
        ws_->set_option(pmd);
        ws_->set_option(beast::websocket::stream_base::decorator(
            [h = host_](beast::websocket::request_type& req) {
                req.set(beast::http::field::host, h);
            }));
        co_await ws_->async_handshake(host_, path_, use_awaitable);

        SPDLOG_INFO("[TqMd] connected to {}{}", host_, path_);

        // 连接就绪：补发已有订阅。
        if (!pending_subs_.empty()) {
            std::string ins_list;
            for (const auto& s : pending_subs_) {
                if (!ins_list.empty())
                    ins_list.push_back(',');
                ins_list += s;
            }
            co_await send_cmd(nlohmann::json{{"aid", "subscribe_quote"}, {"ins_list", ins_list}});
        }
        // 补发已有 K 线订阅。
        for (const auto& [cid, sub] : pending_klines_) {
            co_await do_set_chart(sub);
        }

        // 进入读循环（阻塞直到断开/关闭）。
        co_await read_loop();
    } catch (const boost::system::system_error& e) {
        if (e.code() == asio::error::operation_aborted) {
            SPDLOG_DEBUG("[TqMd] connect/read aborted (shutdown)");
        } else if (e.code() == beast::websocket::error::closed) {
            SPDLOG_INFO("[TqMd] websocket closed by server");
        } else {
            SPDLOG_ERROR("[TqMd] connect error: {} ({})", e.what(), e.code().value());
            fatal_error = std::string("connect error: ") + e.what();
        }
    } catch (const std::exception& e) {
        SPDLOG_ERROR("[TqMd] exception: {}", e.what());
        fatal_error = std::string("exception: ") + e.what();
    }

    // try/catch 之外投递（co_await 不能在 catch 内使用）。
    if (!fatal_error.empty() && !stopped_) {
        MarketDataEvent ev{MarketDataEvent::Kind::Error, nlohmann::json{}, std::move(fatal_error)};
        co_await ch_.async_send(std::move(ev));
    } else if (!stopped_) {
        // 断线：通知上游（阶段4 暂不自动重连，留作 TODO）。
        MarketDataEvent ev{MarketDataEvent::Kind::Disconnected, nlohmann::json{}, {}};
        co_await ch_.async_send(std::move(ev));
    }
    // 连接结束：关闭通道让 md_loop 退出。
    ch_.close();
    co_return;
}

asio::awaitable<void> TqMdGateway::read_loop() {
    while (!stopped_) {
        // 请求一帧：先 peek，再读。
        co_await send_cmd(nlohmann::json{{"aid", "peek_message"}});

        read_buf_.consume(read_buf_.size());
        co_await ws_->async_read(read_buf_, use_awaitable);

        auto           text = beast::buffers_to_string(read_buf_.data());
        nlohmann::json frame;
        try {
            frame = nlohmann::json::parse(text);
        } catch (const std::exception& e) {
            SPDLOG_WARN("[TqMd] bad json frame: {}", e.what());
            continue;
        }

        const std::string aid = frame.value("aid", "");
        if (aid == "rtn_data") {
            if (frame.contains("data") && frame["data"].is_array()) {
                co_await deliver_rtn_data(frame["data"]);
            }
        } else if (aid == "rsp_login") {
            SPDLOG_DEBUG("[TqMd] login session: {}", frame.value("session_id", ""));
        } else {
            // 其他 rtn_* / 通知帧：阶段4 忽略。
        }
    }
    co_return;
}

asio::awaitable<void> TqMdGateway::deliver_rtn_data(const nlohmann::json& data_array) {
    nlohmann::json merged = normalize_rtn_data(data_array);
    if (merged.empty()) {
        co_return; // 本帧无行情数据面内容
    }
    MarketDataEvent ev{MarketDataEvent::Kind::Diff, std::move(merged), {}};
    co_await ch_.async_send(std::move(ev));
    co_return;
}

} // namespace tqsdk

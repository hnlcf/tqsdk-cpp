#include "tqsdk/gateway/tq/tq_md_gateway.h"

#include "tqsdk/gateway/tq/tq_md_protocol.h"
#include "tqsdk/gateway/tq/tq_protocol.h"
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
      ch_(ex, kOutputCapacity),
      host_(std::move(host)),
      port_(std::move(port)),
      path_(std::move(path)),
      ssl_ctx_(ssl::context::tls_client),
      write_mutex_(ex_),
      peek_wakeup_(ex) {
    ssl_ctx_.set_default_verify_paths();
    ssl_ctx_.set_verify_mode(ssl::verify_peer);
    // 默认挂起（无订阅）；订阅到来时 cancel 唤醒 read_loop。
    peek_wakeup_.expires_at(std::chrono::steady_clock::time_point::max());
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
    // 直接关闭最底层 socket：立即中断挂起的 async_read / async_write，使其以
    // operation_aborted 返回。相比有序的 ws_->async_close()，这不会因为对端无响应
    // 或读侧未排空而卡住——这对优雅退出（io_context 必须能排空）至关重要。
    if (ws_) {
        boost::system::error_code ec;
        beast::get_lowest_layer(*ws_).shutdown(tcp_t::socket::shutdown_both, ec);
        beast::get_lowest_layer(*ws_).close(ec);
    }
    // 若仍在解析阶段，取消解析，令 async_resolve 以 operation_aborted 返回。
    if (resolver_) {
        boost::system::error_code ec;
        resolver_->cancel();
    }
    // 唤醒可能因"无订阅"而挂起的 peek 闸门，使 read_loop 能察觉到 stopped_ 并退出。
    peek_wakeup_.cancel();
    // 关闭输出通道，使 TqApi 的 md_loop 以 nullopt 退出。
    ch_.close();
    co_return;
}

asio::awaitable<void> TqMdGateway::subscribe(const std::vector<std::string>& symbols) {
    bool added = false;
    for (const auto& s : symbols) {
        // 引用计数：首次（0->1）才真正加入 pending 并发 subscribe_quote。
        if (++quote_ref_[s] == 1) {
            if (pending_subs_.insert(s).second) {
                added = true;
            }
        }
    }
    // 新订阅到来：唤醒可能因"无订阅"而挂起的 peek 循环。
    wake_peek();
    // 若已连接，立即发送订阅；否则等连接建立时统一发送。
    if (added && ws_ && ws_->is_open()) {
        co_await send_subscribe_quote();
    }
    co_return;
}

std::string TqMdGateway::kline_key(const std::string& symbol, int64_t duration_ns) {
    return fmt::format("{}|{}", symbol, duration_ns);
}

bool TqMdGateway::has_any_subscription() const noexcept {
    return !quote_ref_.empty() || !chart_ref_.empty();
}

void TqMdGateway::wake_peek() noexcept {
    // 令 read_loop 上挂起的 async_wait 立即返回（overwrites 极远到期点）。
    peek_wakeup_.expires_at(std::chrono::steady_clock::time_point::min());
}

asio::awaitable<void> TqMdGateway::send_subscribe_quote() {
    std::string ins_list;
    for (const auto& s : pending_subs_) {
        if (!ins_list.empty()) {
            ins_list.push_back(',');
        }
        ins_list += s;
    }
    co_await send_cmd(tq::serialize(tq::Message{tq::SubscribeQuote{ins_list}}));
}

asio::awaitable<void>
TqMdGateway::release(const std::vector<std::string>&                     symbols,
                     const std::vector<std::pair<std::string, int64_t>>& klines) {
    bool quote_changed = false;
    for (const auto& s : symbols) {
        auto it = quote_ref_.find(s);
        if (it == quote_ref_.end()) {
            continue;
        }
        if (--it->second <= 0) {
            quote_ref_.erase(it);
            if (pending_subs_.erase(s) > 0) {
                quote_changed = true;
            }
        }
    }
    for (const auto& [sym, dur] : klines) {
        auto kit = kline_key_to_chart_.find(kline_key(sym, dur));
        if (kit == kline_key_to_chart_.end()) {
            continue;
        }
        const std::string& cid = kit->second;
        auto               it  = chart_ref_.find(cid);
        if (it == chart_ref_.end()) {
            continue;
        }
        if (--it->second <= 0) {
            chart_ref_.erase(it);
            pending_klines_.erase(cid);
        }
    }
    // 天勤无软退订：仅当仍有订阅时，用剩余的 pending_subs_ 重发一次全量
    // subscribe_quote（覆盖语义，等效于缩小订阅集合）；若已无任何订阅则不再发送，
    // 交给 read_loop 的 peek 闸门自然停推。
    if (quote_changed && ws_ && ws_->is_open() && !pending_subs_.empty()) {
        co_await send_subscribe_quote();
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
    tq::SetChart chart;
    chart.chart_id   = sub.chart_id;
    chart.ins_list   = sub.symbol;
    chart.duration   = sub.duration_ns;
    chart.view_width = sub.view_width;
    co_await send_cmd(tq::serialize(tq::Message{std::move(chart)}));
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
    std::string key = kline_key(symbol, duration_ns);
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

    // chart 引用计数：首次（0->1）才真正需要向服务器 set_chart。
    bool chart_added = (++chart_ref_[chart_id] == 1);

    // 新订阅：唤醒可能挂起的 peek 循环。
    wake_peek();

    // 注意：K 线依赖其合约行情，但此处【不】再自行增加 quote_ref_。合约的行情订阅
    // 由调用方（GatewayIpcServer::handle_subscribe）以显式 subscribe() 统一驱动，
    // 保证网关引用计数与连接的 release() 严格对称（避免 subscribe_kline 额外 +1
    // 导致 release 永远减不到 0 的泄漏）。
    bool need_quote = (pending_subs_.count(symbol) == 0);
    if (need_quote) {
        pending_subs_.insert(symbol);
    }

    if (ws_ && ws_->is_open()) {
        if (need_quote) {
            co_await send_subscribe_quote();
        }
        // 发 set_chart（首次新增或刷新 view_width）。
        if (chart_added) {
            co_await do_set_chart(sub);
        }
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
                if (!ins_list.empty()) {
                    ins_list.push_back(',');
                }
                ins_list += s;
            }
            co_await send_cmd(tq::serialize(tq::Message{tq::SubscribeQuote{ins_list}}));
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
        // peek 闸门：无任何订阅时不发 peek（否则服务器会持续推流，堆积到无人消费的通道）。
        // 挂起在 peek_wakeup_ 上，订阅到来时 cancel 唤醒。
        if (!has_any_subscription()) {
            // 复位为"极远"（可能被上一次 wake 设为 min）。
            peek_wakeup_.expires_at(std::chrono::steady_clock::time_point::max());
            boost::system::error_code wec;
            co_await peek_wakeup_.async_wait(asio::redirect_error(use_awaitable, wec));
            continue; // 醒来后重新判断（有订阅则 peek，否则再挂起）
        }

        // 请求一帧：先 peek，再读。
        co_await send_cmd(tq::serialize(tq::Message{tq::PeekMessage{}}));

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
            auto parsed_frame = tq::parse(frame);
            if (!parsed_frame) {
                SPDLOG_WARN("[TqMd] unsupported/invalid TQ frame: {}", parsed_frame.error().detail);
                continue;
            }
            if (auto* rtn = std::get_if<tq::RtnData>(&*parsed_frame); rtn != nullptr) {
                co_await deliver_rtn_data(rtn->data);
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
    // 先用帧内的 charts.<id>.state 校正本地 (symbol,duration)->chart_id 映射。
    // 该 state 只在 chart 建立/变更时下发，不能假设每帧都有；但一旦出现即最权威。
    for (const auto& patch : data_array) {
        if (!patch.contains("charts") || !patch["charts"].is_object()) {
            continue;
        }
        for (auto it = patch["charts"].begin(); it != patch["charts"].end(); ++it) {
            const std::string& cid = it.key();
            if (!it.value().contains("state") || !it.value()["state"].is_object()) {
                continue;
            }
            const auto& st = it.value()["state"];
            if (st.contains("ins_list") && st["ins_list"].is_string() && st.contains("duration") &&
                st["duration"].is_number_integer()) {
                std::string ins = st["ins_list"].get<std::string>();
                if (!ins.empty() && ins != "*") {
                    kline_key_to_chart_[kline_key(ins, st["duration"].get<int64_t>())] = cid;
                }
            }
        }
    }

    nlohmann::json merged = normalize_rtn_data(data_array);
    if (merged.empty()) {
        co_return; // 本帧无行情数据面内容
    }

    // 投递前过滤：丢弃已无连接需要的 quotes / klines（天勤不支持软退订，
    // 靠这里"入队前丢弃"避免无人消费的 diff 堆积）。
    if (!filter_merged(merged)) {
        co_return;
    }

    MarketDataEvent ev{MarketDataEvent::Kind::Diff, std::move(merged), {}};
    co_await ch_.async_send(std::move(ev));
    co_return;
}

bool TqMdGateway::filter_merged(nlohmann::json& merged) const {
    bool any = false;

    if (merged.contains("quotes") && merged["quotes"].is_object()) {
        auto& q = merged["quotes"];
        for (auto it = q.begin(); it != q.end();) {
            if (quote_ref_.count(it.key()) != 0) {
                any = true;
                ++it;
            } else {
                it = q.erase(it);
            }
        }
        if (q.empty()) {
            merged.erase("quotes");
        }
    }

    if (merged.contains("klines") && merged["klines"].is_object()) {
        auto& k = merged["klines"];
        for (auto it = k.begin(); it != k.end();) {
            const std::string symbol = it.key();
            if (!it.value().is_object()) {
                it = k.erase(it);
                continue;
            }
            // 对每个 duration 依据 (symbol,duration)->chart_id 反查 chart_ref_。
            for (auto dit = it.value().begin(); dit != it.value().end();) {
                int64_t dur = 0;
                try {
                    dur = std::stoll(dit.key());
                } catch (...) {
                    dit = it.value().erase(dit);
                    continue;
                }
                auto kit = kline_key_to_chart_.find(kline_key(symbol, dur));
                bool needed =
                    (kit != kline_key_to_chart_.end() && chart_ref_.count(kit->second) != 0);
                if (needed) {
                    any = true;
                    ++dit;
                } else {
                    dit = it.value().erase(dit);
                }
            }
            if (it.value().empty()) {
                it = k.erase(it);
            } else {
                ++it;
            }
        }
        if (k.empty()) {
            merged.erase("klines");
        }
    }

    return any;
}

} // namespace tqsdk

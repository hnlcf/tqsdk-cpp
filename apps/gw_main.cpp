#include "tqsdk/pch.h"

#include "tqsdk/auth/dynamic_endpoint.h"
#include "tqsdk/auth/tq_auth.h"
#include "tqsdk/config/tqapi_factory.h"
#include "tqsdk/gateway/sim/sim_td_gateway.h"
#include "tqsdk/gateway/tq/tq_md_gateway.h"
#include "tqsdk/gateway/tq/tq_td_gateway.h"

#ifdef TQSDK_ENABLE_CTP
    #include "tqsdk/gateway/ctp/ctp_md_gateway.h"
    #include "tqsdk/gateway/ctp/ctp_td_gateway.h"
#endif
#include "tqsdk/ipc/gateway_server.h"
#include "tqsdk/ipc/ipc_session.h"
#include "tqsdk/ipc/tcp_channel.h"
#include "tqsdk/ipc/uds_channel.h"
#include "tqsdk/util/log.h"
#include "tqsdk/util/spawn_logged.h"

#include <boost/asio/signal_set.hpp>
#include <unistd.h>
#include <CLI/CLI.hpp>

// ============================================================================
// tqsdk-gw —— 接入进程
// ----------------------------------------------------------------------------
// 职责：持有真实网关（行情/交易），在监听端点上等待 node 连接，把网关事件
// 桥接成 IPC 帧上推，并把 node 的 Subscribe/Command 下发给网关。
// 详见 include/tqsdk/ipc/gateway_server.h 的契约。
//
// 用法：
//   tqsdk-gw -c config/gw.toml
//
// 支持的网关（由配置 [md].kind / [td].kind 选择）：
//   md: tq（天勤 WebSocket 实盘行情） | none
//   td: sim（本地撮合模拟器）          | none
// （CTP 网关落地后可在此处扩展分支，GatewayIpcServer 无需改动。）
// ============================================================================

namespace {

using namespace tqsdk;
using tcp = boost::asio::ip::tcp;

// 依据配置构造真实网关；未知/不支持的种类返回 nullptr 并记警告。
std::unique_ptr<MarketDataGateway>
make_real_md(const TqApiConfig::Md& md, const CtpConfig& ctp, asio::any_io_executor ex) {
    switch (md.kind) {
    case MdKind::Tq:
        return std::make_unique<TqMdGateway>(ex, md.host, md.port, md.path);
    case MdKind::Ctp:
#ifdef TQSDK_ENABLE_CTP
        return std::make_unique<CtpMdGateway>(ex, ctp);
#else
        SPDLOG_ERROR("[gw] md.kind=ctp but binary built without TQSDK_ENABLE_CTP");
        return nullptr;
#endif
    case MdKind::None:
    case MdKind::Mock:
    case MdKind::Remote: // gw 进程本身不接受"远程"行情网关
        return nullptr;
    }
    return nullptr;
}

std::unique_ptr<TradingGateway> make_real_td(const TqApiConfig::Td& td,
                                             const CtpConfig&       ctp,
                                             const TqAuth::Config&  tq_auth,
                                             asio::any_io_executor  ex) {
    switch (td.kind) {
    case TdKind::Sim:
        return std::make_unique<SimTdGateway>(ex, td.initial_balance);
    case TdKind::Ctp:
#ifdef TQSDK_ENABLE_CTP
        return std::make_unique<CtpTdGateway>(ex, ctp);
#else
        SPDLOG_ERROR("[gw] td.kind=ctp but binary built without TQSDK_ENABLE_CTP");
        return nullptr;
#endif
    case TdKind::Tq: {
        if (td.tq.broker_name.empty()) {
            SPDLOG_ERROR("[gw] td.kind=tq requires [td.tq].broker_name");
            return nullptr;
        }
        std::string url = td.tq.td_url;
        std::string authorization;
        if (url.empty()) {
            if (!tq_auth.enabled) {
                SPDLOG_ERROR("[gw] td.kind=tq needs [td.tq].td_url or [tq_auth].enabled");
                return nullptr;
            }
            TqAuth     auth(tq_auth);
            const auto login = auth.login_result();
            if (!login) {
                SPDLOG_ERROR("[gw] tq td auth failed: {} ({})",
                             login.error().detail,
                             auth::auth_error_name(login.error().code));
                return nullptr;
            }
            const auto endpoint = auth.get_td_url(td.tq.broker_name, td.tq.account_id);
            if (!endpoint) {
                SPDLOG_ERROR("[gw] tq td url unavailable: {}", auth.last_error());
                return nullptr;
            }
            url           = endpoint->url;
            authorization = auth.authorization_header();
        }
        TqTdGateway::Options options;
        options.url           = std::move(url);
        options.authorization = std::move(authorization);
        options.broker_name   = td.tq.broker_name;
        options.account_id    = td.tq.account_id;
        options.password      = td.tq.password;
        options.broker_id     = td.tq.broker_id;
        options.front_url     = td.tq.front_url;
        options.account_key   = td.tq.account_key;
        return std::make_unique<TqTdGateway>(ex, std::move(options));
    }
    case TdKind::None:
    case TdKind::Mock:
    case TdKind::Remote:
        return nullptr;
    }
    return nullptr;
}

} // namespace

int main(int argc, char** argv) {
    CLI::App    app{"tqsdk-gw: gateway access process (tq md / sim td -> IPC)"};
    std::string config_path;
    std::string log_level;
    int         threads = 0;
    app.add_option("-c,--config", config_path, "Path to gw config TOML")->required();
    app.add_option("-l,--log-level", log_level, "Log level (overrides TQSDK_LOG_LEVEL)");
    app.add_option("-t,--threads", threads, "IO worker threads (overrides [runtime].threads)");
    app.set_version_flag("-v,--version",
                         fmt::format("{} {}", tqsdk::kPackageName, tqsdk::kVersion));
    CLI11_PARSE(app, argc, argv);

    tqsdk::setup_logging(tqsdk::LogOptions{log_level});

    auto cfg_result = load_gw_config(config_path);
    if (!cfg_result) {
        SPDLOG_CRITICAL("failed to load config '{}': {} ({})",
                        config_path,
                        cfg_result.error().detail,
                        config::config_error_name(cfg_result.error().code));
        return 1;
    }
    GwConfig cfg = std::move(*cfg_result);

    if (threads > 0) {
        cfg.runtime.threads = normalize_worker_threads(threads);
    }

    asio::io_context io;
    // 所有网关/会话共享同一条 core strand：多 worker 下状态更新严格串行。
    auto ex = asio::make_strand(io);

    // 可选：通过 TqAuth 名称服务解析动态 MD URL。解析失败时保留静态配置。
    TqApiConfig::Md md_cfg = cfg.md;
    if (md_cfg.kind == MdKind::Tq && md_cfg.auto_url && cfg.tq_auth.enabled) {
        TqAuth     auth(cfg.tq_auth);
        const auto auth_result = auth.login_result();
        if (!auth_result) {
            SPDLOG_WARN("[gw] dynamic md url: TqAuth login failed: {} ({})",
                        auth_result.error().detail,
                        auth::auth_error_name(auth_result.error().code));
        } else {
            DynamicEndpointResolver resolver(auth);
            if (auto endpoint = resolver.md()) {
                md_cfg.host = endpoint->host;
                md_cfg.port = std::to_string(endpoint->port);
                md_cfg.path = endpoint->path;
                SPDLOG_INFO("[gw] dynamic md url resolved: {}", endpoint->to_string());
            } else {
                SPDLOG_WARN("[gw] dynamic md url unavailable: {}", auth.last_error());
            }
        }
    }

    // 真实网关（进程生命周期内常驻，被所有连接共享）。
    auto md = make_real_md(md_cfg, cfg.ctp, ex);
    auto td = make_real_td(cfg.td, cfg.ctp, cfg.tq_auth, ex);

    // 若交易网关是 Sim：按配置灌入合约规格与初始撮合价，使其可直接撮合。
    if (auto* sim = dynamic_cast<SimTdGateway*>(td.get()); sim != nullptr) {
        for (const auto& s : cfg.specs) {
            ContractSpec spec;
            spec.volume_multiple    = s.volume_multiple;
            spec.price_tick         = s.price_tick;
            spec.margin_rate        = s.margin_rate;
            spec.commission_rate    = s.commission_rate;
            spec.commission_per_lot = s.commission_per_lot;
            sim->set_contract_spec(s.symbol, spec);
            SPDLOG_INFO("[gw] sim spec seeded: {}", s.symbol);
        }
        for (const auto& [sym, px] : cfg.last_prices) {
            sim->on_tick(sym, px);
            SPDLOG_INFO("[gw] sim last_price seeded: {} = {}", sym, px);
        }
    }

    if (!md) {
        SPDLOG_INFO("[gw] market-data gateway disabled (kind={})",
                    magic_enum::enum_name(cfg.md.kind));
    }
    if (!td) {
        SPDLOG_INFO("[gw] trading gateway disabled (kind={})", magic_enum::enum_name(cfg.td.kind));
    }

    // 启动网关（幂等协程，后台运行）。
    if (md) {
        co_spawn_logged(
            ex, [&md]() -> asio::awaitable<void> { co_await md->start(); }, "gw.md.start");
    }
    if (td) {
        co_spawn_logged(
            ex, [&td]() -> asio::awaitable<void> { co_await td->start(); }, "gw.td.start");
    }

    // 单消费者网关输出 -> 多连接 sink 的扇出层。多个 node 同时连接时，
    // 每个连接收到同一份事件，而不是互相抢 Channel。
    std::shared_ptr<ipc::EventBus<MarketDataEvent>> md_bus;
    std::shared_ptr<ipc::EventBus<TradeEvent>>      td_bus;
    if (md) {
        md_bus = std::make_shared<ipc::EventBus<MarketDataEvent>>(md->output(), ex);
        md_bus->start();
    }
    if (td) {
        td_bus = std::make_shared<ipc::EventBus<TradeEvent>>(td->event_output(), ex);
        td_bus->start();
    }

    // 活动连接注册表：shutdown 时统一 close。GatewayIpcServer 由各自的
    // 后台循环以 shared_from_this 自持，这里只保留一份引用用于优雅关闭。
    auto live_servers = std::make_shared<std::vector<std::shared_ptr<ipc::GatewayIpcServer>>>();
    auto live_mutex   = std::make_shared<std::mutex>();

    // 处理一条已建立的 channel：握手 -> 建 Server -> 登记 -> 自持运行。
    // 每条连接一个 strand；共享网关状态仍统一 dispatch 到 ex（core strand）。
    auto on_accept = [&cfg, ex, io_ex = io.get_executor(), md_bus, td_bus](
                         std::unique_ptr<ipc::IIpcChannel> channel,
                         std::shared_ptr<std::vector<std::shared_ptr<ipc::GatewayIpcServer>>> live,
                         std::shared_ptr<std::mutex> live_mu,
                         MarketDataGateway*          md_p,
                         TradingGateway*             td_p) {
        ipc::IpcSession::Options opts;
        opts.local_role         = ipc::IpcRole::Gateway; // gw 侧声明 Gateway
        opts.local_name         = cfg.local_name;
        opts.handshake_timeout  = cfg.handshake_timeout;
        opts.heartbeat_interval = cfg.heartbeat_interval;
        opts.heartbeat_timeout  = cfg.heartbeat_timeout;
        opts.encoding           = cfg.encoding;

        // 每条连接独立 strand；共享网关状态通过 gateway_ex=ex 串行化。
        auto srv_ex  = asio::make_strand(io_ex);
        auto session = std::make_unique<ipc::IpcSession>(std::move(channel), srv_ex, opts);
        co_spawn_logged(
            srv_ex,
            [session = std::move(session),
             srv_ex,
             gateway_ex = ex,
             live,
             live_mu,
             md_p,
             td_p,
             md_bus,
             td_bus]() mutable -> asio::awaitable<void> {
                auto started = co_await session->start();
                if (!started) {
                    SPDLOG_WARN("[gw] handshake failed: {} ({})",
                                started.error().detail,
                                ipc::ipc_error_name(started.error().code));
                    co_return;
                }
                SPDLOG_INFO("[gw] node connected & handshake ok");

                auto srv =
                    std::make_shared<ipc::GatewayIpcServer>(std::move(session),
                                                            md_p,
                                                            td_p,
                                                            srv_ex,
                                                            gateway_ex,
                                                            ipc::GatewayIpcServer::Options{});
                srv->attach_buses(md_bus.get(), td_bus.get());
                srv->start();
                {
                    std::lock_guard<std::mutex> lock(*live_mu);
                    live->push_back(srv);
                }

                // watchdog：轮询该连接是否已关闭；关闭后从注册表摘除并 close，
                // 释放我们对 server 的引用（server 仍由后台循环自持直到退出）。
                while (srv->is_open()) {
                    asio::steady_timer        t(co_await asio::this_coro::executor,
                                                std::chrono::milliseconds(200));
                    boost::system::error_code ec;
                    co_await t.async_wait(asio::redirect_error(asio::use_awaitable, ec));
                }
                srv->close(); // 在 srv_ex 上关闭本连接资源
                {
                    std::lock_guard<std::mutex> lock(*live_mu);
                    std::erase(*live, srv);
                }
                SPDLOG_INFO("[gw] node disconnected");
                co_return;
            },
            "gw.conn");
    };

    // ---- 监听端点（UDS 或 TCP）----
    std::shared_ptr<asio::local::stream_protocol::acceptor> uds_acc;
    std::shared_ptr<tcp::acceptor>                          tcp_acc;

    if (cfg.listen.transport == IpcTransport::Uds) {
        const std::string path = cfg.listen.socket;
        ::unlink(path.c_str());
        uds_acc = std::make_shared<asio::local::stream_protocol::acceptor>(
            io, asio::local::stream_protocol::endpoint(path));
        SPDLOG_INFO("[gw] listening on UDS {}", path);

        co_spawn_logged(
            ex,
            [uds_acc, ex, on_accept, live_servers, live_mutex, md = md.get(), td = td.get()]()
                -> asio::awaitable<void> {
                for (;;) {
                    boost::system::error_code ec;
                    auto                      sock = co_await uds_acc->async_accept(
                        asio::redirect_error(asio::use_awaitable, ec));
                    if (ec) {
                        if (ec == asio::error::operation_aborted) {
                            co_return;
                        }
                        SPDLOG_WARN("[gw] accept error: {}", ec.message());
                        continue;
                    }
                    SPDLOG_INFO("[gw] accepted UDS connection");
                    auto ch = std::make_unique<ipc::UdsChannel>(std::move(sock));
                    on_accept(std::move(ch), live_servers, live_mutex, md, td);
                }
            },
            "gw.accept.uds");
    } else {
        tcp::endpoint ep{asio::ip::make_address(cfg.listen.tcp_host), cfg.listen.tcp_port};
        tcp_acc = std::make_shared<tcp::acceptor>(io, ep);
        SPDLOG_INFO(
            "[gw] listening on TCP {}:{}", cfg.listen.tcp_host, tcp_acc->local_endpoint().port());

        co_spawn_logged(
            ex,
            [tcp_acc, ex, on_accept, live_servers, live_mutex, md = md.get(), td = td.get()]()
                -> asio::awaitable<void> {
                for (;;) {
                    boost::system::error_code ec;
                    auto                      sock = co_await tcp_acc->async_accept(
                        asio::redirect_error(asio::use_awaitable, ec));
                    if (ec) {
                        if (ec == asio::error::operation_aborted) {
                            co_return;
                        }
                        SPDLOG_WARN("[gw] accept error: {}", ec.message());
                        continue;
                    }
                    SPDLOG_INFO("[gw] accepted TCP connection");
                    auto ch = std::make_unique<ipc::TcpChannel>(std::move(sock));
                    on_accept(std::move(ch), live_servers, live_mutex, md, td);
                }
            },
            "gw.accept.tcp");
    }

    // ---- 优雅退出：SIGINT/SIGTERM ----
    // 不调用 io.stop()：关闭 acceptor 与所有活动 server 后，让 io_context 自然
    // 排空（accept 协程/后台循环都以 operation_aborted 或 nullopt 退出），
    // run() 返回。这样避免在协程帧仍引用已析构对象时强行销毁（ASan UAF）。
    asio::signal_set signals(io, SIGINT, SIGTERM);
    signals.async_wait([&](boost::system::error_code ec, int sig) {
        if (ec) {
            return;
        }
        SPDLOG_INFO("[gw] received signal {}, shutting down", sig);
        boost::system::error_code ignored;
        if (uds_acc) {
            uds_acc->close(ignored);
        }
        if (tcp_acc) {
            tcp_acc->close(ignored);
        }
        std::vector<std::shared_ptr<ipc::GatewayIpcServer>> snapshot;
        {
            std::lock_guard<std::mutex> lock(*live_mutex);
            snapshot = *live_servers;
        }
        // close 需要访问本连接的 timers/sink，post 到各自的 strand 执行。
        for (auto& srv : snapshot) {
            asio::post(srv->executor(), [srv]() { srv->close(); });
        }
        // 关闭网关事件源：唤醒仍阻塞在 output().async_receive() 的 pump 协程，
        // 使其以 nullopt 退出并释放各 server 的自持引用。
        if (md) {
            md->output().close();
        }
        if (td) {
            td->event_output().close();
        }
        // 关键：还要 stop() 真实网关。仅关闭 output 通道只唤醒 pump，但网关内部
        // 仍挂着未完成的 async 操作（如 TqMd 的 ws_->async_read、重连定时器等），
        // 会一直持有 io_context 的 work → io.run() 永不返回（Ctrl-C 卡死）。
        // stop() 关闭 websocket / 取消定时器，令这些挂起操作以 operation_aborted
        // 结束，io_context 才能真正排空。
        if (md) {
            auto* mdp = md.get();
            co_spawn_logged(
                ex, [mdp]() -> asio::awaitable<void> { co_await mdp->stop(); }, "gw.md.stop");
        }
        if (td) {
            auto* tdp = td.get();
            co_spawn_logged(
                ex, [tdp]() -> asio::awaitable<void> { co_await tdp->stop(); }, "gw.td.stop");
        }
    });

    SPDLOG_INFO("[gw] running with {} io worker thread(s)", cfg.runtime.threads);
    run_io_context(io, cfg.runtime.threads);
    SPDLOG_INFO("[gw] exited");
    return 0;
}

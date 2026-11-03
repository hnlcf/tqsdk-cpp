#include "tqsdk/pch.h"

#include "tqsdk/config/tqapi_factory.h"
#include "tqsdk/gateway/sim/sim_td_gateway.h"
#include "tqsdk/gateway/tq/tq_md_gateway.h"
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
std::unique_ptr<MarketDataGateway> make_real_md(const TqApiConfig::Md& md,
                                                asio::any_io_executor  ex) {
    switch (md.kind) {
    case MdKind::Tq:
        return std::make_unique<TqMdGateway>(ex, md.host, md.port, md.path);
    case MdKind::None:
    case MdKind::Mock:
    case MdKind::Remote: // gw 进程本身不接受"远程"行情网关
        return nullptr;
    }
    return nullptr;
}

std::unique_ptr<TradingGateway> make_real_td(const TqApiConfig::Td& td, asio::any_io_executor ex) {
    switch (td.kind) {
    case TdKind::Sim:
        return std::make_unique<SimTdGateway>(ex, td.initial_balance);
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
    CLI11_PARSE(app, argc, argv);

    tqsdk::setup_logging(tqsdk::LogOptions{log_level});

    GwConfig cfg;
    try {
        cfg = load_gw_config(config_path);
    } catch (const std::exception& e) {
        SPDLOG_CRITICAL("failed to load config '{}': {}", config_path, e.what());
        return 1;
    }

    if (threads > 0) {
        cfg.runtime.threads = normalize_worker_threads(threads);
    }

    asio::io_context io;
    // 所有网关/会话共享同一条 core strand：多 worker 下状态更新严格串行。
    auto ex = asio::make_strand(io);

    // 真实网关（进程生命周期内常驻，被所有连接共享）。
    auto md = make_real_md(cfg.md, ex);
    auto td = make_real_td(cfg.td, ex);

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

    // 处理一条已建立的 channel：握手 -> 建 Server -> 登记 -> 自持运行。
    // 握手失败/连接断开时，watchdog 协程把 server 从注册表移除并 close。
    auto on_accept = [&cfg, ex, md_bus, td_bus](
                         std::unique_ptr<ipc::IIpcChannel> channel,
                         std::shared_ptr<std::vector<std::shared_ptr<ipc::GatewayIpcServer>>> live,
                         MarketDataGateway*                                                   md_p,
                         TradingGateway* td_p) {
        ipc::IpcSession::Options opts;
        opts.local_role         = ipc::IpcRole::Gateway; // gw 侧声明 Gateway
        opts.local_name         = cfg.local_name;
        opts.handshake_timeout  = cfg.handshake_timeout;
        opts.heartbeat_interval = cfg.heartbeat_interval;
        opts.heartbeat_timeout  = cfg.heartbeat_timeout;
        opts.encoding           = cfg.encoding;

        auto session = std::make_unique<ipc::IpcSession>(std::move(channel), ex, opts);
        co_spawn_logged(
            ex,
            [session = std::move(session), ex, live, md_p, td_p, md_bus, td_bus]() mutable
                -> asio::awaitable<void> {
                try {
                    co_await session->start(); // 握手；失败抛异常
                } catch (const std::exception& e) {
                    SPDLOG_WARN("[gw] handshake failed: {}", e.what());
                    co_return;
                }
                SPDLOG_INFO("[gw] node connected & handshake ok");

                auto srv =
                    std::make_shared<ipc::GatewayIpcServer>(std::move(session), md_p, td_p, ex);
                srv->attach_buses(md_bus.get(), td_bus.get());
                srv->start();
                live->push_back(srv);

                // watchdog：轮询该连接是否已关闭；关闭后从注册表摘除并 close，
                // 释放我们对 server 的引用（server 仍由后台循环自持直到退出）。
                while (srv->is_open()) {
                    asio::steady_timer        t(co_await asio::this_coro::executor,
                                                std::chrono::milliseconds(200));
                    boost::system::error_code ec;
                    co_await t.async_wait(asio::redirect_error(asio::use_awaitable, ec));
                }
                srv->close();
                std::erase(*live, srv);
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
            [uds_acc, ex, on_accept, live_servers, md = md.get(), td = td.get()]()
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
                    on_accept(std::move(ch), live_servers, md, td);
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
            [tcp_acc, ex, on_accept, live_servers, md = md.get(), td = td.get()]()
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
                    on_accept(std::move(ch), live_servers, md, td);
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
        for (auto& srv : *live_servers) {
            srv->close();
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

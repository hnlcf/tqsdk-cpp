#include "tqsdk/pch.h"

#include "tqsdk/config/tqapi_factory.h"
#include "tqsdk/data/tick_store.h"
#include "tqsdk/instrument/instrument_catalog.h"
#include "tqsdk/multiaccount/multi_account_manager.h"
#include "tqsdk/serve/http_server.h"
#include "tqsdk/tqapi.h"
#include "tqsdk/trading/holiday_calendar.h"
#include "tqsdk/util/log.h"
#include "tqsdk/util/spawn_logged.h"

#include <boost/asio/signal_set.hpp>
#include <CLI/CLI.hpp>

// ============================================================================
// tqsdk-node —— 数据服务进程
// ----------------------------------------------------------------------------
// 职责：
//   1. 以 IpcRole::Node 连接 gw 端点，拉取行情/交易并合并为进程内权威截面
//      （TqApi + RemoteMdGateway/RemoteTdGateway，md_loop/td_loop 完成合并）。
//   2. 通过 serve 层（HttpServer）对外暴露 REST：行情/账户/订单/持仓 + 下单/撤单。
//
// 用法：
//   tqsdk-node -c config/node.toml
//
// 数据流：
//   gw --IPC(MarketDataPush/TradePush)--> RemoteMd/TdGateway::output()
//        -> TqApi.md_loop/td_loop -> DiffMergeEngine -> DataManager
//        -> HttpServer 读 DataManager -> HTTP JSON 响应
//   HTTP POST/DELETE /order -> TqApi.insert_order/cancel_order -> RemoteTdGateway
//        -> IPC Command -> gw -> 真实网关
// ============================================================================

namespace {

using namespace tqsdk;

// 持有运行期全部对象，保证生命周期覆盖 io.run()。
struct NodeRuntime {
    std::shared_ptr<TqAuth>              auth;
    std::shared_ptr<InstrumentCatalog>   catalog;
    std::shared_ptr<KlineStore>          kline_store;
    std::shared_ptr<TickStore>           tick_store;
    std::shared_ptr<audit::Log>          audit;
    std::shared_ptr<ContinuousTable>     continuous;
    std::shared_ptr<TqApi>               api;
    std::shared_ptr<MultiAccountManager> accounts;
    std::unique_ptr<serve::HttpServer>   http;
};

} // namespace

int main(int argc, char** argv) {
    tqsdk::HolidayCalendar::start_global();
    CLI::App    app{"tqsdk-node: data service process (gw proxy + HTTP serve)"};
    std::string config_path;
    std::string log_level;
    int         threads = 0;
    app.add_option("-c,--config", config_path, "Path to node config TOML")->required();
    app.add_option("-l,--log-level", log_level, "Log level (overrides TQSDK_LOG_LEVEL)");
    app.add_option("-t,--threads", threads, "IO worker threads (overrides [runtime].threads)");
    app.set_version_flag("-v,--version",
                         std::string(tqsdk::kPackageName) + " " + std::string(tqsdk::kVersion));
    CLI11_PARSE(app, argc, argv);

    tqsdk::setup_logging(tqsdk::LogOptions{log_level});

    NodeConfig cfg;
    try {
        cfg = load_node_config(config_path);
    } catch (const std::exception& e) {
        SPDLOG_CRITICAL("failed to load config '{}': {}", config_path, e.what());
        return 1;
    }

    if (threads > 0) {
        cfg.runtime.threads = normalize_worker_threads(threads);
    }

    if (cfg.auth.enabled) {
        if (cfg.auth.token.empty() && !cfg.auth.token_env.empty()) {
            if (const char* env = std::getenv(cfg.auth.token_env.c_str()); env != nullptr) {
                cfg.auth.token = env;
            }
        }
        if (cfg.auth.token.empty()) {
            SPDLOG_CRITICAL("[node] serve auth enabled but no token/token_env is configured");
            return 1;
        }
    }

    asio::io_context io;
    auto             io_ex   = io.get_executor();
    auto             core_ex = asio::make_strand(io);
    auto             rt      = std::make_shared<NodeRuntime>();

    if (cfg.tq_auth.enabled) {
        rt->auth = std::make_shared<TqAuth>(cfg.tq_auth);
        if (!rt->auth->login()) {
            SPDLOG_WARN("[node] TqAuth login failed: {}; catalog will request without token",
                        rt->auth->last_error());
        } else {
            SPDLOG_INFO(
                "[node] TqAuth logged in as {} (catalog/latest downloads will use Bearer token)",
                cfg.tq_auth.user_name);
        }
    }
    if (cfg.instruments.enabled) {
        rt->catalog = std::make_shared<InstrumentCatalog>(cfg.instruments, rt->auth);
        if (!rt->catalog->load_local()) {
            SPDLOG_WARN(
                "[node] instrument catalog not available yet; background refresh will retry");
        }
        rt->catalog->start_background_refresh();
    }
    if (cfg.kline_cache.enabled) {
        rt->kline_store = std::make_shared<KlineStore>(cfg.kline_cache);
        if (!rt->kline_store->ready()) {
            SPDLOG_WARN("[node] kline cache unavailable; continuing without history cache");
        }
    }
    if (cfg.continuous.enabled) {
        rt->continuous = std::make_shared<ContinuousTable>(cfg.continuous);
        if (!rt->continuous->load_local()) {
            SPDLOG_WARN("[node] continuous table not available yet; background refresh will retry");
        }
        rt->continuous->start_background_refresh();
    }
    if (cfg.audit.enabled) {
        try {
            rt->audit = std::make_shared<audit::Log>(cfg.audit);
        } catch (const std::exception& e) {
            SPDLOG_CRITICAL("[node] failed to open audit log '{}': {}", cfg.audit.file, e.what());
            return 1;
        }
    }
    if (cfg.tick_cache.enabled) {
        rt->tick_store = std::make_shared<TickStore>(cfg.tick_cache);
        if (!rt->tick_store->ready()) {
            SPDLOG_WARN("[node] tick cache unavailable; continuing without tick persistence");
        }
    }

    // 引导协程：建连 gw -> 建 TqApi -> 预订阅 -> 起 HTTP serve。
    // 失败（gw 未就绪/握手失败）记错误并停止 io。
    // TqApi/网关/DataManager 全部绑定 core_ex；HTTP 连接 I/O 在 io_ex 的
    // 每连接 strand 上并行，进入路由 handler 前切回 core_ex。
    auto bootstrap = [&io, io_ex, core_ex, rt, &cfg]() -> asio::awaitable<void> {
        std::shared_ptr<TqApi> primary;
        try {
            primary = std::shared_ptr<TqApi>(
                co_await make_tqapi(io, cfg.upstream, core_ex, rt->kline_store));
        } catch (const std::exception& e) {
            SPDLOG_CRITICAL("[node] failed to connect/init upstream gw: {}", e.what());
            io.stop();
            co_return;
        }
        rt->api                      = primary;
        rt->accounts                 = std::make_shared<MultiAccountManager>();
        const std::string primary_id = cfg.account_id.empty() ? "default" : cfg.account_id;
        if (!rt->accounts->add_account(primary_id, primary)) {
            SPDLOG_CRITICAL("[node] invalid/duplicate primary account id: {}", primary_id);
            io.stop();
            co_return;
        }
        SPDLOG_INFO("[node] upstream gateway ready (account={})", primary_id);
        if (rt->tick_store) {
            auto store = rt->tick_store;
            primary->set_tick_sink([store](const Tick& tick) { store->append(tick); });
        }

        // 预订阅：让服务启动后即持续有数据，便于 curl 直接读到。
        for (const auto& sym : cfg.symbols) {
            primary->get_quote(sym); // 触发订阅（不存在则创建占位并订阅）
            // tick sink 只对已创建的 TickSeries 生效；开启持久化时预创建窗口。
            if (rt->tick_store)
                primary->get_tick_serial(sym, 200);
            SPDLOG_INFO("[node] pre-subscribed quote {}", sym);
        }

        // 额外账户：每个账户连接自己的 gw 端点，使用独立 TqApi/DataManager。
        for (const auto& account : cfg.accounts) {
            if (rt->accounts->find_account(account.id)) {
                SPDLOG_WARN("[node] duplicate account id ignored: {}", account.id);
                continue;
            }
            auto account_cfg           = cfg.upstream;
            account_cfg.ipc.local_name = "node-" + account.id;
            account_cfg.md.kind        = MdKind::Remote;
            account_cfg.td.kind        = TdKind::Remote;
            account_cfg.md.tcp_host    = account.gw_host;
            account_cfg.md.tcp_port    = account.gw_port;
            account_cfg.td.tcp_host    = account.gw_host;
            account_cfg.td.tcp_port    = account.gw_port;
            try {
                auto extra =
                    std::shared_ptr<TqApi>(co_await make_tqapi(io, account_cfg, core_ex, nullptr));
                rt->accounts->add_account(account.id, extra);
                for (const auto& sym : cfg.symbols)
                    extra->get_quote(sym);
                SPDLOG_INFO("[node] account {} connected to gw {}:{}",
                            account.id,
                            account.gw_host,
                            account.gw_port);
            } catch (const std::exception& e) {
                SPDLOG_ERROR("[node] account {} failed to connect: {}", account.id, e.what());
            }
        }

        serve::WsOptions ws_opts;
        ws_opts.path               = cfg.ws.path;
        ws_opts.heartbeat_interval = cfg.ws.heartbeat_interval;
        ws_opts.max_clients        = cfg.ws.max_clients;
        ws_opts.max_queue          = cfg.ws.max_queue;
        ws_opts.coalesce           = cfg.ws.coalesce;

        serve::AuthOptions auth_opts;
        auth_opts.enabled           = cfg.auth.enabled;
        auth_opts.token             = cfg.auth.token;
        auth_opts.allow_query_token = cfg.auth.allow_query_token;
        auth_opts.exempt_healthz    = cfg.auth.exempt_healthz;
        rt->http                    = std::make_unique<serve::HttpServer>(rt->api.get(),
                                                                          io_ex,
                                                                          std::move(ws_opts),
                                                                          core_ex,
                                                                          rt->catalog.get(),
                                                                          rt->accounts.get(),
                                                                          rt->tick_store.get(),
                                                                          std::move(auth_opts),
                                                                          rt->audit,
                                                                          rt->continuous.get());
        try {
            rt->http->listen(cfg.serve_bind, cfg.serve_port);
        } catch (const std::exception& e) {
            SPDLOG_CRITICAL("[node] HTTP serve failed: {}", e.what());
            io.stop();
            co_return;
        }
        SPDLOG_INFO("[node] HTTP serve on http://{}:{}/ (ws {}, auth {})",
                    cfg.serve_bind,
                    rt->http->port(),
                    cfg.ws.path,
                    cfg.auth.enabled ? "enabled" : "disabled");
        co_return;
    };

    co_spawn_logged(core_ex, bootstrap, "node.bootstrap");

    // 优雅退出。
    asio::signal_set signals(io, SIGINT, SIGTERM);
    signals.async_wait([&](boost::system::error_code ec, int sig) {
        if (!ec) {
            SPDLOG_INFO("[node] received signal {}, shutting down", sig);
            io.stop();
        }
    });

    SPDLOG_INFO("[node] running with {} io worker thread(s)", cfg.runtime.threads);
    run_io_context(io, cfg.runtime.threads);

    // 收尾：关闭 serve 与 TqApi，驱动 io 跑完后台协程后再析构。
    if (rt->http) {
        rt->http->stop();
    }
    if (rt->accounts) {
        rt->accounts->close();
    } else if (rt->api) {
        rt->api->close();
    }
    if (rt->catalog) {
        rt->catalog->stop();
    }
    io.restart();
    io.run_for(std::chrono::milliseconds(200)); // 排空 close 触发的回调
    if (rt->tick_store) {
        rt->tick_store->flush();
        rt->tick_store->stop();
    }
    if (rt->kline_store) {
        rt->kline_store->flush();
        rt->kline_store->stop();
    }
    SPDLOG_INFO("[node] exited");
    return 0;
}

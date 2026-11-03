#include "tqsdk/pch.h"

#include "tqsdk/config/tqapi_factory.h"
#include "tqsdk/instrument/instrument_catalog.h"
#include "tqsdk/serve/http_server.h"
#include "tqsdk/tqapi.h"
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
    std::shared_ptr<InstrumentCatalog>   catalog;
    std::unique_ptr<TqApi>               api;
    std::unique_ptr<serve::HttpServer>   http;
};

} // namespace

int main(int argc, char** argv) {
    CLI::App    app{"tqsdk-node: data service process (gw proxy + HTTP serve)"};
    std::string config_path;
    std::string log_level;
    int         threads = 0;
    app.add_option("-c,--config", config_path, "Path to node config TOML")->required();
    app.add_option("-l,--log-level", log_level, "Log level (overrides TQSDK_LOG_LEVEL)");
    app.add_option("-t,--threads", threads, "IO worker threads (overrides [runtime].threads)");
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

    asio::io_context io;
    auto             io_ex   = io.get_executor();
    auto             core_ex = asio::make_strand(io);
    auto             rt      = std::make_shared<NodeRuntime>();

    if (cfg.instruments.enabled) {
        rt->catalog = std::make_shared<InstrumentCatalog>(cfg.instruments);
        if (!rt->catalog->load_local()) {
            SPDLOG_WARN("[node] instrument catalog not available yet; background refresh will retry");
        }
        rt->catalog->start_background_refresh();
    }

    // 引导协程：建连 gw -> 建 TqApi -> 预订阅 -> 起 HTTP serve。
    // 失败（gw 未就绪/握手失败）记错误并停止 io。
    // TqApi/网关/DataManager 全部绑定 core_ex；HTTP 连接 I/O 在 io_ex 的
    // 每连接 strand 上并行，进入路由 handler 前切回 core_ex。
    auto bootstrap = [&io, io_ex, core_ex, rt, &cfg]() -> asio::awaitable<void> {
        try {
            rt->api = co_await make_tqapi(io, cfg.upstream, core_ex);
        } catch (const std::exception& e) {
            SPDLOG_CRITICAL("[node] failed to connect/init upstream gw: {}", e.what());
            io.stop();
            co_return;
        }
        SPDLOG_INFO("[node] upstream gateway ready");

        // 预订阅：让服务启动后即持续有数据，便于 curl 直接读到。
        for (const auto& sym : cfg.symbols) {
            rt->api->get_quote(sym); // 触发订阅（不存在则创建占位并订阅）
            SPDLOG_INFO("[node] pre-subscribed quote {}", sym);
        }

        serve::WsOptions ws_opts;
        ws_opts.path               = cfg.ws.path;
        ws_opts.heartbeat_interval = cfg.ws.heartbeat_interval;
        ws_opts.max_clients        = cfg.ws.max_clients;
        ws_opts.max_queue          = cfg.ws.max_queue;

        rt->http = std::make_unique<serve::HttpServer>(
            rt->api.get(), io_ex, std::move(ws_opts), core_ex, rt->catalog.get());
        try {
            rt->http->listen(cfg.serve_bind, cfg.serve_port);
        } catch (const std::exception& e) {
            SPDLOG_CRITICAL("[node] HTTP serve failed: {}", e.what());
            io.stop();
            co_return;
        }
        SPDLOG_INFO("[node] HTTP serve on http://{}:{}/ (ws {})",
                    cfg.serve_bind,
                    rt->http->port(),
                    cfg.ws.path);
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
    if (rt->api) {
        rt->api->close();
    }
    if (rt->catalog) {
        rt->catalog->stop();
    }
    io.restart();
    io.run_for(std::chrono::milliseconds(200)); // 排空 close 触发的回调
    SPDLOG_INFO("[node] exited");
    return 0;
}

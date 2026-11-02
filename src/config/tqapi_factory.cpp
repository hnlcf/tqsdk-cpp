#include "tqsdk/config/tqapi_factory.h"

#include "tqsdk/gateway/sim/sim_td_gateway.h"
#include "tqsdk/gateway/tq/tq_md_gateway.h"
#include "tqsdk/ipc/ipc_client.h"
#include "tqsdk/ipc/ipc_session.h"
#include "tqsdk/ipc/remote_md_gateway.h"
#include "tqsdk/ipc/remote_td_gateway.h"
#include "tqsdk/ipc/tcp_channel.h"
#include "tqsdk/ipc/uds_channel.h"
#include "tqsdk/util/spawn_logged.h"

#include <toml++/toml.hpp>

namespace tqsdk {

namespace {

// ---------- 枚举解析 ----------

MdKind parse_md_kind(std::string_view s) {
    if (s == "none" || s.empty())
        return MdKind::None;
    if (s == "tq")
        return MdKind::Tq;
    if (s == "remote")
        return MdKind::Remote;
    if (s == "mock")
        return MdKind::Mock;
    throw std::runtime_error("config: unknown [md].kind = '" + std::string(s) + "'");
}

TdKind parse_td_kind(std::string_view s) {
    if (s == "none" || s.empty())
        return TdKind::None;
    if (s == "sim")
        return TdKind::Sim;
    if (s == "remote")
        return TdKind::Remote;
    if (s == "mock")
        return TdKind::Mock;
    throw std::runtime_error("config: unknown [td].kind = '" + std::string(s) + "'");
}

IpcTransport parse_transport(std::string_view s) {
    if (s == "uds")
        return IpcTransport::Uds;
    if (s == "tcp")
        return IpcTransport::Tcp;
    throw std::runtime_error("config: unknown [ipc].transport = '" + std::string(s) + "'");
}

ipc::FrameEncoding parse_encoding(std::string_view s) {
    if (s.empty() || s == "json")
        return ipc::FrameEncoding::Json;
    if (s == "msgpack")
        return ipc::FrameEncoding::MessagePack;
    throw std::runtime_error("config: unknown [ipc].encoding = '" + std::string(s) + "'");
}

// 读取指定 table 的某键为 string（缺省返回 def）。
std::string get_str(const toml::table& tbl, std::string_view key, std::string def = {}) {
    if (auto* v = tbl.get(key)) {
        if (auto s = v->value<std::string>()) {
            return *s;
        }
        throw std::runtime_error("config: key '" + std::string(key) + "' must be a string");
    }
    return def;
}

double get_double(const toml::table& tbl, std::string_view key, double def) {
    if (auto* v = tbl.get(key)) {
        if (auto d = v->value<double>())
            return *d;
        if (auto i = v->value<int64_t>())
            return static_cast<double>(*i);
        throw std::runtime_error("config: key '" + std::string(key) + "' must be a number");
    }
    return def;
}

int64_t get_int(const toml::table& tbl, std::string_view key, int64_t def) {
    if (auto* v = tbl.get(key)) {
        if (auto i = v->value<int64_t>())
            return *i;
        throw std::runtime_error("config: key '" + std::string(key) + "' must be an integer");
    }
    return def;
}

const toml::table& require_table(const toml::table& root, std::string_view name) {
    const toml::table*       t = root[name].as_table();
    static const toml::table empty{};
    return t ? *t : empty;
}

// 解析一份已加载的 TOML DOM 到 TqApiConfig。
TqApiConfig build_from_toml(const toml::table& root) {
    TqApiConfig cfg;

    if (const auto& ipc_t = require_table(root, "ipc"); !ipc_t.empty()) {
        cfg.ipc.transport = parse_transport(get_str(ipc_t, "transport", "uds"));
        cfg.ipc.encoding  = parse_encoding(get_str(ipc_t, "encoding", "json"));
        cfg.ipc.uds_dir   = get_str(ipc_t, "uds_dir", "/tmp");
        cfg.ipc.handshake_timeout =
            std::chrono::milliseconds(get_int(ipc_t, "handshake_timeout_ms", 5000));
        cfg.ipc.heartbeat_interval =
            std::chrono::milliseconds(get_int(ipc_t, "heartbeat_interval_ms", 5000));
        cfg.ipc.heartbeat_timeout =
            std::chrono::milliseconds(get_int(ipc_t, "heartbeat_timeout_ms", 15000));
    }

    if (const auto& md_t = require_table(root, "md"); !md_t.empty()) {
        cfg.md.kind     = parse_md_kind(get_str(md_t, "kind", "none"));
        cfg.md.host     = get_str(md_t, "host", cfg.md.host);
        cfg.md.port     = get_str(md_t, "port", cfg.md.port);
        cfg.md.path     = get_str(md_t, "path", cfg.md.path);
        cfg.md.socket   = get_str(md_t, "socket", "");
        cfg.md.tcp_host = get_str(md_t, "tcp_host", "");
        cfg.md.tcp_port = static_cast<uint16_t>(get_int(md_t, "tcp_port", 0));
        cfg.md.name     = get_str(md_t, "name", cfg.md.name);
    }

    if (const auto& td_t = require_table(root, "td"); !td_t.empty()) {
        cfg.td.kind            = parse_td_kind(get_str(td_t, "kind", "none"));
        cfg.td.initial_balance = get_double(td_t, "initial_balance", cfg.td.initial_balance);
        cfg.td.socket          = get_str(td_t, "socket", "");
        cfg.td.tcp_host        = get_str(td_t, "tcp_host", "");
        cfg.td.tcp_port        = static_cast<uint16_t>(get_int(td_t, "tcp_port", 0));
        cfg.td.name            = get_str(td_t, "name", cfg.td.name);
        cfg.td.ack_timeout     = std::chrono::milliseconds(get_int(td_t, "ack_timeout_ms", 10000));
    }

    return cfg;
}

// 用于远程连接的角色/端点参数。
struct RemoteEndpoint {
    std::string socket;
    std::string tcp_host;
    uint16_t    tcp_port = 0;
};

// 建立一条到 gw 的 IIpcChannel（UDS 或 TCP）。
std::unique_ptr<ipc::IIpcChannel>
connect_channel(asio::io_context& ctx, const TqApiConfig::Ipc& ipc_cfg, const RemoteEndpoint& ep) {
    if (ipc_cfg.transport == IpcTransport::Uds) {
        if (ep.socket.empty()) {
            throw std::runtime_error("config: uds transport requires socket path");
        }
        return ipc::UdsChannel::connect(ctx, ep.socket);
    }
    if (ep.tcp_host.empty() || ep.tcp_port == 0) {
        throw std::runtime_error("config: tcp transport requires tcp_host and tcp_port");
    }
    return ipc::TcpChannel::connect(ctx, ep.tcp_host, ep.tcp_port);
}

} // namespace

TqApiConfig parse_tqapi_config(std::string_view toml_text) {
    toml::table root;
    try {
        root = toml::parse(toml_text);
    } catch (const toml::parse_error& e) {
        throw std::runtime_error(std::string("config: toml parse error: ") +
                                 e.description().data());
    }
    return build_from_toml(root);
}

TqApiConfig load_tqapi_config(const std::string& path) {
    toml::table root;
    try {
        root = toml::parse_file(path);
    } catch (const toml::parse_error& e) {
        throw std::runtime_error(std::string("config: parse '") + path +
                                 "': " + e.description().data());
    }
    return build_from_toml(root);
}

asio::awaitable<std::unique_ptr<TqApi>> make_tqapi(asio::io_context& ctx, const TqApiConfig& cfg) {
    auto ex = ctx.get_executor();

    const bool need_md_remote = cfg.md.kind == MdKind::Remote;
    const bool need_td_remote = cfg.td.kind == TdKind::Remote;

    // ---- 远程模式：建立共享的 IpcSession + IpcClient（md/td 共用一条连接）----
    std::shared_ptr<ipc::IpcClient> client;
    if (need_md_remote || need_td_remote) {
        // md 与 td 必须指向同一个 gw 端点（一条连接同时承载行情与交易）。
        RemoteEndpoint ep;
        if (need_md_remote) {
            ep = {cfg.md.socket, cfg.md.tcp_host, cfg.md.tcp_port};
        }
        if (need_td_remote) {
            RemoteEndpoint td_ep{cfg.td.socket, cfg.td.tcp_host, cfg.td.tcp_port};
            if (need_md_remote && (td_ep.socket != ep.socket || td_ep.tcp_host != ep.tcp_host ||
                                   td_ep.tcp_port != ep.tcp_port)) {
                throw std::runtime_error(
                    "config: md and td remote endpoints differ; only one shared gw connection "
                    "is supported");
            }
            ep = td_ep;
        }

        auto channel = connect_channel(ctx, cfg.ipc, ep);

        ipc::IpcSession::Options opts;
        opts.local_role         = ipc::IpcRole::Gateway;
        opts.local_name         = need_md_remote ? cfg.md.name : cfg.td.name;
        opts.handshake_timeout  = cfg.ipc.handshake_timeout;
        opts.heartbeat_interval = cfg.ipc.heartbeat_interval;
        opts.heartbeat_timeout  = cfg.ipc.heartbeat_timeout;
        opts.encoding           = cfg.ipc.encoding;

        auto session = std::make_unique<ipc::IpcSession>(std::move(channel), ex, opts);
        co_await session->start(); // 握手；失败抛异常

        client = std::make_shared<ipc::IpcClient>(std::move(session), ex);
    }

    // ---- 造行情网关 ----
    std::unique_ptr<MarketDataGateway> md;
    switch (cfg.md.kind) {
    case MdKind::None:
    case MdKind::Mock:
        // Mock 需要外部注入：这里不造；调用方可自行构造后再包装。
        md = nullptr;
        break;
    case MdKind::Tq:
        md = std::make_unique<TqMdGateway>(ex, cfg.md.host, cfg.md.port, cfg.md.path);
        break;
    case MdKind::Remote:
        // 注册 md handler 进 client（在 client->start() 之前完成）。
        md = std::make_unique<ipc::RemoteMdGateway>(client, ex);
        break;
    }

    // ---- 造交易网关 ----
    std::unique_ptr<TradingGateway> td;
    switch (cfg.td.kind) {
    case TdKind::None:
    case TdKind::Mock:
        td = nullptr;
        break;
    case TdKind::Sim:
        td = std::make_unique<SimTdGateway>(ex, cfg.td.initial_balance);
        break;
    case TdKind::Remote:
        td = std::make_unique<ipc::RemoteTdGateway>(client, ex, cfg.td.ack_timeout);
        break;
    }

    // 远程 client 的读循环：必须在 md/td 都注册完 handler 之后启动。
    if (client) {
        client->start();
    }

    auto api = std::make_unique<TqApi>(ex, std::move(md), std::move(td));
    // 远程模式：把共享 IpcClient 的关闭纳入 TqApi::close()，确保 close() 会
    // 同步关闭 IPC 连接（进而唤醒读循环），调用方驱动 io 后即可安全析构。
    if (client) {
        auto c = client; // 拷贝 shared_ptr 到钩子，生命周期与 api 一致
        api->add_close_hook([c]() { c->stop(); });
    }
    api->start();
    co_return api;
}

} // namespace tqsdk

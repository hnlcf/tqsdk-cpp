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
    if (s == "none" || s.empty()) {
        return MdKind::None;
    }
    if (s == "tq") {
        return MdKind::Tq;
    }
    if (s == "ctp") {
        return MdKind::Ctp;
    }
    if (s == "remote") {
        return MdKind::Remote;
    }
    if (s == "mock") {
        return MdKind::Mock;
    }
    throw std::runtime_error("config: unknown [md].kind = '" + std::string(s) + "'");
}

TdKind parse_td_kind(std::string_view s) {
    if (s == "none" || s.empty()) {
        return TdKind::None;
    }
    if (s == "sim") {
        return TdKind::Sim;
    }
    if (s == "ctp") {
        return TdKind::Ctp;
    }
    if (s == "remote") {
        return TdKind::Remote;
    }
    if (s == "mock") {
        return TdKind::Mock;
    }
    throw std::runtime_error("config: unknown [td].kind = '" + std::string(s) + "'");
}

IpcTransport parse_transport(std::string_view s) {
    if (s == "uds") {
        return IpcTransport::Uds;
    }
    if (s == "tcp") {
        return IpcTransport::Tcp;
    }
    throw std::runtime_error("config: unknown [ipc].transport = '" + std::string(s) + "'");
}

ipc::FrameEncoding parse_encoding(std::string_view s) {
    if (s.empty() || s == "json") {
        return ipc::FrameEncoding::Json;
    }
    if (s == "msgpack") {
        return ipc::FrameEncoding::MessagePack;
    }
    throw std::runtime_error("config: unknown [ipc].encoding = '" + std::string(s) + "'");
}

ipc::IpcRole parse_role(std::string_view s) {
    if (s.empty() || s == "gateway") {
        return ipc::IpcRole::Gateway;
    }
    if (s == "node") {
        return ipc::IpcRole::Node;
    }
    throw std::runtime_error("config: unknown [ipc].role = '" + std::string(s) + "'");
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
        if (auto d = v->value<double>()) {
            return *d;
        }
        if (auto i = v->value<int64_t>()) {
            return static_cast<double>(*i);
        }
        throw std::runtime_error("config: key '" + std::string(key) + "' must be a number");
    }
    return def;
}

int64_t get_int(const toml::table& tbl, std::string_view key, int64_t def) {
    if (auto* v = tbl.get(key)) {
        if (auto i = v->value<int64_t>()) {
            return *i;
        }
        throw std::runtime_error("config: key '" + std::string(key) + "' must be an integer");
    }
    return def;
}

bool get_bool(const toml::table& tbl, std::string_view key, bool def) {
    if (auto* v = tbl.get(key)) {
        if (auto b = v->value<bool>()) {
            return *b;
        }
        throw std::runtime_error("config: key '" + std::string(key) + "' must be a boolean");
    }
    return def;
}

const toml::table& require_table(const toml::table& root, std::string_view name) {
    const toml::table*       t = root[name].as_table();
    static const toml::table empty{};
    return t ? *t : empty;
}

CtpConfig parse_ctp_config(const toml::table& root) {
    CtpConfig ctp;
    if (const auto& t = require_table(root, "ctp"); !t.empty()) {
        ctp.broker_id = get_str(t, "broker_id", ctp.broker_id);
        ctp.user_id   = get_str(t, "user_id", ctp.user_id);
        ctp.password  = get_str(t, "password", ctp.password);
        ctp.auth_code = get_str(t, "auth_code", ctp.auth_code);
        ctp.app_id    = get_str(t, "app_id", ctp.app_id);
        ctp.td_front  = get_str(t, "td_front", ctp.td_front);
        ctp.md_front  = get_str(t, "md_front", ctp.md_front);
        ctp.flow_path = get_str(t, "flow_path", ctp.flow_path);
        ctp.order_interval =
            std::chrono::milliseconds(get_int(t, "order_interval_ms", ctp.order_interval.count()));
        ctp.front_timeout = std::chrono::milliseconds(
            std::max<int64_t>(0, get_int(t, "front_timeout_ms", ctp.front_timeout.count())));
    }
    return ctp;
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
        cfg.ipc.role       = parse_role(get_str(ipc_t, "role", "gateway"));
        cfg.ipc.local_name = get_str(ipc_t, "local_name", cfg.ipc.local_name);
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
        cfg.md.auto_url = get_bool(md_t, "auto_url", cfg.md.auto_url);
    }

    if (const auto& td_t = require_table(root, "td"); !td_t.empty()) {
        cfg.td.kind            = parse_td_kind(get_str(td_t, "kind", "none"));
        cfg.td.initial_balance = get_double(td_t, "initial_balance", cfg.td.initial_balance);
        cfg.td.socket          = get_str(td_t, "socket", "");
        cfg.td.tcp_host        = get_str(td_t, "tcp_host", "");
        cfg.td.tcp_port        = static_cast<uint16_t>(get_int(td_t, "tcp_port", 0));
        cfg.td.name            = get_str(td_t, "name", cfg.td.name);
        cfg.td.ack_timeout     = std::chrono::milliseconds(get_int(td_t, "ack_timeout_ms", 10000));
        cfg.td.shfe_market_as_limit =
            get_bool(td_t, "shfe_market_as_limit", cfg.td.shfe_market_as_limit);
        cfg.td.auto_url = get_bool(td_t, "auto_url", cfg.td.auto_url);
        if (auto* specs_tv = td_t.get("specs"); specs_tv && specs_tv->is_table()) {
            const auto& specs_t = *specs_tv->as_table();
            for (auto&& [k, v] : specs_t) {
                const auto* sub = v.as_table();
                if (!sub) {
                    continue;
                }
                TqApiConfig::Td::Spec spec;
                spec.symbol          = std::string(k.str());
                spec.volume_multiple = get_double(*sub, "volume_multiple", spec.volume_multiple);
                spec.price_tick      = get_double(*sub, "price_tick", spec.price_tick);
                spec.margin_rate     = get_double(*sub, "margin_rate", spec.margin_rate);
                spec.commission_rate = get_double(*sub, "commission_rate", spec.commission_rate);
                spec.commission_per_lot =
                    get_double(*sub, "commission_per_lot", spec.commission_per_lot);
                cfg.td.specs.push_back(std::move(spec));
            }
        }
        if (auto* lp_tv = td_t.get("last_prices"); lp_tv && lp_tv->is_table()) {
            for (auto&& [k, v] : *lp_tv->as_table()) {
                if (auto d = v.value<double>()) {
                    cfg.td.last_prices[std::string(k.str())] = *d;
                } else if (auto i = v.value<int64_t>()) {
                    cfg.td.last_prices[std::string(k.str())] = static_cast<double>(*i);
                }
            }
        }
    }

    cfg.ctp = parse_ctp_config(root);
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

config::ConfigResult<TqApiConfig> load_tqapi_config(const std::string& path) noexcept {
    try {
        auto root = toml::parse_file(path);
        return build_from_toml(root);
    } catch (const toml::parse_error& e) {
        return std::unexpected(config::ConfigError{
            config::ConfigErrorCode::parse,
            fmt::format("parse '{}': {}", path, e.description().data()),
        });
    } catch (const std::exception& e) {
        return std::unexpected(config::ConfigError{
            config::ConfigErrorCode::unknown,
            fmt::format("load '{}': {}", path, e.what()),
        });
    } catch (...) {
        return std::unexpected(config::ConfigError{
            config::ConfigErrorCode::unknown,
            fmt::format("load '{}': unknown error", path),
        });
    }
}

config::ConfigResult<TqApiConfig> try_parse_tqapi_config(std::string_view toml_text) noexcept {
    try {
        return parse_tqapi_config(toml_text);
    } catch (const std::exception& e) {
        return std::unexpected(config::ConfigError{
            config::ConfigErrorCode::parse,
            fmt::format("parse TOML: {}", e.what()),
        });
    } catch (...) {
        return std::unexpected(config::ConfigError{
            config::ConfigErrorCode::unknown,
            "parse TOML: unknown error",
        });
    }
}

asio::awaitable<std::unique_ptr<TqApi>> make_tqapi(asio::io_context&           ctx,
                                                   const TqApiConfig&          cfg,
                                                   asio::any_io_executor       core_ex,
                                                   std::shared_ptr<KlineStore> kline_store) {
    auto ex = core_ex ? std::move(core_ex) : asio::any_io_executor(ctx.get_executor());

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

        ipc::IpcSession::Options opts;
        opts.local_role         = cfg.ipc.role;
        opts.local_name         = cfg.ipc.local_name;
        opts.handshake_timeout  = cfg.ipc.handshake_timeout;
        opts.heartbeat_interval = cfg.ipc.heartbeat_interval;
        opts.heartbeat_timeout  = cfg.ipc.heartbeat_timeout;
        opts.encoding           = cfg.ipc.encoding;

        auto ipc_cfg         = cfg.ipc;
        auto endpoint        = ep;
        auto session_factory = [&ctx, ex, ipc_cfg, endpoint, opts]()
            -> asio::awaitable<std::unique_ptr<ipc::IpcSession>> {
            auto channel = connect_channel(ctx, ipc_cfg, endpoint);
            auto session = std::make_unique<ipc::IpcSession>(std::move(channel), ex, opts);
            co_await session->start(); // 握手；失败抛异常
            co_return session;
        };

        client = std::make_shared<ipc::IpcClient>(session_factory, ex);
        co_await client->connect(); // 首连失败直接抛错（保持启动 fail-fast）
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
    case MdKind::Ctp:
        // CTP 只在 gw 进程（可选 tqsdk_ctp_gateway）中构造；node 直连模式不支持。
        md = nullptr;
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
    case TdKind::Sim: {
        auto sim = std::make_unique<SimTdGateway>(ex, cfg.td.initial_balance);
        for (const auto& seed : cfg.td.specs) {
            ContractSpec spec;
            spec.volume_multiple    = seed.volume_multiple;
            spec.price_tick         = seed.price_tick;
            spec.margin_rate        = seed.margin_rate;
            spec.commission_rate    = seed.commission_rate;
            spec.commission_per_lot = seed.commission_per_lot;
            sim->set_contract_spec(seed.symbol, spec);
        }
        for (const auto& [symbol, price] : cfg.td.last_prices) {
            sim->on_tick(symbol, price);
        }
        td = std::move(sim);
        break;
    }
    case TdKind::Ctp:
        // CTP 只在 gw 进程中构造。
        td = nullptr;
        break;
    case TdKind::Remote:
        td = std::make_unique<ipc::RemoteTdGateway>(
            client, ex, cfg.td.ack_timeout, cfg.td.shfe_market_as_limit);
        break;
    }

    // 远程 client 的读循环：必须在 md/td 都注册完 handler 之后启动。
    if (client) {
        client->start();
    }

    auto api = std::make_unique<TqApi>(ex, std::move(md), std::move(td), std::move(kline_store));
    // 远程模式：把共享 IpcClient 的关闭纳入 TqApi::close()，确保 close() 会
    // 同步关闭 IPC 连接（进而唤醒读循环），调用方驱动 io 后即可安全析构。
    if (client) {
        auto c = client; // 拷贝 shared_ptr 到钩子，生命周期与 api 一致
        api->add_close_hook([c]() { c->stop(); });
    }
    api->start();
    co_return api;
}

// ============================================================================
// 两进程配置解析
// ============================================================================

namespace {

// 从 [md]/[td] 表里读出 real gateway 种类与参数（gw 进程用，只允许直连种类）。
TqApiConfig::Md parse_gw_md(const toml::table& root) {
    TqApiConfig::Md md;
    if (const auto& t = require_table(root, "md"); !t.empty()) {
        md.kind     = parse_md_kind(get_str(t, "kind", "none"));
        md.host     = get_str(t, "host", md.host);
        md.port     = get_str(t, "port", md.port);
        md.path     = get_str(t, "path", md.path);
        md.auto_url = get_bool(t, "auto_url", md.auto_url);
    }
    return md;
}

TqApiConfig::Td parse_gw_td(const toml::table& root) {
    TqApiConfig::Td td;
    if (const auto& t = require_table(root, "td"); !t.empty()) {
        td.kind            = parse_td_kind(get_str(t, "kind", "none"));
        td.initial_balance = get_double(t, "initial_balance", td.initial_balance);
        td.auto_url        = get_bool(t, "auto_url", td.auto_url);
    }
    return td;
}

TqAuth::Config parse_tq_auth(const toml::table& root) {
    TqAuth::Config auth;
    if (const auto& t = require_table(root, "tq_auth"); !t.empty()) {
        auth.enabled           = get_bool(t, "enabled", auth.enabled);
        auth.user_name         = get_str(t, "user_name", auth.user_name);
        auth.password          = get_str(t, "password", auth.password);
        auth.auth_url          = get_str(t, "auth_url", auth.auth_url);
        auth.ns_url            = get_str(t, "ns_url", auth.ns_url);
        auth.broker_url_prefix = get_str(t, "broker_url_prefix", auth.broker_url_prefix);
        auth.mode              = get_str(t, "mode", auth.mode);
        auth.client_id         = get_str(t, "client_id", auth.client_id);
        auth.client_secret     = get_str(t, "client_secret", auth.client_secret);
        auth.user_agent        = get_str(t, "user_agent", auth.user_agent);
        auth.request_timeout   = std::chrono::seconds(
            std::max<int64_t>(1, get_int(t, "request_timeout_sec", auth.request_timeout.count())));
        auth.refresh_margin = std::chrono::seconds(
            std::max<int64_t>(0, get_int(t, "refresh_margin_sec", auth.refresh_margin.count())));
    }
    return auth;
}

RuntimeConfig parse_runtime(const toml::table& root) {
    RuntimeConfig rt;
    if (const auto& t = require_table(root, "runtime"); !t.empty()) {
        rt.threads = normalize_worker_threads(static_cast<int>(get_int(t, "threads", rt.threads)));
    }
    return rt;
}

GwConfig build_gw_from_toml(const toml::table& root) {
    GwConfig cfg;
    if (const auto& l = require_table(root, "listen"); !l.empty()) {
        cfg.listen.transport = parse_transport(get_str(l, "transport", "uds"));
        cfg.listen.socket    = get_str(l, "socket", cfg.listen.socket);
        cfg.listen.tcp_host  = get_str(l, "tcp_host", cfg.listen.tcp_host);
        cfg.listen.tcp_port  = static_cast<uint16_t>(get_int(l, "tcp_port", 0));
    }
    if (const auto& i = require_table(root, "ipc"); !i.empty()) {
        cfg.encoding          = parse_encoding(get_str(i, "encoding", "json"));
        cfg.handshake_timeout = std::chrono::milliseconds(get_int(i, "handshake_timeout_ms", 5000));
        cfg.heartbeat_interval =
            std::chrono::milliseconds(get_int(i, "heartbeat_interval_ms", 5000));
        cfg.heartbeat_timeout =
            std::chrono::milliseconds(get_int(i, "heartbeat_timeout_ms", 15000));
        cfg.local_name = get_str(i, "local_name", cfg.local_name);
    }
    cfg.md      = parse_gw_md(root);
    cfg.td      = parse_gw_td(root);
    cfg.ctp     = parse_ctp_config(root);
    cfg.tq_auth = parse_tq_auth(root);
    cfg.runtime = parse_runtime(root);

    // [td.specs."SYMBOL"] -> SpecSeed（仅 sim 用）
    if (const auto& td_t = require_table(root, "td"); !td_t.empty()) {
        if (auto* specs_tv = td_t.get("specs"); specs_tv && specs_tv->is_table()) {
            const auto& specs_t = *specs_tv->as_table();
            for (auto&& [k, v] : specs_t) {
                const auto* sub = v.as_table();
                if (!sub) {
                    continue;
                }
                GwConfig::SpecSeed s;
                s.symbol             = std::string(k.str());
                s.volume_multiple    = get_double(*sub, "volume_multiple", s.volume_multiple);
                s.price_tick         = get_double(*sub, "price_tick", s.price_tick);
                s.margin_rate        = get_double(*sub, "margin_rate", s.margin_rate);
                s.commission_rate    = get_double(*sub, "commission_rate", s.commission_rate);
                s.commission_per_lot = get_double(*sub, "commission_per_lot", s.commission_per_lot);
                cfg.specs.push_back(std::move(s));
            }
        }
        // [td.last_prices] SYMBOL = price
        if (auto* lp_tv = td_t.get("last_prices"); lp_tv && lp_tv->is_table()) {
            const auto& lp_t = *lp_tv->as_table();
            for (auto&& [k, v] : lp_t) {
                if (auto d = v.value<double>()) {
                    cfg.last_prices[std::string(k.str())] = *d;
                } else if (auto i = v.value<int64_t>()) {
                    cfg.last_prices[std::string(k.str())] = static_cast<double>(*i);
                }
            }
        }
    }
    return cfg;
}

NodeConfig build_node_from_toml(const toml::table& root) {
    NodeConfig cfg;
    cfg.upstream = build_from_toml(root); // 复用客户端解析（[ipc]/[md]/[td]）
    // node 一律以 Node 角色连接 gw（gw 侧声明 Gateway，两端角色必须不同）。
    cfg.upstream.ipc.role = ipc::IpcRole::Node;
    cfg.account_id        = get_str(root, "account_id", cfg.account_id);

    if (auto* arr = root.get("accounts"); arr && arr->is_array()) {
        for (auto&& item : *arr->as_array()) {
            const auto* t = item.as_table();
            if (!t) {
                continue;
            }
            NodeConfig::AccountEndpoint ep;
            ep.id      = get_str(*t, "id", "");
            ep.gw_host = get_str(*t, "gw_host", ep.gw_host);
            ep.gw_port = static_cast<uint16_t>(get_int(*t, "gw_port", 0));
            if (!ep.id.empty() && ep.gw_port != 0) {
                cfg.accounts.push_back(std::move(ep));
            }
        }
    }

    if (const auto& s = require_table(root, "serve"); !s.empty()) {
        cfg.serve_bind = get_str(s, "bind", cfg.serve_bind);
        cfg.serve_port = static_cast<uint16_t>(get_int(s, "port", cfg.serve_port));
        if (auto* arr = s.get("symbols")) {
            if (auto* a = arr->as_array()) {
                for (auto&& v : *a) {
                    if (auto sv = v.value<std::string>()) {
                        cfg.symbols.push_back(*sv);
                    }
                }
            }
        }
        if (auto* auth_tv = s.get("auth"); auth_tv && auth_tv->is_table()) {
            const auto& a      = *auth_tv->as_table();
            cfg.auth.enabled   = get_bool(a, "enabled", cfg.auth.enabled);
            cfg.auth.token     = get_str(a, "token", cfg.auth.token);
            cfg.auth.token_env = get_str(a, "token_env", cfg.auth.token_env);
            cfg.auth.allow_query_token =
                get_bool(a, "allow_query_token", cfg.auth.allow_query_token);
            cfg.auth.exempt_healthz = get_bool(a, "exempt_healthz", cfg.auth.exempt_healthz);
        }
        if (auto* ws_tv = s.get("ws"); ws_tv && ws_tv->is_table()) {
            const auto& w             = *ws_tv->as_table();
            cfg.ws.path               = get_str(w, "path", cfg.ws.path);
            cfg.ws.heartbeat_interval = std::chrono::milliseconds(
                get_int(w, "heartbeat_interval_ms", cfg.ws.heartbeat_interval.count()));
            cfg.ws.max_clients = static_cast<std::size_t>(
                std::max<int64_t>(1, get_int(w, "max_clients", cfg.ws.max_clients)));
            cfg.ws.max_queue = static_cast<std::size_t>(
                std::max<int64_t>(1, get_int(w, "max_queue", cfg.ws.max_queue)));
            cfg.ws.coalesce = std::chrono::milliseconds(
                std::max<int64_t>(0, get_int(w, "coalesce_ms", cfg.ws.coalesce.count())));
        }
    }
    if (const auto& t = require_table(root, "instruments"); !t.empty()) {
        cfg.instruments.enabled          = get_bool(t, "enabled", true);
        cfg.instruments.file             = get_str(t, "file", cfg.instruments.file);
        cfg.instruments.url              = get_str(t, "url", cfg.instruments.url);
        cfg.instruments.refresh_interval = std::chrono::seconds(
            get_int(t, "refresh_interval_sec", cfg.instruments.refresh_interval.count()));
        cfg.instruments.initial_delay = std::chrono::seconds(
            get_int(t, "initial_delay_sec", cfg.instruments.initial_delay.count()));
    }
    bool kline_file_set = false;
    bool tick_file_set  = false;
    if (const auto& t = require_table(root, "kline_cache"); !t.empty()) {
        cfg.kline_cache.enabled = get_bool(t, "enabled", true);
        if (auto* file = t.get("file")) {
            cfg.kline_cache.file = file->value<std::string>().value_or(cfg.kline_cache.file);
            kline_file_set       = true;
        }
        cfg.kline_cache.max_bars_per_series = static_cast<int>(std::max<int64_t>(
            1, get_int(t, "max_bars_per_series", cfg.kline_cache.max_bars_per_series)));
        cfg.kline_cache.flush_interval_ms   = static_cast<int>(std::max<int64_t>(
            1, get_int(t, "flush_interval_ms", cfg.kline_cache.flush_interval_ms)));
        cfg.kline_cache.max_pending_bars    = static_cast<std::size_t>(
            std::max<int64_t>(1, get_int(t, "max_pending_bars", cfg.kline_cache.max_pending_bars)));
        cfg.kline_cache.fresh_after_sec =
            std::max<int64_t>(0, get_int(t, "fresh_after_sec", cfg.kline_cache.fresh_after_sec));
    }
    if (const auto& t = require_table(root, "tick_cache"); !t.empty()) {
        cfg.tick_cache.enabled = get_bool(t, "enabled", cfg.tick_cache.enabled);
        if (auto* file = t.get("file")) {
            cfg.tick_cache.file = file->value<std::string>().value_or(cfg.tick_cache.file);
            tick_file_set       = true;
        }
        cfg.tick_cache.max_ticks_per_series = static_cast<std::size_t>(std::max<int64_t>(
            1, get_int(t, "max_ticks_per_series", cfg.tick_cache.max_ticks_per_series)));
        cfg.tick_cache.flush_interval_ms    = static_cast<int>(std::max<int64_t>(
            1, get_int(t, "flush_interval_ms", cfg.tick_cache.flush_interval_ms)));
        cfg.tick_cache.max_pending_ticks    = static_cast<std::size_t>(std::max<int64_t>(
            1, get_int(t, "max_pending_ticks", cfg.tick_cache.max_pending_ticks)));
    }
    if (const auto& m = require_table(root, "market_cache"); !m.empty()) {
        cfg.market_cache_file = get_str(m, "file", cfg.market_cache_file);
    }
    if (!kline_file_set) {
        cfg.kline_cache.file = cfg.market_cache_file;
    }
    if (!tick_file_set) {
        cfg.tick_cache.file = cfg.market_cache_file;
    }
    if (const auto& c = require_table(root, "continuous"); !c.empty()) {
        cfg.continuous.enabled = get_bool(c, "enabled", cfg.continuous.enabled);
        cfg.continuous.url     = get_str(c, "url", cfg.continuous.url);
        cfg.continuous.file    = get_str(c, "file", cfg.continuous.file);
        cfg.continuous.refresh_interval =
            std::chrono::seconds(std::max<int64_t>(60, get_int(c, "refresh_interval_sec", 86400)));
        cfg.continuous.initial_delay =
            std::chrono::seconds(std::max<int64_t>(0, get_int(c, "initial_delay_sec", 1)));
    }
    if (const auto& a = require_table(root, "audit"); !a.empty()) {
        cfg.audit.enabled   = get_bool(a, "enabled", cfg.audit.enabled);
        cfg.audit.file      = get_str(a, "file", cfg.audit.file);
        cfg.audit.max_bytes = static_cast<std::size_t>(
            std::max<int64_t>(1024, get_int(a, "max_bytes", cfg.audit.max_bytes)));
        cfg.audit.max_files = static_cast<std::size_t>(
            std::max<int64_t>(1, get_int(a, "max_files", cfg.audit.max_files)));
    }
    cfg.tq_auth = parse_tq_auth(root);
    cfg.runtime = parse_runtime(root);
    return cfg;
}

} // namespace

GwConfig parse_gw_config(std::string_view toml_text) {
    toml::table root;
    try {
        root = toml::parse(toml_text);
    } catch (const toml::parse_error& e) {
        throw std::runtime_error(std::string("gw config: toml parse error: ") +
                                 e.description().data());
    }
    return build_gw_from_toml(root);
}

NodeConfig parse_node_config(std::string_view toml_text) {
    toml::table root;
    try {
        root = toml::parse(toml_text);
    } catch (const toml::parse_error& e) {
        throw std::runtime_error(std::string("node config: toml parse error: ") +
                                 e.description().data());
    }
    return build_node_from_toml(root);
}

config::ConfigResult<GwConfig> load_gw_config(const std::string& path) noexcept {
    try {
        auto root = toml::parse_file(path);
        return build_gw_from_toml(root);
    } catch (const toml::parse_error& e) {
        return std::unexpected(config::ConfigError{
            config::ConfigErrorCode::parse,
            fmt::format("gw config parse '{}': {}", path, e.description().data()),
        });
    } catch (const std::exception& e) {
        return std::unexpected(config::ConfigError{
            config::ConfigErrorCode::unknown,
            fmt::format("gw config load '{}': {}", path, e.what()),
        });
    } catch (...) {
        return std::unexpected(config::ConfigError{
            config::ConfigErrorCode::unknown,
            fmt::format("gw config load '{}': unknown error", path),
        });
    }
}

config::ConfigResult<GwConfig> try_parse_gw_config(std::string_view toml_text) noexcept {
    try {
        return parse_gw_config(toml_text);
    } catch (const std::exception& e) {
        return std::unexpected(config::ConfigError{
            config::ConfigErrorCode::parse,
            fmt::format("gw config parse: {}", e.what()),
        });
    } catch (...) {
        return std::unexpected(config::ConfigError{
            config::ConfigErrorCode::unknown,
            "gw config parse: unknown error",
        });
    }
}

config::ConfigResult<NodeConfig> load_node_config(const std::string& path) noexcept {
    try {
        auto root = toml::parse_file(path);
        return build_node_from_toml(root);
    } catch (const toml::parse_error& e) {
        return std::unexpected(config::ConfigError{
            config::ConfigErrorCode::parse,
            fmt::format("node config parse '{}': {}", path, e.description().data()),
        });
    } catch (const std::exception& e) {
        return std::unexpected(config::ConfigError{
            config::ConfigErrorCode::unknown,
            fmt::format("node config load '{}': {}", path, e.what()),
        });
    } catch (...) {
        return std::unexpected(config::ConfigError{
            config::ConfigErrorCode::unknown,
            fmt::format("node config load '{}': unknown error", path),
        });
    }
}

config::ConfigResult<NodeConfig> try_parse_node_config(std::string_view toml_text) noexcept {
    try {
        return parse_node_config(toml_text);
    } catch (const std::exception& e) {
        return std::unexpected(config::ConfigError{
            config::ConfigErrorCode::parse,
            fmt::format("node config parse: {}", e.what()),
        });
    } catch (...) {
        return std::unexpected(config::ConfigError{
            config::ConfigErrorCode::unknown,
            "node config parse: unknown error",
        });
    }
}

} // namespace tqsdk

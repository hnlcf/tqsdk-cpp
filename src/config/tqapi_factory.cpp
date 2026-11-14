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

#include <set>

namespace tqsdk {

namespace {

struct ConfigParseState {
    bool                    failed = false;
    config::ConfigErrorCode code   = config::ConfigErrorCode::invalid_value;
    std::string             detail;
};

thread_local ConfigParseState* g_config_parse_state = nullptr;

void fail_parse(config::ConfigErrorCode code, std::string detail) {
    if (g_config_parse_state != nullptr && !g_config_parse_state->failed) {
        g_config_parse_state->failed = true;
        g_config_parse_state->code   = code;
        g_config_parse_state->detail = std::move(detail);
    }
}

struct ParseGuard {
    explicit ParseGuard(ConfigParseState& state) { g_config_parse_state = &state; }
    ~ParseGuard() { g_config_parse_state = nullptr; }
    ParseGuard(const ParseGuard&)            = delete;
    ParseGuard& operator=(const ParseGuard&) = delete;
};

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
    fail_parse(config::ConfigErrorCode::invalid_value, fmt::format("unknown [md].kind = '{}'", s));
    return MdKind::None;
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
    if (s == "tq") {
        return TdKind::Tq;
    }
    if (s == "remote") {
        return TdKind::Remote;
    }
    if (s == "mock") {
        return TdKind::Mock;
    }
    fail_parse(config::ConfigErrorCode::invalid_value, fmt::format("unknown [td].kind = '{}'", s));
    return TdKind::None;
}

IpcTransport parse_transport(std::string_view s) {
    if (s == "uds") {
        return IpcTransport::Uds;
    }
    if (s == "tcp") {
        return IpcTransport::Tcp;
    }
    fail_parse(config::ConfigErrorCode::invalid_value,
               fmt::format("unknown [ipc].transport = '{}'", s));
    return IpcTransport::Uds;
}

ipc::FrameEncoding parse_encoding(std::string_view s) {
    if (s.empty() || s == "json") {
        return ipc::FrameEncoding::Json;
    }
    if (s == "msgpack") {
        return ipc::FrameEncoding::MessagePack;
    }
    fail_parse(config::ConfigErrorCode::invalid_value,
               fmt::format("unknown [ipc].encoding = '{}'", s));
    return ipc::FrameEncoding::Json;
}

// ---------- TOML getters ----------

std::string get_str(const toml::table& tbl, std::string_view key, std::string def = {}) {
    if (auto* v = tbl.get(key)) {
        if (auto s = v->value<std::string>()) {
            return *s;
        }
        fail_parse(config::ConfigErrorCode::invalid_value,
                   fmt::format("key '{}' must be a string", key));
        return def;
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
        fail_parse(config::ConfigErrorCode::invalid_value,
                   fmt::format("key '{}' must be a number", key));
        return def;
    }
    return def;
}

int64_t get_int(const toml::table& tbl, std::string_view key, int64_t def) {
    if (auto* v = tbl.get(key)) {
        if (auto i = v->value<int64_t>()) {
            return *i;
        }
        fail_parse(config::ConfigErrorCode::invalid_value,
                   fmt::format("key '{}' must be an integer", key));
        return def;
    }
    return def;
}

bool get_bool(const toml::table& tbl, std::string_view key, bool def) {
    if (auto* v = tbl.get(key)) {
        if (auto b = v->value<bool>()) {
            return *b;
        }
        fail_parse(config::ConfigErrorCode::invalid_value,
                   fmt::format("key '{}' must be a boolean", key));
        return def;
    }
    return def;
}

const toml::table& require_table(const toml::table& root, std::string_view name) {
    const toml::table*       t = root[name].as_table();
    static const toml::table empty{};
    return t ? *t : empty;
}

// ---------- 结构解析 ----------

IpcConfig parse_ipc_config(const toml::table& t, std::string_view fallback_name) {
    IpcConfig cfg;
    cfg.name              = get_str(t, "name", std::string(fallback_name));
    cfg.transport         = parse_transport(get_str(t, "transport", "uds"));
    cfg.socket            = get_str(t, "socket", cfg.socket);
    cfg.tcp_host          = get_str(t, "tcp_host", cfg.tcp_host);
    const auto tcp_port   = get_int(t, "tcp_port", cfg.tcp_port);
    cfg.tcp_port          = static_cast<uint16_t>(std::clamp<int64_t>(tcp_port, 0, 65535));
    cfg.encoding          = parse_encoding(get_str(t, "encoding", "json"));
    cfg.handshake_timeout = std::chrono::milliseconds(
        std::max<int64_t>(0, get_int(t, "handshake_timeout_ms", cfg.handshake_timeout.count())));
    cfg.heartbeat_interval = std::chrono::milliseconds(
        std::max<int64_t>(0, get_int(t, "heartbeat_interval_ms", cfg.heartbeat_interval.count())));
    cfg.heartbeat_timeout = std::chrono::milliseconds(
        std::max<int64_t>(0, get_int(t, "heartbeat_timeout_ms", cfg.heartbeat_timeout.count())));

    if (cfg.transport == IpcTransport::Uds && cfg.socket.empty()) {
        fail_parse(config::ConfigErrorCode::invalid_value,
                   fmt::format("[ipc] '{}' requires socket for uds", cfg.name));
    }
    if (cfg.transport == IpcTransport::Tcp &&
        (cfg.tcp_host.empty() || tcp_port <= 0 || tcp_port > 65535)) {
        fail_parse(config::ConfigErrorCode::invalid_value,
                   fmt::format("[ipc] '{}' requires tcp_host and tcp_port", cfg.name));
    }
    return cfg;
}

TqMdConfig parse_tq_md(const toml::table& t) {
    TqMdConfig cfg;
    cfg.host     = get_str(t, "host", cfg.host);
    cfg.port     = get_str(t, "port", cfg.port);
    cfg.path     = get_str(t, "path", cfg.path);
    cfg.auto_url = get_bool(t, "auto_url", cfg.auto_url);
    return cfg;
}

MdConfig parse_md(const toml::table& root) {
    MdConfig cfg;
    if (const auto& t = require_table(root, "md"); !t.empty()) {
        cfg.kind = parse_md_kind(get_str(t, "kind", "none"));
        if (const auto* tq = t.get("tq"); tq != nullptr && tq->is_table()) {
            cfg.tq = parse_tq_md(*tq->as_table());
        }
    }
    return cfg;
}

TqTdConfig parse_tq_td(const toml::table& t) {
    TqTdConfig cfg;
    cfg.broker_name = get_str(t, "broker_name", cfg.broker_name);
    cfg.account_id  = get_str(t, "account_id", cfg.account_id);
    cfg.password    = get_str(t, "password", cfg.password);
    cfg.broker_id   = get_str(t, "broker_id", cfg.broker_id);
    cfg.front_url   = get_str(t, "front_url", cfg.front_url);
    cfg.td_url      = get_str(t, "td_url", cfg.td_url);
    cfg.account_key = get_str(t, "account_key", cfg.account_key);
    return cfg;
}

SimConfig parse_sim(const toml::table& t) {
    SimConfig cfg;
    cfg.initial_balance = get_double(t, "initial_balance", cfg.initial_balance);
    if (auto* specs_tv = t.get("specs"); specs_tv != nullptr && specs_tv->is_table()) {
        for (auto&& [k, v] : *specs_tv->as_table()) {
            const auto* sub = v.as_table();
            if (sub == nullptr) {
                continue;
            }
            SimSpec spec;
            spec.symbol          = std::string(k.str());
            spec.volume_multiple = get_double(*sub, "volume_multiple", spec.volume_multiple);
            spec.price_tick      = get_double(*sub, "price_tick", spec.price_tick);
            spec.margin_rate     = get_double(*sub, "margin_rate", spec.margin_rate);
            spec.commission_rate = get_double(*sub, "commission_rate", spec.commission_rate);
            spec.commission_per_lot =
                get_double(*sub, "commission_per_lot", spec.commission_per_lot);
            cfg.specs.push_back(std::move(spec));
        }
    }
    if (auto* lp_tv = t.get("last_prices"); lp_tv != nullptr && lp_tv->is_table()) {
        for (auto&& [k, v] : *lp_tv->as_table()) {
            if (auto d = v.value<double>()) {
                cfg.last_prices[std::string(k.str())] = *d;
            } else if (auto i = v.value<int64_t>()) {
                cfg.last_prices[std::string(k.str())] = static_cast<double>(*i);
            }
        }
    }
    return cfg;
}

TdConfig parse_td(const toml::table& root) {
    TdConfig cfg;
    if (const auto& t = require_table(root, "td"); !t.empty()) {
        cfg.kind        = parse_td_kind(get_str(t, "kind", "none"));
        cfg.auto_url    = get_bool(t, "auto_url", cfg.auto_url);
        cfg.ack_timeout = std::chrono::milliseconds(
            std::max<int64_t>(0, get_int(t, "ack_timeout_ms", cfg.ack_timeout.count())));
        cfg.shfe_market_as_limit = get_bool(t, "shfe_market_as_limit", cfg.shfe_market_as_limit);
        if (const auto* sim = t.get("sim"); sim != nullptr && sim->is_table()) {
            cfg.sim = parse_sim(*sim->as_table());
        }
        if (const auto* tq = t.get("tq"); tq != nullptr && tq->is_table()) {
            cfg.tq = parse_tq_td(*tq->as_table());
        }
    }
    return cfg;
}

CtpConfig parse_ctp(const toml::table& root) {
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

ServeConfig parse_serve(const toml::table& root) {
    ServeConfig cfg;
    const auto& s = require_table(root, "serve");
    if (!s.empty()) {
        cfg.bind = get_str(s, "bind", cfg.bind);
        cfg.port =
            static_cast<uint16_t>(std::clamp<int64_t>(get_int(s, "port", cfg.port), 0, 65535));
        if (auto* arr = s.get("symbols"); arr != nullptr && arr->is_array()) {
            for (auto&& v : *arr->as_array()) {
                if (auto sv = v.value<std::string>()) {
                    cfg.symbols.push_back(*sv);
                }
            }
        }
        if (const auto* auth_tv = s.get("auth"); auth_tv != nullptr && auth_tv->is_table()) {
            const auto& a      = *auth_tv->as_table();
            cfg.auth.enabled   = get_bool(a, "enabled", cfg.auth.enabled);
            cfg.auth.token     = get_str(a, "token", cfg.auth.token);
            cfg.auth.token_env = get_str(a, "token_env", cfg.auth.token_env);
            cfg.auth.allow_query_token =
                get_bool(a, "allow_query_token", cfg.auth.allow_query_token);
            cfg.auth.exempt_healthz = get_bool(a, "exempt_healthz", cfg.auth.exempt_healthz);
        }
        if (const auto* ws_tv = s.get("ws"); ws_tv != nullptr && ws_tv->is_table()) {
            const auto& w             = *ws_tv->as_table();
            cfg.ws.path               = get_str(w, "path", cfg.ws.path);
            cfg.ws.heartbeat_interval = std::chrono::milliseconds(std::max<int64_t>(
                0, get_int(w, "heartbeat_interval_ms", cfg.ws.heartbeat_interval.count())));
            cfg.ws.max_clients        = static_cast<std::size_t>(
                std::max<int64_t>(1, get_int(w, "max_clients", cfg.ws.max_clients)));
            cfg.ws.max_queue = static_cast<std::size_t>(
                std::max<int64_t>(1, get_int(w, "max_queue", cfg.ws.max_queue)));
            cfg.ws.coalesce = std::chrono::milliseconds(
                std::max<int64_t>(0, get_int(w, "coalesce_ms", cfg.ws.coalesce.count())));
        }
    }
    return cfg;
}

CacheConfig parse_cache(const toml::table& root) {
    CacheConfig cfg;
    const auto& c = require_table(root, "cache");
    if (c.empty()) {
        return cfg;
    }
    cfg.file = get_str(c, "file", cfg.file);

    if (const auto& t = require_table(c, "instruments"); !t.empty()) {
        cfg.instruments.enabled          = get_bool(t, "enabled", cfg.instruments.enabled);
        cfg.instruments.file             = get_str(t, "file", cfg.instruments.file);
        cfg.instruments.url              = get_str(t, "url", cfg.instruments.url);
        cfg.instruments.refresh_interval = std::chrono::seconds(
            get_int(t, "refresh_interval_sec", cfg.instruments.refresh_interval.count()));
        cfg.instruments.initial_delay = std::chrono::seconds(
            get_int(t, "initial_delay_sec", cfg.instruments.initial_delay.count()));
    }

    bool kline_file_set = false;
    if (const auto& t = require_table(c, "kline"); !t.empty()) {
        cfg.kline.enabled = get_bool(t, "enabled", cfg.kline.enabled);
        if (auto* file = t.get("file"); file != nullptr) {
            cfg.kline.file = file->value<std::string>().value_or(cfg.kline.file);
            kline_file_set = true;
        }
        cfg.kline.max_bars_per_series = static_cast<int>(
            std::max<int64_t>(1, get_int(t, "max_bars_per_series", cfg.kline.max_bars_per_series)));
        cfg.kline.flush_interval_ms = static_cast<int>(
            std::max<int64_t>(1, get_int(t, "flush_interval_ms", cfg.kline.flush_interval_ms)));
        cfg.kline.max_pending_bars = static_cast<std::size_t>(
            std::max<int64_t>(1, get_int(t, "max_pending_bars", cfg.kline.max_pending_bars)));
        cfg.kline.fresh_after_sec =
            std::max<int64_t>(0, get_int(t, "fresh_after_sec", cfg.kline.fresh_after_sec));
    }

    bool tick_file_set = false;
    if (const auto& t = require_table(c, "tick"); !t.empty()) {
        cfg.tick.enabled = get_bool(t, "enabled", cfg.tick.enabled);
        if (auto* file = t.get("file"); file != nullptr) {
            cfg.tick.file = file->value<std::string>().value_or(cfg.tick.file);
            tick_file_set = true;
        }
        cfg.tick.max_ticks_per_series = static_cast<std::size_t>(std::max<int64_t>(
            1, get_int(t, "max_ticks_per_series", cfg.tick.max_ticks_per_series)));
        cfg.tick.flush_interval_ms    = static_cast<int>(
            std::max<int64_t>(1, get_int(t, "flush_interval_ms", cfg.tick.flush_interval_ms)));
        cfg.tick.max_pending_ticks = static_cast<std::size_t>(
            std::max<int64_t>(1, get_int(t, "max_pending_ticks", cfg.tick.max_pending_ticks)));
    }

    if (!kline_file_set) {
        cfg.kline.file = cfg.file;
    }
    if (!tick_file_set) {
        cfg.tick.file = cfg.file;
    }

    if (const auto& t = require_table(c, "continuous"); !t.empty()) {
        cfg.continuous.enabled = get_bool(t, "enabled", cfg.continuous.enabled);
        cfg.continuous.url     = get_str(t, "url", cfg.continuous.url);
        cfg.continuous.file    = get_str(t, "file", cfg.continuous.file);
        cfg.continuous.refresh_interval =
            std::chrono::seconds(std::max<int64_t>(60, get_int(t, "refresh_interval_sec", 86400)));
        cfg.continuous.initial_delay =
            std::chrono::seconds(std::max<int64_t>(0, get_int(t, "initial_delay_sec", 1)));
    }
    return cfg;
}

audit::Options parse_audit(const toml::table& root) {
    audit::Options cfg;
    if (const auto& t = require_table(root, "audit"); !t.empty()) {
        cfg.enabled   = get_bool(t, "enabled", cfg.enabled);
        cfg.file      = get_str(t, "file", cfg.file);
        cfg.max_bytes = static_cast<std::size_t>(
            std::max<int64_t>(1024, get_int(t, "max_bytes", cfg.max_bytes)));
        cfg.max_files =
            static_cast<std::size_t>(std::max<int64_t>(1, get_int(t, "max_files", cfg.max_files)));
    }
    return cfg;
}

// ---------- include 合并 ----------

void merge_toml_table(toml::table& dst, const toml::table& src) {
    for (auto&& [k, v] : src) {
        const auto key      = std::string(k.str());
        auto*      existing = dst.get(key);
        if (existing != nullptr && existing->is_table() && v.is_table()) {
            merge_toml_table(*existing->as_table(), *v.as_table());
        } else {
            dst.insert_or_assign(key, v);
        }
    }
}

std::filesystem::path normalized_config_path(const std::filesystem::path& path) {
    std::error_code ec;
    auto            result = std::filesystem::weakly_canonical(path, ec);
    if (!ec) {
        return result;
    }
    result = std::filesystem::absolute(path, ec);
    return ec ? path.lexically_normal() : result.lexically_normal();
}

config::ConfigResult<toml::table> load_toml_with_includes(const std::filesystem::path& entry,
                                                          std::set<std::string>&       stack,
                                                          int depth) noexcept {
    if (depth > 32) {
        return std::unexpected(config::ConfigError{
            config::ConfigErrorCode::invalid_value,
            fmt::format("config include depth exceeds 32: {}", entry.string()),
        });
    }

    const auto normalized = normalized_config_path(entry);
    const auto key        = normalized.string();
    if (!stack.insert(key).second) {
        return std::unexpected(config::ConfigError{
            config::ConfigErrorCode::invalid_value,
            fmt::format("config include cycle at '{}'", entry.string()),
        });
    }

    std::error_code fs_ec;
    if (!std::filesystem::is_regular_file(normalized, fs_ec)) {
        stack.erase(key);
        return std::unexpected(config::ConfigError{
            config::ConfigErrorCode::file_io,
            fmt::format("config file not found: {}", entry.string()),
        });
    }

    toml::table root;
    try {
        root = toml::parse_file(normalized.string());
    } catch (const toml::parse_error& e) {
        stack.erase(key);
        return std::unexpected(config::ConfigError{
            config::ConfigErrorCode::parse,
            fmt::format("parse '{}': {}", entry.string(), e.description().data()),
        });
    } catch (const std::exception& e) {
        stack.erase(key);
        return std::unexpected(config::ConfigError{
            config::ConfigErrorCode::unknown,
            fmt::format("load '{}': {}", entry.string(), e.what()),
        });
    } catch (...) {
        stack.erase(key);
        return std::unexpected(config::ConfigError{
            config::ConfigErrorCode::unknown,
            fmt::format("load '{}': unknown error", entry.string()),
        });
    }

    std::vector<std::filesystem::path> includes;
    if (auto* include_node = root.get("include"); include_node != nullptr) {
        const auto* include_table = include_node->as_table();
        if (include_table == nullptr) {
            stack.erase(key);
            return std::unexpected(config::ConfigError{
                config::ConfigErrorCode::invalid_value,
                fmt::format("[include] must be a table in '{}'", entry.string()),
            });
        }
        const auto* files = include_table->get("files");
        if (files == nullptr || !files->is_array()) {
            stack.erase(key);
            return std::unexpected(config::ConfigError{
                config::ConfigErrorCode::invalid_value,
                fmt::format("[include].files must be an array in '{}'", entry.string()),
            });
        }
        for (auto&& item : *files->as_array()) {
            const auto* value = item.as_string();
            if (value == nullptr || value->get().empty()) {
                stack.erase(key);
                return std::unexpected(config::ConfigError{
                    config::ConfigErrorCode::invalid_value,
                    fmt::format("[include].files entries must be non-empty strings in '{}'",
                                entry.string()),
                });
            }
            auto path = std::filesystem::path(value->get());
            if (path.is_relative()) {
                path = entry.parent_path() / path;
            }
            includes.push_back(std::move(path));
        }
        root.erase("include");
    }

    toml::table merged;
    for (const auto& include : includes) {
        auto included = load_toml_with_includes(include, stack, depth + 1);
        if (!included) {
            stack.erase(key);
            return std::unexpected(std::move(included.error()));
        }
        merge_toml_table(merged, *included);
    }
    merge_toml_table(merged, root);
    stack.erase(key);
    return merged;
}

std::filesystem::path config_entry_path(const std::string& path, std::string_view entry_name) {
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) {
        return std::filesystem::path(path) / std::string(entry_name);
    }
    return std::filesystem::path(path);
}

config::ConfigResult<IpcConfig> resolve_ipc_group(const toml::table&           root,
                                                  const std::filesystem::path& base_dir,
                                                  std::string_view             key) noexcept {
    if (key.empty()) {
        return std::unexpected(config::ConfigError{
            config::ConfigErrorCode::invalid_value,
            "ipc group key is empty",
        });
    }

    const auto& ipc_table = require_table(root, "ipc");
    if (const auto* inline_group = ipc_table.get(key);
        inline_group != nullptr && inline_group->is_table()) {
        return parse_ipc_config(*inline_group->as_table(), key);
    }

    if (base_dir.empty()) {
        return std::unexpected(config::ConfigError{
            config::ConfigErrorCode::invalid_value,
            fmt::format("ipc group '{}' is not inline; use load_gw_config/load_node_config", key),
        });
    }

    auto path = std::filesystem::path(key);
    if (path.extension() != ".toml") {
        path += ".toml";
    }
    if (path.is_relative()) {
        path = base_dir / "ipc" / path;
    }

    std::set<std::string> stack;
    auto                  group_root = load_toml_with_includes(path, stack, 0);
    if (!group_root) {
        return std::unexpected(std::move(group_root.error()));
    }
    const auto& group_table = require_table(*group_root, "ipc");
    if (group_table.empty()) {
        return std::unexpected(config::ConfigError{
            config::ConfigErrorCode::invalid_value,
            fmt::format("ipc group '{}' must contain [ipc]", key),
        });
    }
    return parse_ipc_config(group_table, key);
}

config::ConfigResult<IpcConfig>
gateway_ipc_from_root(const toml::table& root, const std::filesystem::path& base_dir) noexcept {
    const auto& gateway = require_table(root, "gateway");
    const auto  key     = get_str(gateway, "ipc", "");
    if (key.empty()) {
        return std::unexpected(config::ConfigError{
            config::ConfigErrorCode::invalid_value,
            "[gateway].ipc is required",
        });
    }
    return resolve_ipc_group(root, base_dir, key);
}

config::ConfigResult<std::vector<NodeConfig::AccountEndpoint>>
node_accounts_from_root(const toml::table& root, const std::filesystem::path& base_dir) noexcept {
    std::vector<NodeConfig::AccountEndpoint> accounts;
    std::set<std::string>                    ids;
    const auto*                              arr = root.get("accounts");
    if (arr == nullptr || !arr->is_array()) {
        return std::unexpected(config::ConfigError{
            config::ConfigErrorCode::invalid_value,
            "[[accounts]] is required",
        });
    }

    for (auto&& item : *arr->as_array()) {
        const auto* t = item.as_table();
        if (t == nullptr) {
            continue;
        }
        NodeConfig::AccountEndpoint ep;
        ep.id = get_str(*t, "id", "");
        if (ep.id.empty()) {
            return std::unexpected(config::ConfigError{
                config::ConfigErrorCode::invalid_value,
                "[[accounts]].id must not be empty",
            });
        }
        if (!ids.insert(ep.id).second) {
            return std::unexpected(config::ConfigError{
                config::ConfigErrorCode::invalid_value,
                fmt::format("duplicate account id '{}'", ep.id),
            });
        }
        const auto ipc_key = get_str(*t, "ipc", "");
        if (ipc_key.empty()) {
            return std::unexpected(config::ConfigError{
                config::ConfigErrorCode::invalid_value,
                fmt::format("[[accounts]] '{}' requires ipc key", ep.id),
            });
        }
        auto group = resolve_ipc_group(root, base_dir, ipc_key);
        if (!group) {
            return std::unexpected(std::move(group.error()));
        }
        ep.ipc = std::move(*group);
        accounts.push_back(std::move(ep));
    }

    if (accounts.empty()) {
        return std::unexpected(config::ConfigError{
            config::ConfigErrorCode::invalid_value,
            "at least one [[accounts]] entry is required",
        });
    }
    return accounts;
}

GatewayConfig build_gateway_config(toml::table root, IpcConfig ipc) {
    GatewayConfig cfg;
    cfg.engine.ipc = std::move(ipc);
    cfg.engine.md  = parse_md(root);
    cfg.engine.td  = parse_td(root);
    cfg.engine.ctp = parse_ctp(root);
    cfg.tq_auth    = parse_tq_auth(root);
    cfg.runtime    = parse_runtime(root);

    if (cfg.engine.td.kind == TdKind::Tq && cfg.engine.td.tq.broker_name.empty()) {
        fail_parse(config::ConfigErrorCode::invalid_value,
                   "[td.tq].broker_name is required when [td].kind = tq");
    }
    return cfg;
}

NodeConfig build_node_config(toml::table                              root,
                             std::vector<NodeConfig::AccountEndpoint> accounts,
                             std::string                              primary_account) {
    NodeConfig cfg;
    cfg.accounts        = std::move(accounts);
    cfg.primary_account = std::move(primary_account);
    if (cfg.primary_account.empty()) {
        cfg.primary_account = cfg.accounts.front().id;
    }

    const auto primary = std::find_if(cfg.accounts.begin(), cfg.accounts.end(), [&](const auto& a) {
        return a.id == cfg.primary_account;
    });
    if (primary == cfg.accounts.end()) {
        fail_parse(config::ConfigErrorCode::invalid_value,
                   fmt::format("primary_account '{}' not found", cfg.primary_account));
    }

    cfg.serve   = parse_serve(root);
    cfg.cache   = parse_cache(root);
    cfg.tq_auth = parse_tq_auth(root);
    cfg.audit   = parse_audit(root);
    cfg.runtime = parse_runtime(root);
    return cfg;
}

// ---------- IPC channel ----------

std::unique_ptr<ipc::IIpcChannel> connect_channel(asio::io_context& ctx, const IpcConfig& cfg) {
    if (cfg.transport == IpcTransport::Uds) {
        if (cfg.socket.empty()) {
            fail_parse(config::ConfigErrorCode::invalid_value,
                       "uds transport requires socket path");
            return nullptr;
        }
        return ipc::UdsChannel::connect(ctx, cfg.socket);
    }
    if (cfg.tcp_host.empty() || cfg.tcp_port == 0) {
        fail_parse(config::ConfigErrorCode::invalid_value, "tcp transport requires host and port");
        return nullptr;
    }
    return ipc::TcpChannel::connect(ctx, cfg.tcp_host, cfg.tcp_port);
}

} // namespace

EngineConfig make_remote_engine_config(const IpcConfig& ipc, std::string account_id) {
    EngineConfig cfg;
    cfg.ipc      = ipc;
    cfg.ipc.name = fmt::format("node-{}", account_id.empty() ? "default" : std::move(account_id));
    cfg.md.kind  = MdKind::Remote;
    cfg.td.kind  = TdKind::Remote;
    return cfg;
}

asio::awaitable<gateway::GatewayResult<std::unique_ptr<TqApi>>>
make_tqapi(asio::io_context&           ctx,
           const EngineConfig&         cfg,
           asio::any_io_executor       core_ex,
           std::shared_ptr<KlineStore> kline_store) noexcept {
    auto ex = core_ex ? std::move(core_ex) : asio::any_io_executor(ctx.get_executor());

    const bool need_md_remote = cfg.md.kind == MdKind::Remote;
    const bool need_td_remote = cfg.td.kind == TdKind::Remote;

    std::shared_ptr<ipc::IpcClient> client;
    if (need_md_remote || need_td_remote) {
        ipc::IpcSession::Options opts;
        opts.local_role         = ipc::IpcRole::Node;
        opts.local_name         = cfg.ipc.name;
        opts.handshake_timeout  = cfg.ipc.handshake_timeout;
        opts.heartbeat_interval = cfg.ipc.heartbeat_interval;
        opts.heartbeat_timeout  = cfg.ipc.heartbeat_timeout;
        opts.encoding           = cfg.ipc.encoding;

        auto endpoint        = cfg.ipc;
        auto session_factory = [&ctx, ex, endpoint, opts]()
            -> asio::awaitable<ipc::IpcResult<std::unique_ptr<ipc::IpcSession>>> {
            auto channel = connect_channel(ctx, endpoint);
            if (!channel) {
                co_return std::unexpected(
                    ipc::IpcFailure{ipc::IpcErrorCode::connect, "cannot create IPC channel"});
            }
            auto session = std::make_unique<ipc::IpcSession>(std::move(channel), ex, opts);
            auto started = co_await session->start();
            if (!started) {
                co_return std::unexpected(started.error());
            }
            co_return session;
        };

        client              = std::make_shared<ipc::IpcClient>(session_factory, ex);
        auto connect_result = co_await client->connect();
        if (!connect_result) {
            co_return std::unexpected(gateway::GatewayError{
                gateway::GatewayErrorCode::connect,
                fmt::format("{} ({})",
                            connect_result.error().detail,
                            ipc::ipc_error_name(connect_result.error().code)),
            });
        }
    }

    std::unique_ptr<MarketDataGateway> md;
    switch (cfg.md.kind) {
    case MdKind::None:
    case MdKind::Mock:
    case MdKind::Ctp:
        md = nullptr;
        break;
    case MdKind::Tq:
        md = std::make_unique<TqMdGateway>(ex, cfg.md.tq.host, cfg.md.tq.port, cfg.md.tq.path);
        break;
    case MdKind::Remote:
        md = std::make_unique<ipc::RemoteMdGateway>(client, ex);
        break;
    }

    std::unique_ptr<TradingGateway> td;
    switch (cfg.td.kind) {
    case TdKind::None:
    case TdKind::Mock:
    case TdKind::Ctp:
    case TdKind::Tq:
        td = nullptr;
        break;
    case TdKind::Sim: {
        auto sim = std::make_unique<SimTdGateway>(ex, cfg.td.sim.initial_balance);
        for (const auto& seed : cfg.td.sim.specs) {
            ContractSpec spec;
            spec.volume_multiple    = seed.volume_multiple;
            spec.price_tick         = seed.price_tick;
            spec.margin_rate        = seed.margin_rate;
            spec.commission_rate    = seed.commission_rate;
            spec.commission_per_lot = seed.commission_per_lot;
            sim->set_contract_spec(seed.symbol, spec);
        }
        for (const auto& [symbol, price] : cfg.td.sim.last_prices) {
            sim->on_tick(symbol, price);
        }
        td = std::move(sim);
        break;
    }
    case TdKind::Remote:
        td = std::make_unique<ipc::RemoteTdGateway>(
            client, ex, cfg.td.ack_timeout, cfg.td.shfe_market_as_limit);
        break;
    }

    if (client) {
        client->start();
    }

    auto api = std::make_unique<TqApi>(ex, std::move(md), std::move(td), std::move(kline_store));
    if (client) {
        auto c = client;
        api->add_close_hook([c]() { c->stop(); });
    }
    api->start();
    co_return api;
}

asio::awaitable<gateway::GatewayResult<std::unique_ptr<TqApi>>>
try_make_tqapi(asio::io_context&           ctx,
               const EngineConfig&         cfg,
               asio::any_io_executor       core_ex,
               std::shared_ptr<KlineStore> kline_store) noexcept {
    co_return co_await make_tqapi(ctx, cfg, std::move(core_ex), std::move(kline_store));
}

config::ConfigResult<GatewayConfig> parse_gw_config(std::string_view toml_text) noexcept {
    toml::table      root;
    ConfigParseState state;
    ParseGuard       guard(state);
    try {
        root = toml::parse(toml_text);
    } catch (const toml::parse_error& e) {
        return std::unexpected(config::ConfigError{
            config::ConfigErrorCode::parse,
            fmt::format("gw toml parse error: {}", e.description().data()),
        });
    } catch (const std::exception& e) {
        return std::unexpected(config::ConfigError{
            config::ConfigErrorCode::unknown,
            fmt::format("gw toml parse error: {}", e.what()),
        });
    }
    auto ipc = gateway_ipc_from_root(root, {});
    if (!ipc) {
        return std::unexpected(std::move(ipc.error()));
    }
    auto cfg = build_gateway_config(std::move(root), std::move(*ipc));
    if (state.failed) {
        return std::unexpected(config::ConfigError{state.code, std::move(state.detail)});
    }
    return cfg;
}

config::ConfigResult<NodeConfig> parse_node_config(std::string_view toml_text) noexcept {
    toml::table      root;
    ConfigParseState state;
    ParseGuard       guard(state);
    try {
        root = toml::parse(toml_text);
    } catch (const toml::parse_error& e) {
        return std::unexpected(config::ConfigError{
            config::ConfigErrorCode::parse,
            fmt::format("node toml parse error: {}", e.description().data()),
        });
    } catch (const std::exception& e) {
        return std::unexpected(config::ConfigError{
            config::ConfigErrorCode::unknown,
            fmt::format("node toml parse error: {}", e.what()),
        });
    }
    auto accounts = node_accounts_from_root(root, {});
    if (!accounts) {
        return std::unexpected(std::move(accounts.error()));
    }
    auto primary = get_str(root, "primary_account", "");
    auto cfg     = build_node_config(std::move(root), std::move(*accounts), std::move(primary));
    if (state.failed) {
        return std::unexpected(config::ConfigError{state.code, std::move(state.detail)});
    }
    return cfg;
}

config::ConfigResult<GatewayConfig> load_gw_config(const std::string& path) noexcept {
    const auto            entry = config_entry_path(path, "gw.toml");
    std::set<std::string> stack;
    auto                  root = load_toml_with_includes(entry, stack, 0);
    if (!root) {
        return std::unexpected(std::move(root.error()));
    }

    ConfigParseState state;
    ParseGuard       guard(state);
    auto             ipc = gateway_ipc_from_root(*root, entry.parent_path());
    if (!ipc) {
        return std::unexpected(std::move(ipc.error()));
    }
    auto cfg = build_gateway_config(std::move(*root), std::move(*ipc));
    if (state.failed) {
        return std::unexpected(config::ConfigError{state.code, std::move(state.detail)});
    }
    return cfg;
}

config::ConfigResult<NodeConfig> load_node_config(const std::string& path) noexcept {
    const auto            entry = config_entry_path(path, "node.toml");
    std::set<std::string> stack;
    auto                  root = load_toml_with_includes(entry, stack, 0);
    if (!root) {
        return std::unexpected(std::move(root.error()));
    }

    ConfigParseState state;
    ParseGuard       guard(state);
    auto             accounts = node_accounts_from_root(*root, entry.parent_path());
    if (!accounts) {
        return std::unexpected(std::move(accounts.error()));
    }
    auto primary = get_str(*root, "primary_account", "");
    auto cfg     = build_node_config(std::move(*root), std::move(*accounts), std::move(primary));
    if (state.failed) {
        return std::unexpected(config::ConfigError{state.code, std::move(state.detail)});
    }
    return cfg;
}

config::ConfigResult<GatewayConfig> try_parse_gw_config(std::string_view toml_text) noexcept {
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

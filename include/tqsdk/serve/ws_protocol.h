#pragma once

#include "tqsdk/pch.h"

#include "tqsdk/util/json_value.h"

#include <variant>

namespace tqsdk {
namespace serve {

using Json = nlohmann::json;

/// `/stream` 当前协议版本（放在 welcome.protocol 中）。
inline constexpr int kWsProtocolVersion = 1;

/// WebSocket 协议消息解析错误（JSON 非法 / 缺字段 / op/type 未知）。
struct WsParseError {
    std::string detail;
};

template <typename T>
using WsParseResult = std::expected<T, WsParseError>;

namespace detail {

/// 把 nlohmann 的 type_error/out_of_range 统一收敛成标准参数异常；
/// 公共 parse 入口会再把它们转换成 WsParseError。
template <typename Message>
Message get_or_throw(const Json& j, const char* name) {
    try {
        return j.get<Message>();
    } catch (const std::invalid_argument&) {
        throw;
    } catch (const std::exception& e) {
        throw std::invalid_argument(fmt::format("{}: {}", name, e.what()));
    }
}

} // namespace detail

// ============================================================================
// 客户端 -> 服务端
// ============================================================================

struct KlineSubscription {
    std::string symbol;
    int64_t     duration_ns = 0;
    int         view_width  = 200;
};

inline void to_json(Json& j, const KlineSubscription& k) {
    j = Json{{"symbol", k.symbol}, {"duration_ns", k.duration_ns}, {"view_width", k.view_width}};
}
inline void from_json(const Json& j, KlineSubscription& k) {
    k.symbol      = j.value("symbol", std::string{});
    k.duration_ns = j.value("duration_ns", int64_t{0});
    k.view_width  = j.value("view_width", 200);
}

/// Build the JSON array explicitly. Passing std::vector<KlineSubscription>
/// straight into nlohmann's implicit constructor triggers a clang 18 +
/// libstdc++ 14 recursive exception-specification diagnostic.
inline Json klines_to_json(const std::vector<KlineSubscription>& klines) {
    Json out = Json::array();
    for (const auto& k : klines) {
        out.push_back(Json{
            {"symbol", k.symbol}, {"duration_ns", k.duration_ns}, {"view_width", k.view_width}});
    }
    return out;
}

/// {"op":"subscribe","symbols":[...],"klines":[...],"topics":[...]}
struct WsSubscribe {
    std::vector<std::string>       symbols;
    std::vector<KlineSubscription> klines;
    std::vector<std::string>       topics;
    /// 是否显式携带 topics 字段。缺省/空数组都表示不订阅交易 topic；
    /// UI 需要 account/orders/trades/positions 时必须显式列出。
    bool topics_provided = false;
};

/// {"op":"unsubscribe","symbols":[...],"klines":[...]}
struct WsUnsubscribe {
    std::vector<std::string>       symbols;
    std::vector<KlineSubscription> klines;
};

/// {"op":"ping"}
struct WsPing {};

inline void to_json(Json& j, const WsSubscribe& m) {
    j = Json{{"op", "subscribe"},
             {"symbols", m.symbols},
             {"klines", klines_to_json(m.klines)},
             {"topics", m.topics}};
}
inline void from_json(const Json& j, WsSubscribe& m) {
    if (j.value("op", std::string{}) != "subscribe") {
        throw std::invalid_argument("expected op=subscribe");
    }
    m.symbols.clear();
    for (const auto& s : j.value("symbols", std::vector<std::string>{})) {
        if (!s.empty()) {
            m.symbols.push_back(s);
        }
    }
    m.klines.clear();
    for (auto k : j.value("klines", std::vector<KlineSubscription>{})) {
        if (k.symbol.empty() || k.duration_ns <= 0) {
            continue;
        }
        if (k.view_width <= 0) {
            k.view_width = 200;
        }
        m.klines.push_back(std::move(k));
    }
    m.topics_provided = j.contains("topics");
    m.topics.clear();
    for (const auto& t : j.value("topics", std::vector<std::string>{})) {
        if (!t.empty()) {
            m.topics.push_back(t);
        }
    }
}

inline void to_json(Json& j, const WsUnsubscribe& m) {
    j = Json{{"op", "unsubscribe"}, {"symbols", m.symbols}, {"klines", klines_to_json(m.klines)}};
}
inline void from_json(const Json& j, WsUnsubscribe& m) {
    if (j.value("op", std::string{}) != "unsubscribe") {
        throw std::invalid_argument("expected op=unsubscribe");
    }
    m.symbols.clear();
    for (const auto& s : j.value("symbols", std::vector<std::string>{})) {
        if (!s.empty()) {
            m.symbols.push_back(s);
        }
    }
    m.klines.clear();
    for (auto k : j.value("klines", std::vector<KlineSubscription>{})) {
        if (k.symbol.empty() || k.duration_ns <= 0) {
            continue;
        }
        if (k.view_width <= 0) {
            k.view_width = 200;
        }
        m.klines.push_back(std::move(k));
    }
}

inline void to_json(Json& j, const WsPing&) {
    j = Json{{"op", "ping"}};
}
inline void from_json(const Json& j, WsPing&) {
    if (j.value("op", std::string{}) != "ping") {
        throw std::invalid_argument("expected op=ping");
    }
}

/// 客户端消息的强类型 variant。
using WsClientMessage = std::variant<WsSubscribe, WsUnsubscribe, WsPing>;

inline void to_json(Json& j, const WsClientMessage& m) {
    // Avoid a generic std::visit + JSON conversion here: libstdc++ 13/14 with
    // clang 18 can diagnose a recursive exception specification in variant.
    switch (m.index()) {
    case 0:
        to_json(j, std::get<0>(m));
        break;
    case 1:
        to_json(j, std::get<1>(m));
        break;
    case 2:
        to_json(j, std::get<2>(m));
        break;
    default:
        throw std::invalid_argument("invalid client message variant");
    }
}

/// 从 JSON DOM / 文本解析客户端消息；失败返回 WsParseError。
WsParseResult<WsClientMessage> parse_ws_client_message(const Json& j) noexcept;

template <typename Text>
inline constexpr bool kWsTextLike =
    std::is_convertible_v<const std::decay_t<Text>&, std::string_view>;

template <typename Text, std::enable_if_t<kWsTextLike<Text>, int> = 0>
WsParseResult<WsClientMessage> parse_ws_client_message(const Text& text) noexcept;

WsClientMessage parse_ws_client_message_impl(const Json& j);
template <typename Text, std::enable_if_t<kWsTextLike<Text>, int> = 0>
WsClientMessage parse_ws_client_message_impl(const Text& text);

inline void from_json(const Json& j, WsClientMessage& m) {
    m = parse_ws_client_message_impl(j);
}

// ============================================================================
// 服务端 -> 客户端
// ============================================================================

/// {"type":"welcome","protocol":1,"heartbeat_interval_ms":30000}
struct WsWelcome {
    int     protocol              = kWsProtocolVersion;
    int64_t heartbeat_interval_ms = 30'000;
};

/// {"type":"subscribed","symbols":[...],"klines":[...],"topics":[...]}
struct WsSubscribed {
    std::vector<std::string>       symbols;
    std::vector<KlineSubscription> klines;
    std::vector<std::string>       topics;
};

/// {"type":"unsubscribed","symbols":[...],"klines":[...]}
struct WsUnsubscribed {
    std::vector<std::string>       symbols;
    std::vector<KlineSubscription> klines;
};

/// {"type":"snapshot","data":{...}}
struct WsSnapshot {
    Json data = Json::object();
};

/// {"type":"diff","seq":N,"timestamp_ns":...,"data":{...}}
struct WsDiff {
    uint64_t seq          = 0;
    int64_t  timestamp_ns = 0;
    Json     data         = Json::object();
};

/// {"type":"pong"}
struct WsPong {};

/// {"type":"error","error":"..."}
struct WsError {
    std::string error;
};

inline void to_json(Json& j, const WsWelcome& m) {
    j = Json{{"type", "welcome"},
             {"protocol", m.protocol},
             {"heartbeat_interval_ms", m.heartbeat_interval_ms}};
}
inline void from_json(const Json& j, WsWelcome& m) {
    if (j.value("type", std::string{}) != "welcome") {
        throw std::invalid_argument("expected type=welcome");
    }
    m.protocol              = j.value("protocol", kWsProtocolVersion);
    m.heartbeat_interval_ms = j.value("heartbeat_interval_ms", int64_t{30'000});
}

inline void to_json(Json& j, const WsSubscribed& m) {
    j = Json{{"type", "subscribed"},
             {"symbols", m.symbols},
             {"klines", klines_to_json(m.klines)},
             {"topics", m.topics}};
}
inline void from_json(const Json& j, WsSubscribed& m) {
    if (j.value("type", std::string{}) != "subscribed") {
        throw std::invalid_argument("expected type=subscribed");
    }
    m.symbols = j.value("symbols", std::vector<std::string>{});
    m.klines  = j.value("klines", std::vector<KlineSubscription>{});
    m.topics  = j.value("topics", std::vector<std::string>{});
}

inline void to_json(Json& j, const WsUnsubscribed& m) {
    j = Json{
        {"type", "unsubscribed"}, {"symbols", m.symbols}, {"klines", klines_to_json(m.klines)}};
}
inline void from_json(const Json& j, WsUnsubscribed& m) {
    if (j.value("type", std::string{}) != "unsubscribed") {
        throw std::invalid_argument("expected type=unsubscribed");
    }
    m.symbols = j.value("symbols", std::vector<std::string>{});
    m.klines  = j.value("klines", std::vector<KlineSubscription>{});
}

inline void to_json(Json& j, const WsSnapshot& m) {
    j = Json{{"type", "snapshot"}, {"data", m.data}};
}
inline void from_json(const Json& j, WsSnapshot& m) {
    if (j.value("type", std::string{}) != "snapshot") {
        throw std::invalid_argument("expected type=snapshot");
    }
    m.data = j.value("data", Json::object());
}

inline void to_json(Json& j, const WsDiff& m) {
    j = Json{{"type", "diff"}, {"seq", m.seq}, {"timestamp_ns", m.timestamp_ns}, {"data", m.data}};
}
inline void from_json(const Json& j, WsDiff& m) {
    if (j.value("type", std::string{}) != "diff") {
        throw std::invalid_argument("expected type=diff");
    }
    m.seq          = j.value("seq", uint64_t{0});
    m.timestamp_ns = j.value("timestamp_ns", int64_t{0});
    m.data         = j.value("data", Json::object());
}

inline void to_json(Json& j, const WsPong&) {
    j = Json{{"type", "pong"}};
}
inline void from_json(const Json& j, WsPong&) {
    if (j.value("type", std::string{}) != "pong") {
        throw std::invalid_argument("expected type=pong");
    }
}

inline void to_json(Json& j, const WsError& m) {
    j = Json{{"type", "error"}, {"error", m.error}};
}
inline void from_json(const Json& j, WsError& m) {
    if (j.value("type", std::string{}) != "error") {
        throw std::invalid_argument("expected type=error");
    }
    m.error = j.value("error", std::string{});
}

/// 服务端消息的强类型 variant。
using WsServerMessage =
    std::variant<WsWelcome, WsSubscribed, WsUnsubscribed, WsSnapshot, WsDiff, WsPong, WsError>;

inline void to_json(Json& j, const WsServerMessage& m) {
    // See WsClientMessage::to_json: explicit switch is portable across
    // clang/libstdc++ combinations (notably clang 18 + libstdc++ 14).
    switch (m.index()) {
    case 0:
        to_json(j, std::get<0>(m));
        break;
    case 1:
        to_json(j, std::get<1>(m));
        break;
    case 2:
        to_json(j, std::get<2>(m));
        break;
    case 3:
        to_json(j, std::get<3>(m));
        break;
    case 4:
        to_json(j, std::get<4>(m));
        break;
    case 5:
        to_json(j, std::get<5>(m));
        break;
    case 6:
        to_json(j, std::get<6>(m));
        break;
    default:
        throw std::invalid_argument("invalid server message variant");
    }
}

/// 从 JSON DOM / 文本解析服务端消息；失败返回 WsParseError。
WsParseResult<WsServerMessage> parse_ws_server_message(const Json& j) noexcept;

template <typename Text, std::enable_if_t<kWsTextLike<Text>, int> = 0>
WsParseResult<WsServerMessage> parse_ws_server_message(const Text& text) noexcept;

WsServerMessage parse_ws_server_message_impl(const Json& j);
template <typename Text, std::enable_if_t<kWsTextLike<Text>, int> = 0>
WsServerMessage parse_ws_server_message_impl(const Text& text);

inline void from_json(const Json& j, WsServerMessage& m) {
    m = parse_ws_server_message_impl(j);
}

// ============================================================================
// 解析 / 序列化辅助
// ============================================================================

inline WsClientMessage parse_ws_client_message_impl(const Json& j) {
    if (!j.is_object()) {
        throw std::invalid_argument("client message must be a JSON object");
    }
    auto op_value = j.find("op");
    if (op_value == j.end()) {
        throw std::invalid_argument("missing op");
    }
    const auto op_opt = json_value::string(*op_value);
    if (!op_opt) {
        throw std::invalid_argument("op must be a string");
    }
    const std::string& op = *op_opt;
    if (op == "subscribe") {
        return detail::get_or_throw<WsSubscribe>(j, "subscribe");
    }
    if (op == "unsubscribe") {
        return detail::get_or_throw<WsUnsubscribe>(j, "unsubscribe");
    }
    if (op == "ping") {
        return detail::get_or_throw<WsPing>(j, "ping");
    }
    if (op.empty()) {
        throw std::invalid_argument("missing op");
    }
    throw std::invalid_argument(fmt::format("unknown op: {}", op));
}

template <typename Text, std::enable_if_t<kWsTextLike<Text>, int>>
inline WsClientMessage parse_ws_client_message_impl(const Text& text) {
    const std::string_view sv = text;
    Json                   j  = Json::parse(sv.begin(), sv.end(), nullptr, false);
    if (j.is_discarded()) {
        throw std::invalid_argument("invalid json");
    }
    return parse_ws_client_message_impl(j);
}

inline WsServerMessage parse_ws_server_message_impl(const Json& j) {
    if (!j.is_object()) {
        throw std::invalid_argument("server message must be a JSON object");
    }
    auto type_value = j.find("type");
    if (type_value == j.end()) {
        throw std::invalid_argument("missing type");
    }
    const auto type_opt = json_value::string(*type_value);
    if (!type_opt) {
        throw std::invalid_argument("type must be a string");
    }
    const std::string& type = *type_opt;
    if (type == "welcome") {
        return detail::get_or_throw<WsWelcome>(j, "welcome");
    }
    if (type == "subscribed") {
        return detail::get_or_throw<WsSubscribed>(j, "subscribed");
    }
    if (type == "unsubscribed") {
        return detail::get_or_throw<WsUnsubscribed>(j, "unsubscribed");
    }
    if (type == "snapshot") {
        return detail::get_or_throw<WsSnapshot>(j, "snapshot");
    }
    if (type == "diff") {
        return detail::get_or_throw<WsDiff>(j, "diff");
    }
    if (type == "pong") {
        return detail::get_or_throw<WsPong>(j, "pong");
    }
    if (type == "error") {
        return detail::get_or_throw<WsError>(j, "error");
    }
    if (type.empty()) {
        throw std::invalid_argument("missing type");
    }
    throw std::invalid_argument(fmt::format("unknown type: {}", type));
}

template <typename Text, std::enable_if_t<kWsTextLike<Text>, int>>
inline WsServerMessage parse_ws_server_message_impl(const Text& text) {
    const std::string_view sv = text;
    Json                   j  = Json::parse(sv.begin(), sv.end(), nullptr, false);
    if (j.is_discarded()) {
        throw std::invalid_argument("invalid json");
    }
    return parse_ws_server_message_impl(j);
}

inline WsParseResult<WsClientMessage> parse_ws_client_message(const Json& j) noexcept {
    try {
        return parse_ws_client_message_impl(j);
    } catch (const std::exception& e) {
        return std::unexpected(WsParseError{e.what()});
    }
}

template <typename Text, std::enable_if_t<kWsTextLike<Text>, int>>
inline WsParseResult<WsClientMessage> parse_ws_client_message(const Text& text) noexcept {
    const std::string_view sv = text;
    Json                   j  = Json::parse(sv.begin(), sv.end(), nullptr, false);
    if (j.is_discarded()) {
        return std::unexpected(WsParseError{"invalid json"});
    }
    return parse_ws_client_message(j);
}

inline WsParseResult<WsServerMessage> parse_ws_server_message(const Json& j) noexcept {
    try {
        return parse_ws_server_message_impl(j);
    } catch (const std::exception& e) {
        return std::unexpected(WsParseError{e.what()});
    }
}

template <typename Text, std::enable_if_t<kWsTextLike<Text>, int>>
inline WsParseResult<WsServerMessage> parse_ws_server_message(const Text& text) noexcept {
    const std::string_view sv = text;
    Json                   j  = Json::parse(sv.begin(), sv.end(), nullptr, false);
    if (j.is_discarded()) {
        return std::unexpected(WsParseError{"invalid json"});
    }
    return parse_ws_server_message(j);
}

/// 任意协议结构 / variant -> 文本帧。
template <typename Message>
std::string serialize_ws_message(const Message& msg) {
    Json j;
    to_json(j, msg);
    return j.dump();
}

} // namespace serve
} // namespace tqsdk

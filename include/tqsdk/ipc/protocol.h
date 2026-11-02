#pragma once

#include "tqsdk/pch.h"

#include "tqsdk/gateway/trading_gateway.h" // InsertOrderReq / CancelOrderReq / CommandAck
#include "codec.h"                         // 序列化抽象（JSON / MessagePack ...）
#include "frame.h"

namespace tqsdk {
namespace ipc {

using json = ::nlohmann::json;

// ============================================================================
// IPC 协议消息体（body）定义
// ----------------------------------------------------------------------------
// 帧格式固定为 [len u32 BE][type u8][encoding u8][payload]，payload 为下面对应
// FrameType 的 JSON 编码。本文件定义每类帧的 body 结构，并提供：
//   - 各字段的 to_json / from_json
//   - encode_frame(type, ...) 构造 JSON 字符串/字节
//   - decode_frame(ReceivedFrame) 校验 type/encoding 后解析出结构
//
// 设计约定：
//   * 握手/订阅/命令/心跳/关闭等"控制面"消息有强类型 body；
//   * MarketDataPush / TradePush 的 body 是"网关内部 diff JSON"（与天勤 rtn_data
//     同构），用 json::object 直接承载，不在此处细化字段；
//   * 尚未落地的帧（Snapshot/Diff/GatewayEvent/QueryOrders/QueryOrdersAck）保留
//     定义：用 OpaqueBody{json} 透传，便于未来平滑接入而不改协议头。
// ============================================================================

// ---------------------------------------------------------------------------
// 角色与服务标识
// ---------------------------------------------------------------------------

/// 连接发起方角色，用于 Hello 握手校验。
enum class IpcRole : uint8_t {
    Gateway = 1, ///< 接入进程（gw）
    Node    = 2, ///< 数据服务进程（node: core+serve）
};
DEFINE_ENUM_JSON_SERIALIZE(IpcRole)

/// Hello 默认协议版本；future 不兼容升级时递增。
inline constexpr int kIpcProtocolVersion = 1;

// ---------------------------------------------------------------------------
// 握手
// ---------------------------------------------------------------------------

struct HelloBody {
    int         version = kIpcProtocolVersion;
    IpcRole     role    = IpcRole::Gateway;
    std::string name; ///< 实例名（用于日志/多连接辨识）
};
inline void to_json(json& j, const HelloBody& m) {
    j = json{{"version", m.version}, {"role", m.role}, {"name", m.name}};
}
inline void from_json(const json& j, HelloBody& m) {
    m.version = j.at("version").get<int>();
    m.role    = j.at("role").get<IpcRole>();
    m.name    = j.value("name", std::string{});
}

struct HelloAckBody {
    bool        ok = false;
    std::string error; ///< ok == false 时有效
};
inline void to_json(json& j, const HelloAckBody& m) {
    j = json{{"ok", m.ok}, {"error", m.error}};
}
inline void from_json(const json& j, HelloAckBody& m) {
    m.ok    = j.at("ok").get<bool>();
    m.error = j.value("error", std::string{});
}

// ---------------------------------------------------------------------------
// 订阅
// ---------------------------------------------------------------------------

/// 一个 K 线订阅项。
struct KlineSub {
    InstrumentId symbol      = {};
    int64_t      duration_ns = 0;
    int          view_width  = 0;
};
inline void to_json(json& j, const KlineSub& m) {
    j = json{{"symbol", m.symbol}, {"duration_ns", m.duration_ns}, {"view_width", m.view_width}};
}
inline void from_json(const json& j, KlineSub& m) {
    m.symbol      = j.at("symbol").get<InstrumentId>();
    m.duration_ns = j.at("duration_ns").get<int64_t>();
    m.view_width  = j.at("view_width").get<int>();
}

struct SubscribeBody {
    std::vector<InstrumentId> symbols; ///< 盘口订阅
    std::vector<KlineSub>     klines;  ///< K 线订阅
};
inline void to_json(json& j, const SubscribeBody& m) {
    j = json{{"symbols", m.symbols}, {"klines", m.klines}};
}
inline void from_json(const json& j, SubscribeBody& m) {
    m.symbols = j.value("symbols", std::vector<InstrumentId>{});
    m.klines  = j.value("klines", std::vector<KlineSub>{});
}

struct SubscribeAckBody {
    bool        ok = false;
    std::string error;
};
inline void to_json(json& j, const SubscribeAckBody& m) {
    j = json{{"ok", m.ok}, {"error", m.error}};
}
inline void from_json(const json& j, SubscribeAckBody& m) {
    m.ok    = j.at("ok").get<bool>();
    m.error = j.value("error", std::string{});
}

// ---------------------------------------------------------------------------
// 行情 / 交易推送（payload 为内部 diff JSON，原样承载）
// ---------------------------------------------------------------------------

struct MarketDataPushBody {
    json data = json::object(); ///< 与天勤 rtn_data 同构的行情 diff
};
inline void to_json(json& j, const MarketDataPushBody& m) {
    j = m.data;
}
inline void from_json(const json& j, MarketDataPushBody& m) {
    m.data = j;
}

struct TradePushBody {
    json data = json::object(); ///< 含 user.orders / user.trades / user.accounts 等
};
inline void to_json(json& j, const TradePushBody& m) {
    j = m.data;
}
inline void from_json(const json& j, TradePushBody& m) {
    m.data = j;
}

// ---------------------------------------------------------------------------
// 命令（下单 / 撤单）
// ---------------------------------------------------------------------------

/// 命令操作类型。
enum class CommandOp : uint8_t {
    InsertOrder = 1,
    CancelOrder = 2,
};
DEFINE_ENUM_JSON_SERIALIZE(CommandOp)

struct CommandBody {
    CommandOp op = CommandOp::InsertOrder;

    // op == InsertOrder 时有效
    OrderId        rid = {}; ///< 本地订单号（回执关联键）
    InsertOrderReq insert;

    // op == CancelOrder 时有效
    CancelOrderReq cancel;
};
inline void to_json(json& j, const CommandBody& m) {
    j = json{{"op", m.op}};
    switch (m.op) {
    case CommandOp::InsertOrder:
        j["rid"]    = m.rid;
        j["insert"] = json{{"instrument_id", m.insert.instrument_id},
                           {"exchange_id", m.insert.exchange_id},
                           {"direction", m.insert.direction},
                           {"offset", m.insert.offset},
                           {"price_type", m.insert.price_type},
                           {"limit_price", m.insert.limit_price},
                           {"time_cond", m.insert.time_cond},
                           {"volume_cond", m.insert.volume_cond},
                           {"volume", m.insert.volume},
                           {"min_volume", m.insert.min_volume}};
        break;
    case CommandOp::CancelOrder:
        j["cancel"] = json{{"order_id", m.cancel.order_id},
                           {"exchange_order_id", m.cancel.exchange_order_id}};
        break;
    }
}
inline void from_json(const json& j, CommandBody& m) {
    m.op = j.at("op").get<CommandOp>();
    switch (m.op) {
    case CommandOp::InsertOrder: {
        m.rid                  = j.at("rid").get<OrderId>();
        const json& ins        = j.at("insert");
        m.insert.instrument_id = ins.at("instrument_id").get<InstrumentId>();
        m.insert.exchange_id   = ins.value("exchange_id", ExchangeId{});
        m.insert.direction     = ins.at("direction").get<Direction>();
        m.insert.offset        = ins.at("offset").get<Offset>();
        m.insert.price_type    = ins.at("price_type").get<PriceType>();
        m.insert.limit_price   = ins.at("limit_price").get<double>();
        m.insert.time_cond     = ins.at("time_cond").get<TimeCondition>();
        m.insert.volume_cond   = ins.at("volume_cond").get<VolumeCondition>();
        m.insert.volume        = ins.at("volume").get<uint64_t>();
        m.insert.min_volume    = ins.value("min_volume", uint64_t{0});
        break;
    }
    case CommandOp::CancelOrder: {
        const json& c              = j.at("cancel");
        m.cancel.order_id          = c.at("order_id").get<OrderId>();
        m.cancel.exchange_order_id = c.value("exchange_order_id", std::string{});
        break;
    }
    }
}

struct CommandAckBody {
    CommandAck ack; ///< 复用 trading_gateway.h 的 CommandAck
};
inline void to_json(json& j, const CommandAckBody& m) {
    j = json{
        {"success", m.ack.success}, {"order_id", m.ack.order_id}, {"error_msg", m.ack.error_msg}};
}
inline void from_json(const json& j, CommandAckBody& m) {
    m.ack.success   = j.at("success").get<bool>();
    m.ack.order_id  = j.value("order_id", OrderId{});
    m.ack.error_msg = j.value("error_msg", std::string{});
}

// ---------------------------------------------------------------------------
// 心跳 / 关闭
// ---------------------------------------------------------------------------

struct HeartbeatBody {}; ///< 空 body
inline void to_json(json& j, const HeartbeatBody&) {
    j = json::object();
}
inline void from_json(const json& j, HeartbeatBody&) {
    if (!j.is_object() && !j.is_null()) {
        throw std::invalid_argument("Heartbeat body must be object");
    }
}

struct GoodbyeBody {
    std::string reason;
};
inline void to_json(json& j, const GoodbyeBody& m) {
    j = json{{"reason", m.reason}};
}
inline void from_json(const json& j, GoodbyeBody& m) {
    m.reason = j.value("reason", std::string{});
}

// ---------------------------------------------------------------------------
// 保留帧（尚未落地）：统一用 OpaqueBody 透传 raw json
//   Snapshot / Diff / GatewayEvent / QueryOrders / QueryOrdersAck
// ---------------------------------------------------------------------------

struct OpaqueBody {
    json data = json::object();
};
inline void to_json(json& j, const OpaqueBody& m) {
    j = m.data;
}
inline void from_json(const json& j, OpaqueBody& m) {
    m.data = j;
}

// ---------------------------------------------------------------------------
// FrameType -> body 类型映射
// ---------------------------------------------------------------------------

/// 强类型 body 的 variant：覆盖当前已落地的消息类型。
using FrameBody = std::variant<HelloBody,
                               HelloAckBody,
                               SubscribeBody,
                               SubscribeAckBody,
                               MarketDataPushBody,
                               TradePushBody,
                               CommandBody,
                               CommandAckBody,
                               HeartbeatBody,
                               GoodbyeBody>;

/// 与 FrameType 一一对应的 body 类型（含保留帧用 OpaqueBody）。
template <FrameType T>
struct BodyFor;
template <>
struct BodyFor<FrameType::Hello> {
    using type = HelloBody;
};
template <>
struct BodyFor<FrameType::HelloAck> {
    using type = HelloAckBody;
};
template <>
struct BodyFor<FrameType::Subscribe> {
    using type = SubscribeBody;
};
template <>
struct BodyFor<FrameType::SubscribeAck> {
    using type = SubscribeAckBody;
};
template <>
struct BodyFor<FrameType::MarketDataPush> {
    using type = MarketDataPushBody;
};
template <>
struct BodyFor<FrameType::TradePush> {
    using type = TradePushBody;
};
template <>
struct BodyFor<FrameType::Command> {
    using type = CommandBody;
};
template <>
struct BodyFor<FrameType::CommandAck> {
    using type = CommandAckBody;
};
template <>
struct BodyFor<FrameType::Heartbeat> {
    using type = HeartbeatBody;
};
template <>
struct BodyFor<FrameType::HeartbeatAck> {
    using type = HeartbeatBody;
};
template <>
struct BodyFor<FrameType::Goodbye> {
    using type = GoodbyeBody;
};
// 保留帧（尚未落地）：统一 OpaqueBody 透传
template <>
struct BodyFor<FrameType::Snapshot> {
    using type = OpaqueBody;
};
template <>
struct BodyFor<FrameType::Diff> {
    using type = OpaqueBody;
};
template <>
struct BodyFor<FrameType::GatewayEvent> {
    using type = OpaqueBody;
};
template <>
struct BodyFor<FrameType::QueryOrders> {
    using type = OpaqueBody;
};
template <>
struct BodyFor<FrameType::QueryOrdersAck> {
    using type = OpaqueBody;
};

// ---------------------------------------------------------------------------
// 编解码帮助函数（委托给 ICodec，线上格式可替换：JSON / MessagePack ...）
// ---------------------------------------------------------------------------

/// 把 body 编码成线上字节（默认 JSON）。返回的字节可直接作为
/// IIpcChannel::async_send 的 payload（encoding 需与所用 codec 一致）。
/// body 先经 to_json 变成中性 DOM，再交给 codec 序列化，故与具体格式无关。
template <typename Body>
std::vector<uint8_t> encode_body(const Body& body, FrameEncoding encoding = kDefaultEncoding) {
    nlohmann::json dom = body;
    return codec_for(encoding).encode(dom);
}

/// 按 frame、用与 frame.encoding 一致的 codec 把 payload 还原成 DOM，再 get<Body>。
/// payload 非法 / 字段缺失 / 类型不符 均抛 IpcError。
template <typename Body>
Body decode_body(const ReceivedFrame& frame) {
    nlohmann::json dom = codec_for(frame.encoding).decode(frame.payload);
    try {
        return dom.get<Body>();
    } catch (const nlohmann::json::exception& e) {
        throw IpcError(std::string("decode_body: field mismatch: ") + e.what());
    }
}

/// 便捷：校验 frame.type 后解码成对应 FrameType 的强类型 body。
template <FrameType T>
typename BodyFor<T>::type decode_as(const ReceivedFrame& frame) {
    if (frame.type != T) {
        throw IpcError(std::string("decode_as: frame type mismatch (expected ") +
                       std::to_string(static_cast<int>(T)) + ", got " +
                       std::to_string(static_cast<int>(frame.type)) + ")");
    }
    return decode_body<typename BodyFor<T>::type>(frame);
}

/// 一步到位：给定 FrameType + body 产出 (encoding, payload) 二元组，
/// 供调用方组装 async_send。类型与 FrameType 不匹配时编译期即报错
/// （因为 encode_body 的 Body 由调用方显式给出、T 仅用于校验语义）。
template <FrameType T, typename Body>
std::pair<FrameEncoding, std::vector<uint8_t>>
encode_frame(const Body& body, FrameEncoding encoding = kDefaultEncoding) {
    static_assert(std::is_same_v<Body, typename BodyFor<T>::type>,
                  "encode_frame<T>: Body 与 FrameType 不匹配");
    return {encoding, encode_body(body, encoding)};
}

} // namespace ipc
} // namespace tqsdk

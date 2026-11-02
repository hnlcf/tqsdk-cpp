#pragma once

#include "tqsdk/pch.h"

#include "frame.h"
#include "ipc_channle.h" // IpcError

namespace tqsdk {
namespace ipc {

/// 序列化编解码抽象。
///
/// 协议 body 以 nlohmann::json 作为"中性 DOM"（struct <-> json 的映射由各 body 的
/// to_json/from_json 定义，与具体线上格式无关）。Codec 只负责
/// "DOM <-> 线上字节" 的转换，因此：
///   - 换线上格式（JSON / MessagePack / 未来其它）无需改动任何 body 或调用方；
///   - 由 frame 头里的 FrameEncoding 字节决定收发两侧使用哪个 Codec。
///
/// 当前提供 JsonCodec 与 MsgpackCodec；两者都基于 nlohmann，
/// 后者依赖其内置的 to_msgpack/from_msgpack，零新增依赖。
class ICodec {
public:
    virtual ~ICodec() = default;

    /// 该 codec 对应的帧编码标识。
    virtual FrameEncoding encoding() const noexcept = 0;

    /// DOM -> 线上字节。失败抛 IpcError。
    virtual std::vector<uint8_t> encode(const nlohmann::json& dom) const = 0;

    /// 线上字节 -> DOM。失败（非法输入）抛 IpcError。
    virtual nlohmann::json decode(std::span<const uint8_t> bytes) const = 0;
};

/// JSON 文本编码。
class JsonCodec final : public ICodec {
public:
    FrameEncoding encoding() const noexcept override { return FrameEncoding::Json; }

    std::vector<uint8_t> encode(const nlohmann::json& dom) const override {
        const std::string s = dom.dump();
        return std::vector<uint8_t>(s.begin(), s.end());
    }

    nlohmann::json decode(std::span<const uint8_t> bytes) const override {
        try {
            return nlohmann::json::parse(bytes.begin(), bytes.end());
        } catch (const nlohmann::json::exception& e) {
            throw IpcError(std::string("json decode failed: ") + e.what());
        }
    }
};

/// MessagePack 二进制编码（体积小，适合高频行情；后续性能阶段启用）。
class MsgpackCodec final : public ICodec {
public:
    FrameEncoding encoding() const noexcept override { return FrameEncoding::MessagePack; }

    std::vector<uint8_t> encode(const nlohmann::json& dom) const override {
        try {
            return nlohmann::json::to_msgpack(dom);
        } catch (const nlohmann::json::exception& e) {
            throw IpcError(std::string("msgpack encode failed: ") + e.what());
        }
    }

    nlohmann::json decode(std::span<const uint8_t> bytes) const override {
        try {
            return nlohmann::json::from_msgpack(bytes.begin(), bytes.end());
        } catch (const nlohmann::json::exception& e) {
            throw IpcError(std::string("msgpack decode failed: ") + e.what());
        }
    }
};

/// 取编码对应的 codec 单例。未知编码抛 IpcError。
inline const ICodec& codec_for(FrameEncoding enc) {
    static const JsonCodec    json_codec;
    static const MsgpackCodec msgpack_codec;
    switch (enc) {
    case FrameEncoding::Json:
        return json_codec;
    case FrameEncoding::MessagePack:
        return msgpack_codec;
    }
    throw IpcError("codec_for: unknown FrameEncoding");
}

/// 默认编码（一期用 JSON）。
inline constexpr FrameEncoding kDefaultEncoding = FrameEncoding::Json;

/// 默认 codec 单例。
inline const ICodec& default_codec() { return codec_for(kDefaultEncoding); }

} // namespace ipc
} // namespace tqsdk

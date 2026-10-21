#include "tqsdk/ipc/frame.h"

namespace tqsdk {

std::vector<uint8_t> FrameCodec::encode(const Frame& frame) {
    std::span<uint8_t> payload_span(const_cast<uint8_t*>(frame.payload), frame.length - 2);
    return encode(frame.type, frame.encoding, payload_span);
}

std::vector<uint8_t>
FrameCodec::encode(FrameType type, FrameEncoding encoding, std::span<uint8_t> payload) {
    uint32_t             length = static_cast<uint32_t>(payload.size() + 2);
    std::vector<uint8_t> buf(sizeof(uint32_t) + length);

    uint8_t* cursor = buf.data();

    // write length
    uint32_t length_be = htonl(length);
    std::memcpy(cursor, &length_be, sizeof(length_be));
    cursor += sizeof(uint32_t);

    // write type
    *cursor = static_cast<uint8_t>(type);
    cursor += sizeof(uint8_t);

    // write encoding
    *cursor = static_cast<uint8_t>(encoding);
    cursor += sizeof(uint8_t);

    // write payload
    std::memcpy(cursor, payload.data(), payload.size());
    cursor += payload.size();

    return buf;
}

} // namespace tqsdk

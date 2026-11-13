#pragma once

#include "tqsdk/pch.h"

namespace tqsdk::ipc {

/// IPC 连接/协议边界的独立错误类别。
enum class IpcErrorCode {
    no_factory,
    connect,
    handshake,
    encoding,
    closed,
    protocol,
    timeout,
    unknown,
};

struct IpcFailure {
    IpcErrorCode code = IpcErrorCode::unknown;
    std::string  detail;
};

constexpr std::string_view ipc_error_name(IpcErrorCode code) noexcept {
    switch (code) {
    case IpcErrorCode::no_factory:
        return "no_factory";
    case IpcErrorCode::connect:
        return "connect";
    case IpcErrorCode::handshake:
        return "handshake";
    case IpcErrorCode::encoding:
        return "encoding";
    case IpcErrorCode::closed:
        return "closed";
    case IpcErrorCode::protocol:
        return "protocol";
    case IpcErrorCode::timeout:
        return "timeout";
    case IpcErrorCode::unknown:
        return "unknown";
    }
    return "unknown";
}

template <typename T>
using IpcResult = std::expected<T, IpcFailure>;

} // namespace tqsdk::ipc

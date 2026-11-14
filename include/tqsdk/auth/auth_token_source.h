#pragma once

#include "tqsdk/pch.h"

namespace tqsdk {

/// InstrumentCatalog 只需要“以什么身份下载”的最小接口。
/// 进程内 gw 用 TqAuth 实现；node 用 IPC 上的 RemoteAuthService 实现。
class AuthTokenSource {
public:
    virtual ~AuthTokenSource() = default;

    virtual std::string user_agent() const     = 0;
    virtual std::string authorization_header() = 0;
};

} // namespace tqsdk

#pragma once

#include "tqsdk/pch.h"

namespace tqsdk::util {

/// 低频后台 HTTP 请求的附加 header（例如 Authorization / User-Agent）。
using HttpHeaders = std::map<std::string, std::string>;

/// 同步 HTTP(S) GET，仅用于低频后台刷新（合约目录、节假日表）。
///
/// - 支持 http:// 和 https://，TLS 使用系统 CA；
/// - 不跟随重定向；非 200 抛 std::runtime_error；
/// - 该函数会阻塞调用线程，必须放在后台刷新线程中。
std::string http_get(std::string_view          url,
                     std::string_view          user_agent = "tqsdk-node/0.1",
                     std::chrono::milliseconds timeout    = std::chrono::seconds(60),
                     const HttpHeaders&        headers    = {});

/// 同步 PUT（可带空 body）；TqAuth add-account 使用。
std::string http_put(std::string_view          url,
                     std::string_view          body = {},
                     std::string_view          user_agent = "tqsdk-node/0.1",
                     std::chrono::milliseconds timeout = std::chrono::seconds(30),
                     const HttpHeaders&        headers = {});

/// 同步 form-urlencoded POST；天勤 TqAuth 的 token endpoint 使用。
std::string http_post_form(std::string_view          url,
                           std::string_view          body,
                           std::string_view          user_agent = "tqsdk-node/0.1",
                           std::chrono::milliseconds timeout    = std::chrono::seconds(30),
                           const HttpHeaders&        headers    = {});

} // namespace tqsdk::util

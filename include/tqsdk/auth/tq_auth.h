#pragma once

#include "tqsdk/pch.h"

#include <mutex>

namespace tqsdk {

/// 快期（TqSdk）账户认证。
///
/// 官方 TqSdk 使用 OIDC password/refresh_token 流程拿取 access_token，
/// 随后在请求 openmd 合约目录、名称服务等 HTTP 接口时携带
/// `Authorization: Bearer <access_token>`。本类只实现认证与 token 生命周期，
/// 不负责交易账户授权。
class TqAuth {
public:
    struct Config {
        bool                     enabled = false;
        std::string              user_name;
        std::string              password;
        std::string              auth_url = "https://auth.shinnytech.com";
        std::string              client_id = "shinny_tq";
        std::string              client_secret = "be30b9f4-6862-488a-99ad-21bde0400081";
        std::string              user_agent = "tqsdk-cpp/0.1";
        std::chrono::seconds     request_timeout{30};
        /// token 提前刷新窗口，避免请求边界刚好过期。
        std::chrono::seconds     refresh_margin{30};
    };

    explicit TqAuth(Config config);
    ~TqAuth() = default;

    TqAuth(const TqAuth&)            = delete;
    TqAuth& operator=(const TqAuth&) = delete;

    /// 使用账号密码登录；失败返回 false，并通过 last_error() 提供原因。
    bool login();

    /// 使用 refresh_token 刷新；refresh 失败时会回退到 password 登录。
    bool refresh();

    /// 返回当前有效 access token；必要时自动登录/刷新。失败返回空串。
    std::string ensure_access_token();

    /// 生成 `Bearer <token>`；未配置或失败返回空串。
    std::string authorization_header();

    /// HTTP 下载使用的 User-Agent（官方 TqSdk 标识）。
    std::string user_agent() const { return cfg_.user_agent; }

    bool logged_in() const;
    std::string last_error() const;
    const Config& config() const { return cfg_; }

private:
    bool request_token(std::string_view grant_type,
                       std::string_view extra_key,
                       std::string_view extra_value);
    bool apply_token_response(std::string_view body, std::string_view fallback_error);
    bool token_is_fresh() const;
    void set_error(std::string message);

    mutable std::mutex mutex_;
    Config             cfg_;
    std::string        access_token_;
    std::string        refresh_token_;
    std::string        last_error_;
    std::chrono::system_clock::time_point expires_at_{};
};

/// base64url 解码（用于不验签地读取 JWT exp）；失败返回空串。
std::string base64url_decode(std::string_view input);

} // namespace tqsdk

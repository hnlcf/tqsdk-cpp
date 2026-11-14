#pragma once

#include "tqsdk/auth/auth_error.h"
#include "tqsdk/constants.h"
#include "tqsdk/pch.h"

#include <mutex>

namespace tqsdk {

/// 快期（TqSdk）账户认证。
///
/// 对齐官方 tqsdk/auth.py：
/// - OIDC password / refresh_token 获取 access_token；
/// - 用 Bearer token 请求 get-grant / update-grant-accounts / 名称服务；
/// - 解析 JWT 中的 auth_id、grants；
/// - 获取到期时间、期货公司交易网关、行情网关地址。
///
/// 该类只负责认证与地址服务，不直接建立行情/交易连接。
class TqAuth {
public:
    struct Config {
        bool                 enabled = false;
        std::string          user_name;
        std::string          password;
        std::string          auth_url          = std::string(constants::kTqAuthUrl);
        std::string          ns_url            = std::string(constants::kTqNameServiceUrl);
        std::string          broker_url_prefix = std::string(constants::kTqBrokerUrlPrefix);
        std::string          client_id         = std::string(constants::kTqClientId);
        std::string          client_secret     = std::string(constants::kTqClientSecret);
        std::string          user_agent        = std::string(constants::kTqUserAgent);
        std::string          mode = std::string(constants::kTqDefaultMode); ///< "real" / "bt"
        std::chrono::seconds request_timeout{30};
        std::chrono::seconds refresh_margin{30};
    };

    struct TdUrl {
        std::string url;
        std::string broker_type = "FUTURE";
        std::string smtype;
        std::string smconfig;
    };

    explicit TqAuth(Config config);
    ~TqAuth() = default;

    TqAuth(const TqAuth&)            = delete;
    TqAuth& operator=(const TqAuth&) = delete;

    /// 官方 TqAuth.init(mode)；只改变后续名称服务请求的模式。
    void        init(std::string mode = "real");
    std::string mode() const;

    /// 使用账号密码登录；失败返回 false，并通过 last_error() 提供原因。
    bool login();

    /// 登录的 expected 边界；不抛异常，错误码/详情由 AuthError 给出。
    auth::AuthResult<void> login_result() noexcept;

    /// 使用 refresh_token 刷新；失败返回 false。
    bool refresh();

    /// 返回当前有效 access token；必要时自动登录/刷新。失败返回空串。
    std::string ensure_access_token();

    /// 生成 `Bearer <token>`；未配置或失败返回空串。
    std::string authorization_header();

    /// 查询授权到期时间（带缓存）。
    std::optional<std::chrono::system_clock::time_point> expire_datetime();
    int                                                  expire_days_left();
    std::string                                          product_type();

    /// 把期货账户加入当前认证的 grants；成功后刷新 token。
    bool add_account(std::string_view account_id);

    /// 获取期货公司交易网关地址；对应官方 _get_td_url。
    /// broker_name 是天勤元数据键，不是可选的 req_login.broker_id。
    std::optional<TdUrl> get_td_url(std::string_view broker_name, std::string_view account_id);

    /// 获取行情网关地址；对应官方 _get_md_url。
    std::optional<std::string> get_md_url(bool stock = false, bool backtest = false);

    std::string   auth_id() const;
    std::string   grants_json() const;
    std::string   user_agent() const { return cfg_.user_agent; }
    bool          logged_in() const;
    std::string   last_error() const;
    const Config& config() const { return cfg_; }

private:
    bool        post_token(std::string_view body);
    bool        apply_token_response(std::string_view body, std::string_view fallback_error);
    void        apply_claims(std::string_view access_token);
    bool        token_is_fresh() const;
    void        set_error(std::string message);
    std::string bearer_header_unlocked() const;

    mutable std::mutex                    mutex_;
    Config                                cfg_;
    std::string                           access_token_;
    std::string                           refresh_token_;
    std::string                           auth_id_;
    std::string                           grants_json_ = "{\"features\":[],\"accounts\":[]}";
    std::string                           last_error_;
    std::chrono::system_clock::time_point expires_at_{};

    bool                                  expire_loaded_ = false;
    std::chrono::system_clock::time_point expire_at_value_{};
    int                                   expire_days_left_ = 0;
    std::string                           product_type_;
};

/// base64url 解码（用于不验签地读取 JWT claims）；失败返回空串。
std::string base64url_decode(std::string_view input);

} // namespace tqsdk

#include "tqsdk/auth/tq_auth.h"

#include "tqsdk/util/http_download.h"

#include <nlohmann/json.hpp>

#include <cctype>

namespace tqsdk {

namespace {

using json = nlohmann::json;

std::string trim_url(std::string value) {
    while (!value.empty() && value.back() == '/') {
        value.pop_back();
    }
    return value;
}

std::string url_encode(std::string_view value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string           out;
    out.reserve(value.size() * 3);
    for (const unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0f]);
        }
    }
    return out;
}

std::optional<json> jwt_claims(std::string_view token) {
    const auto first = token.find('.');
    if (first == std::string_view::npos) {
        return std::nullopt;
    }
    const auto second = token.find('.', first + 1);
    if (second == std::string_view::npos) {
        return std::nullopt;
    }
    const auto payload = base64url_decode(token.substr(first + 1, second - first - 1));
    if (payload.empty()) {
        return std::nullopt;
    }
    try {
        return json::parse(payload);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::int64_t> jwt_exp(std::string_view token) {
    const auto claims = jwt_claims(token);
    if (!claims || !claims->contains("exp") || !(*claims)["exp"].is_number_integer()) {
        return std::nullopt;
    }
    return (*claims)["exp"].get<std::int64_t>();
}

std::string string_or(const json& object, std::string_view key, std::string fallback = {}) {
    const auto actual_key = std::string{key};
    if (object.contains(actual_key) && object[actual_key].is_string()) {
        return object[actual_key].get<std::string>();
    }
    return fallback;
}

bool category_has_tq(const json& category) {
    if (category.is_array()) {
        for (const auto& item : category) {
            if (item.is_string() && item.get<std::string>().find("TQ") != std::string::npos) {
                return true;
            }
        }
    } else if (category.is_string()) {
        return category.get<std::string>().find("TQ") != std::string::npos;
    }
    return false;
}

} // namespace

std::string base64url_decode(std::string_view input) {
    std::string normalized;
    normalized.reserve(input.size() + 4);
    for (char c : input) {
        if (c == '-') {
            normalized.push_back('+');
        } else if (c == '_') {
            normalized.push_back('/');
        } else {
            normalized.push_back(c);
        }
    }
    while (normalized.size() % 4 != 0) {
        normalized.push_back('=');
    }
    static constexpr int8_t table[256] = {
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62,
        -1, -1, -1, 63, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, -1, -1, -1, -1, 0,
        1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22,
        23, 24, 25, -1, -1, -1, -1, -1, -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38,
        39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    };
    std::string out;
    out.reserve(normalized.size() / 4 * 3);
    uint32_t value = 0;
    int      bits  = 0;
    for (unsigned char c : normalized) {
        if (c == '=') {
            break;
        }
        const int decoded = table[c];
        if (decoded < 0) {
            return {};
        }
        value = (value << 6) | static_cast<uint32_t>(decoded);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((value >> bits) & 0xff));
        }
    }
    return out;
}

TqAuth::TqAuth(Config config)
    : cfg_(std::move(config)) {
    if (cfg_.auth_url.empty()) {
        cfg_.auth_url = std::string(constants::kTqAuthUrl);
    }
    if (cfg_.ns_url.empty()) {
        cfg_.ns_url = std::string(constants::kTqNameServiceUrl);
    }
    if (cfg_.broker_url_prefix.empty()) {
        cfg_.broker_url_prefix = std::string(constants::kTqBrokerUrlPrefix);
    }
    cfg_.auth_url          = trim_url(cfg_.auth_url);
    cfg_.ns_url            = trim_url(cfg_.ns_url);
    cfg_.broker_url_prefix = trim_url(cfg_.broker_url_prefix);
    if (cfg_.mode != "bt") {
        cfg_.mode = "real";
    }
}

void TqAuth::init(std::string mode) {
    std::lock_guard lock(mutex_);
    cfg_.mode = mode == "bt" ? "bt" : "real";
}

std::string TqAuth::mode() const {
    std::lock_guard lock(mutex_);
    return cfg_.mode;
}

void TqAuth::set_error(std::string message) {
    SPDLOG_WARN("[TqAuth] {}", message);
    last_error_ = std::move(message);
}

bool TqAuth::token_is_fresh() const {
    if (access_token_.empty()) {
        return false;
    }
    return std::chrono::system_clock::now() + cfg_.refresh_margin < expires_at_;
}

std::string TqAuth::bearer_header_unlocked() const {
    return access_token_.empty() ? std::string{} : fmt::format("Bearer {}", access_token_);
}

void TqAuth::apply_claims(std::string_view access_token) {
    const auto claims = jwt_claims(access_token);
    if (!claims) {
        return;
    }
    auth_id_ = string_or(*claims, "sub", auth_id_);
    if (claims->contains("grants") && (*claims)["grants"].is_object()) {
        grants_json_ = (*claims)["grants"].dump();
    }
}

bool TqAuth::apply_token_response(std::string_view body, std::string_view fallback_error) {
    try {
        const auto doc    = json::parse(body);
        const auto access = doc.value("access_token", std::string{});
        if (access.empty()) {
            set_error(fmt::format("{}: no access_token", fallback_error));
            return false;
        }
        access_token_  = access;
        refresh_token_ = doc.value("refresh_token", refresh_token_);
        expire_loaded_ = false;
        const auto now = std::chrono::system_clock::now();
        if (const auto exp = jwt_exp(access_token_); exp.has_value()) {
            expires_at_ = std::chrono::system_clock::from_time_t(static_cast<std::time_t>(*exp));
        } else if (doc.contains("expires_in") && doc["expires_in"].is_number_integer()) {
            expires_at_ = now + std::chrono::seconds(doc["expires_in"].get<std::int64_t>());
        } else {
            expires_at_ = now + std::chrono::seconds(300);
        }
        apply_claims(access_token_);
        last_error_.clear();
        return true;
    } catch (const std::exception& e) {
        set_error(fmt::format("{}: {}", fallback_error, e.what()));
        return false;
    }
}

bool TqAuth::post_token(std::string_view body) {
    const std::string url =
        fmt::format("{}/auth/realms/shinnytech/protocol/openid-connect/token", cfg_.auth_url);
    const auto response_result = util::http_post_form(
        url,
        body,
        cfg_.user_agent,
        std::chrono::duration_cast<std::chrono::milliseconds>(cfg_.request_timeout));
    if (!response_result) {
        set_error(fmt::format("TqAuth token request failed: {} ({})",
                              response_result.error().detail,
                              util::http_error_name(response_result.error().code)));
        return false;
    }
    return apply_token_response(*response_result, "TqAuth token request failed");
}

bool TqAuth::login() {
    if (!cfg_.enabled) {
        std::lock_guard lock(mutex_);
        set_error("TqAuth is disabled");
        return false;
    }
    if (cfg_.user_name.empty() || cfg_.password.empty()) {
        std::lock_guard lock(mutex_);
        set_error("TqAuth user_name or password is empty");
        return false;
    }
    std::lock_guard lock(mutex_);
    SPDLOG_INFO("[TqAuth] login user={}", cfg_.user_name);
    const std::string body =
        fmt::format("grant_type=password&username={}&password={}&client_id={}&client_secret={}",
                    url_encode(cfg_.user_name),
                    url_encode(cfg_.password),
                    url_encode(cfg_.client_id),
                    url_encode(cfg_.client_secret));
    const bool applied = post_token(body);
    if (!applied) {
        SPDLOG_WARN("[TqAuth] login failed: {}", last_error_);
    }
    return applied;
}

bool TqAuth::refresh() {
    std::lock_guard lock(mutex_);
    if (!cfg_.enabled) {
        set_error("TqAuth is disabled");
        return false;
    }
    if (refresh_token_.empty()) {
        set_error("TqAuth refresh_token is empty");
        return false;
    }
    SPDLOG_DEBUG("[TqAuth] refreshing access token");
    const std::string body =
        fmt::format("grant_type=refresh_token&refresh_token={}&client_id={}&client_secret={}",
                    url_encode(refresh_token_),
                    url_encode(cfg_.client_id),
                    url_encode(cfg_.client_secret));
    return post_token(body);
}

std::string TqAuth::ensure_access_token() {
    std::string refresh_token;
    {
        std::lock_guard lock(mutex_);
        if (token_is_fresh()) {
            return access_token_;
        }
        refresh_token = refresh_token_;
    }
    if (!refresh_token.empty() && refresh()) {
        std::lock_guard lock(mutex_);
        return access_token_;
    }
    if (login()) {
        std::lock_guard lock(mutex_);
        return access_token_;
    }
    return {};
}

std::string TqAuth::authorization_header() {
    if (ensure_access_token().empty()) {
        return {};
    }
    std::lock_guard lock(mutex_);
    return bearer_header_unlocked();
}

std::optional<std::chrono::system_clock::time_point> TqAuth::expire_datetime() {
    {
        std::lock_guard lock(mutex_);
        if (expire_loaded_) {
            return expire_at_value_;
        }
    }
    const auto authorization = authorization_header();
    if (authorization.empty()) {
        return std::nullopt;
    }
    try {
        const std::string url =
            fmt::format("{}/auth/realms/shinnytech/rest/get-grant/tq", cfg_.auth_url);
        const auto body_result = util::http_get(
            url,
            cfg_.user_agent,
            std::chrono::duration_cast<std::chrono::milliseconds>(cfg_.request_timeout),
            {{"Authorization", authorization}});
        if (!body_result) {
            std::lock_guard lock(mutex_);
            set_error(fmt::format("TqAuth expire request failed: {} ({})",
                                  body_result.error().detail,
                                  util::http_error_name(body_result.error().code)));
            return std::nullopt;
        }
        const auto doc = json::parse(*body_result);
        if (!doc.contains("exp") || !doc["exp"].is_number_integer()) {
            std::lock_guard lock(mutex_);
            set_error("TqAuth expire response has no exp");
            return std::nullopt;
        }
        const auto value = std::chrono::system_clock::from_time_t(
            static_cast<std::time_t>(doc["exp"].get<std::int64_t>()));
        std::lock_guard lock(mutex_);
        expire_loaded_    = true;
        expire_at_value_  = value;
        expire_days_left_ = doc.value("daysleft", 0);
        product_type_     = doc.value("account_type", std::string{});
        last_error_.clear();
        return value;
    } catch (const std::exception& e) {
        std::lock_guard lock(mutex_);
        set_error(std::string("TqAuth expire request failed: ") + e.what());
        return std::nullopt;
    }
}

int TqAuth::expire_days_left() {
    (void)expire_datetime();
    std::lock_guard lock(mutex_);
    return expire_days_left_;
}

std::string TqAuth::product_type() {
    (void)expire_datetime();
    std::lock_guard lock(mutex_);
    return product_type_;
}

bool TqAuth::add_account(std::string_view account_id) {
    std::lock_guard lock(mutex_);
    if (account_id.empty()) {
        set_error("TqAuth account_id is empty");
        return false;
    }
    try {
        const auto grants = json::parse(grants_json_);
        if (grants.contains("accounts") && grants["accounts"].is_array()) {
            for (const auto& account : grants["accounts"]) {
                if (account.is_string() && account.get<std::string>() == std::string{account_id}) {
                    return true;
                }
            }
        }
    } catch (...) {
        // 忽略损坏的缓存 claims，继续让服务端更新。
    }
    const auto authorization = bearer_header_unlocked();
    if (authorization.empty()) {
        set_error("TqAuth is not logged in");
        return false;
    }
    try {
        const std::string url =
            fmt::format("{}/auth/realms/shinnytech/rest/update-grant-accounts/{}",
                        cfg_.auth_url,
                        url_encode(account_id));
        const auto put_result = util::http_put(
            url,
            {},
            cfg_.user_agent,
            std::chrono::duration_cast<std::chrono::milliseconds>(cfg_.request_timeout),
            {{"Authorization", authorization}});
        if (!put_result) {
            set_error(fmt::format("TqAuth add account failed: {} ({})",
                                  put_result.error().detail,
                                  util::http_error_name(put_result.error().code)));
            return false;
        }
    } catch (const std::exception& e) {
        set_error(std::string("TqAuth add account failed: ") + e.what());
        return false;
    }
    if (refresh_token_.empty()) {
        set_error("TqAuth add account succeeded but refresh_token is empty");
        return false;
    }
    const std::string body =
        fmt::format("grant_type=refresh_token&refresh_token={}&client_id={}&client_secret={}",
                    url_encode(refresh_token_),
                    url_encode(cfg_.client_id),
                    url_encode(cfg_.client_secret));
    return post_token(body);
}

std::optional<TqAuth::TdUrl> TqAuth::get_td_url(std::string_view broker_id,
                                                std::string_view account_id) {
    const auto authorization = authorization_header();
    if (authorization.empty()) {
        return std::nullopt;
    }
    try {
        const std::string url         = fmt::format("{}/{}.json?account_id={}&auth={}",
                                                    cfg_.broker_url_prefix,
                                                    url_encode(broker_id),
                                                    url_encode(account_id),
                                                    url_encode(cfg_.user_name));
        const auto        body_result = util::http_get(
            url,
            cfg_.user_agent,
            std::chrono::duration_cast<std::chrono::milliseconds>(cfg_.request_timeout),
            {{"Authorization", authorization}});
        if (!body_result) {
            std::lock_guard lock(mutex_);
            set_error(fmt::format("TqAuth td url request failed: {} ({})",
                                  body_result.error().detail,
                                  util::http_error_name(body_result.error().code)));
            return std::nullopt;
        }
        const auto doc        = json::parse(*body_result);
        const auto broker_key = std::string{broker_id};
        if (!doc.contains(broker_key) || !doc[broker_key].is_object()) {
            std::lock_guard lock(mutex_);
            set_error(fmt::format("TqAuth unsupported broker: {}", broker_key));
            return std::nullopt;
        }
        const auto& broker = doc[broker_key];
        if (!broker.contains("category") || !category_has_tq(broker["category"])) {
            std::lock_guard lock(mutex_);
            set_error(fmt::format("TqAuth broker does not support TQ: {}", broker_key));
            return std::nullopt;
        }
        TdUrl result;
        result.url         = string_or(broker, "url");
        result.broker_type = string_or(broker, "broker_type", "FUTURE");
        result.smtype      = string_or(broker, "smtype");
        result.smconfig    = string_or(broker, "smconfig");
        if (result.url.empty()) {
            std::lock_guard lock(mutex_);
            set_error("TqAuth broker response has no url");
            return std::nullopt;
        }
        return result;
    } catch (const std::exception& e) {
        std::lock_guard lock(mutex_);
        set_error(std::string("TqAuth td url request failed: ") + e.what());
        return std::nullopt;
    }
}

std::optional<std::string> TqAuth::get_md_url(bool stock, bool backtest) {
    const auto authorization = authorization_header();
    if (authorization.empty()) {
        return std::nullopt;
    }
    try {
        const std::string url         = fmt::format("{}?stock={}&backtest={}",
                                                    cfg_.ns_url,
                                                    stock ? "true" : "false",
                                                    backtest ? "true" : "false");
        const auto        body_result = util::http_get(
            url,
            cfg_.user_agent,
            std::chrono::duration_cast<std::chrono::milliseconds>(cfg_.request_timeout),
            {{"Authorization", authorization}});
        if (!body_result) {
            std::lock_guard lock(mutex_);
            set_error(fmt::format("TqAuth md url request failed: {} ({})",
                                  body_result.error().detail,
                                  util::http_error_name(body_result.error().code)));
            return std::nullopt;
        }
        const auto doc   = json::parse(*body_result);
        const auto mdurl = string_or(doc, "mdurl");
        if (mdurl.empty()) {
            std::lock_guard lock(mutex_);
            set_error("TqAuth name service returned no mdurl");
            return std::nullopt;
        }
        return mdurl;
    } catch (const std::exception& e) {
        std::lock_guard lock(mutex_);
        set_error(std::string("TqAuth md url request failed: ") + e.what());
        return std::nullopt;
    }
}

std::string TqAuth::auth_id() const {
    std::lock_guard lock(mutex_);
    return auth_id_;
}

std::string TqAuth::grants_json() const {
    std::lock_guard lock(mutex_);
    return grants_json_;
}

bool TqAuth::logged_in() const {
    std::lock_guard lock(mutex_);
    return !access_token_.empty();
}

std::string TqAuth::last_error() const {
    std::lock_guard lock(mutex_);
    return last_error_;
}

} // namespace tqsdk

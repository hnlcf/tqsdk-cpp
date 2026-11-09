#include "tqsdk/auth/tq_auth.h"

#include "tqsdk/util/http_download.h"

#include <nlohmann/json.hpp>

#include <cctype>
#include <charconv>

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
    std::string out;
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

std::optional<std::int64_t> jwt_exp(std::string_view token) {
    const auto first = token.find('.');
    if (first == std::string_view::npos) return std::nullopt;
    const auto second = token.find('.', first + 1);
    if (second == std::string_view::npos) return std::nullopt;
    const auto payload = base64url_decode(token.substr(first + 1, second - first - 1));
    if (payload.empty()) return std::nullopt;
    try {
        const auto doc = json::parse(payload);
        if (doc.contains("exp") && doc["exp"].is_number_integer()) {
            return doc["exp"].get<std::int64_t>();
        }
    } catch (...) {
        // token 可能不是 JWT；回退到 expires_in。
    }
    return std::nullopt;
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
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62, -1, -1, -1, 63,
        52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, -1, -1, -1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
        15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, -1,
        -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
        41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    };
    std::string out;
    out.reserve(normalized.size() / 4 * 3);
    uint32_t value = 0;
    int bits = 0;
    for (unsigned char c : normalized) {
        if (c == '=') break;
        const int decoded = table[c];
        if (decoded < 0) return {};
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
    if (cfg_.auth_url.empty()) cfg_.auth_url = "https://auth.shinnytech.com";
    cfg_.auth_url = trim_url(cfg_.auth_url);
}

void TqAuth::set_error(std::string message) {
    last_error_ = std::move(message);
}

bool TqAuth::token_is_fresh() const {
    if (access_token_.empty()) return false;
    return std::chrono::system_clock::now() + cfg_.refresh_margin < expires_at_;
}

bool TqAuth::apply_token_response(std::string_view body, std::string_view fallback_error) {
    try {
        const auto doc = json::parse(body);
        const auto access = doc.value("access_token", std::string{});
        if (access.empty()) {
            set_error(std::string{fallback_error} + ": no access_token");
            return false;
        }
        access_token_ = access;
        refresh_token_ = doc.value("refresh_token", refresh_token_);
        const auto now = std::chrono::system_clock::now();
        if (const auto exp = jwt_exp(access_token_); exp.has_value()) {
            expires_at_ = std::chrono::system_clock::from_time_t(
                static_cast<std::time_t>(*exp));
        } else if (doc.contains("expires_in") && doc["expires_in"].is_number_integer()) {
            expires_at_ = now + std::chrono::seconds(doc["expires_in"].get<std::int64_t>());
        } else {
            expires_at_ = now + std::chrono::seconds(300);
        }
        last_error_.clear();
        return true;
    } catch (const std::exception& e) {
        set_error(std::string{fallback_error} + ": " + e.what());
        return false;
    }
}

bool TqAuth::request_token(std::string_view grant_type,
                           std::string_view extra_key,
                           std::string_view extra_value) {
    const std::string url = cfg_.auth_url +
                            "/auth/realms/shinnytech/protocol/openid-connect/token";
    std::string body = "grant_type=" + url_encode(grant_type) +
                       "&client_id=" + url_encode(cfg_.client_id) +
                       "&client_secret=" + url_encode(cfg_.client_secret);
    if (!extra_key.empty()) {
        body += "&" + url_encode(extra_key) + "=" + url_encode(extra_value);
    }
    try {
        const std::string response = util::http_post_form(
            url, body, cfg_.user_agent, std::chrono::duration_cast<std::chrono::milliseconds>(cfg_.request_timeout));
        return apply_token_response(response, "TqAuth token request failed");
    } catch (const std::exception& e) {
        set_error(std::string("TqAuth token request failed: ") + e.what());
        return false;
    }
}

bool TqAuth::login() {
    if (!cfg_.enabled) {
        set_error("TqAuth is disabled");
        return false;
    }
    if (cfg_.user_name.empty() || cfg_.password.empty()) {
        set_error("TqAuth user_name or password is empty");
        return false;
    }
    std::lock_guard lock(mutex_);
    SPDLOG_INFO("[TqAuth] login user={}", cfg_.user_name);
    const std::string url = cfg_.auth_url +
                            "/auth/realms/shinnytech/protocol/openid-connect/token";
    const std::string body = "grant_type=password&username=" + url_encode(cfg_.user_name) +
                             "&password=" + url_encode(cfg_.password) +
                             "&client_id=" + url_encode(cfg_.client_id) +
                             "&client_secret=" + url_encode(cfg_.client_secret);
    try {
        const std::string response =
            util::http_post_form(url, body, cfg_.user_agent, std::chrono::duration_cast<std::chrono::milliseconds>(cfg_.request_timeout));
        const bool applied = apply_token_response(response, "TqAuth login failed");
        if (!applied) {
            SPDLOG_WARN("[TqAuth] login failed: {}", last_error_);
        }
        return applied;
    } catch (const std::exception& e) {
        set_error(std::string("TqAuth login failed: ") + e.what());
        SPDLOG_WARN("[TqAuth] login failed: {}", last_error_);
        return false;
    }
}

bool TqAuth::refresh() {
    if (!cfg_.enabled) {
        set_error("TqAuth is disabled");
        return false;
    }
    std::lock_guard lock(mutex_);
    if (refresh_token_.empty()) {
        set_error("TqAuth refresh_token is empty");
        return false;
    }
    SPDLOG_DEBUG("[TqAuth] refreshing access token");
    return request_token("refresh_token", "refresh_token", refresh_token_);
}

std::string TqAuth::ensure_access_token() {
    std::string refresh_token;
    {
        std::lock_guard lock(mutex_);
        if (token_is_fresh()) return access_token_;
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
    const auto token = ensure_access_token();
    return token.empty() ? std::string{} : "Bearer " + token;
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

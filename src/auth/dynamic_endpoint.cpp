#include "tqsdk/auth/dynamic_endpoint.h"

#include "tqsdk/auth/tq_auth.h"

#include <charconv>

namespace tqsdk {

namespace {

std::optional<std::uint16_t> parse_port(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    unsigned value = 0;
    auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || ptr != text.data() + text.size() || value > 65535) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(value);
}

} // namespace

std::optional<Endpoint> Endpoint::parse(std::string_view url) {
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string_view::npos || scheme_end == 0) {
        return std::nullopt;
    }
    Endpoint endpoint;
    endpoint.scheme            = std::string{url.substr(0, scheme_end)};
    const auto authority_begin = scheme_end + 3;
    const auto path_begin      = url.find('/', authority_begin);
    const auto authority =
        url.substr(authority_begin,
                   path_begin == std::string_view::npos ? std::string_view::npos
                                                        : path_begin - authority_begin);
    if (authority.empty()) {
        return std::nullopt;
    }

    const auto colon = authority.rfind(':');
    if (colon == std::string_view::npos) {
        endpoint.host = std::string{authority};
        endpoint.port = endpoint.scheme == "ws" ? 80 : 443;
    } else {
        endpoint.host     = std::string{authority.substr(0, colon)};
        const auto parsed = parse_port(authority.substr(colon + 1));
        if (!parsed) {
            return std::nullopt;
        }
        endpoint.port = *parsed;
    }
    if (endpoint.host.empty() || endpoint.port == 0) {
        return std::nullopt;
    }
    endpoint.path = path_begin == std::string_view::npos ? std::string{"/"}
                                                         : std::string{url.substr(path_begin)};
    return endpoint;
}

std::string Endpoint::to_string() const {
    return fmt::format("{}://{}:{}{}", scheme, host, port, path);
}

std::optional<Endpoint> DynamicEndpointResolver::md(bool stock, bool backtest) {
    const auto url = auth_.get_md_url(stock, backtest);
    if (!url.has_value() || url->empty()) {
        return std::nullopt;
    }
    return Endpoint::parse(*url);
}

std::optional<std::string> DynamicEndpointResolver::td(std::string_view broker_name,
                                                       std::string_view account_id) {
    auto url = auth_.get_td_url(broker_name, account_id);
    if (!url.has_value() || url->url.empty()) {
        return std::nullopt;
    }
    return url->url;
}

} // namespace tqsdk

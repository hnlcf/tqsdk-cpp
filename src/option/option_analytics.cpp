#include "tqsdk/option/option_analytics.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace tqsdk {

namespace {

constexpr double kSqrt2 = 1.4142135623730950488;

double normal_cdf(double x) {
    return 0.5 * std::erfc(-x / kSqrt2);
}

double normal_pdf(double x) {
    return std::exp(-0.5 * x * x) / std::sqrt(2.0 * std::numbers::pi);
}

double intrinsic(const OptionSpec& spec) {
    return spec.right == OptionRight::CALL ? std::max(spec.spot - spec.strike, 0.0)
                                           : std::max(spec.strike - spec.spot, 0.0);
}

} // namespace

OptionValuation black_scholes(const OptionSpec& spec) {
    const double S     = std::max(spec.spot, 0.0);
    const double K     = std::max(spec.strike, 0.0);
    const double T     = std::max(spec.time_to_expiry, 0.0);
    const double r     = spec.risk_free_rate;
    const double q     = spec.dividend_yield;
    const double sigma = std::max(spec.volatility, 0.0);

    OptionValuation out;
    out.intrinsic = intrinsic(spec);

    if (T <= 0.0 || sigma <= 0.0) {
        // 到期或零波动：价格为贴现后的远期内在价值。
        const double forward = S * std::exp((r - q) * T);
        const double df      = std::exp(-r * T);
        out.price            = spec.right == OptionRight::CALL ? std::max(forward - K, 0.0) * df
                                                               : std::max(K - forward, 0.0) * df;
        out.time_value       = std::max(out.price - out.intrinsic, 0.0);
        return out;
    }

    const double sqrt_t = std::sqrt(T);
    const double d1     = (std::log(S / K) + (r - q + 0.5 * sigma * sigma) * T) / (sigma * sqrt_t);
    const double d2     = d1 - sigma * sqrt_t;
    const double nd1    = normal_cdf(d1);
    const double nd2    = normal_cdf(d2);
    const double pdf    = normal_pdf(d1);
    const double discount_q = std::exp(-q * T);
    const double discount_r = std::exp(-r * T);

    if (spec.right == OptionRight::CALL) {
        out.price        = S * discount_q * nd1 - K * discount_r * nd2;
        out.greeks.delta = discount_q * nd1;
        out.greeks.rho   = K * T * discount_r * nd2;
        out.greeks.theta = -S * discount_q * pdf * sigma / (2.0 * sqrt_t) -
                           r * K * discount_r * nd2 + q * S * discount_q * nd1;
    } else {
        out.price        = K * discount_r * normal_cdf(-d2) - S * discount_q * normal_cdf(-d1);
        out.greeks.delta = -discount_q * normal_cdf(-d1);
        out.greeks.rho   = -K * T * discount_r * normal_cdf(-d2);
        out.greeks.theta = -S * discount_q * pdf * sigma / (2.0 * sqrt_t) +
                           r * K * discount_r * normal_cdf(-d2) -
                           q * S * discount_q * normal_cdf(-d1);
    }
    out.greeks.gamma = discount_q * pdf / (S * sigma * sqrt_t);
    out.greeks.vega  = S * discount_q * pdf * sqrt_t;
    out.time_value   = std::max(out.price - out.intrinsic, 0.0);
    return out;
}

std::optional<double>
implied_volatility(double market_price, OptionSpec spec, double tolerance, int max_iterations) {
    if (!std::isfinite(market_price) || market_price <= 0.0 || spec.time_to_expiry <= 0.0) {
        return std::nullopt;
    }
    const auto intrinsic_value = intrinsic(spec);
    if (market_price < intrinsic_value - 1e-9) {
        return std::nullopt;
    }

    auto price_at = [&](double vol) {
        spec.volatility = vol;
        return black_scholes(spec).price;
    };

    double low  = 1e-6;
    double high = 5.0;
    if (price_at(low) > market_price || price_at(high) < market_price) {
        return std::nullopt;
    }
    for (int i = 0; i < max_iterations; ++i) {
        const double mid = 0.5 * (low + high);
        const double px  = price_at(mid);
        if (std::abs(px - market_price) <= tolerance) {
            return mid;
        }
        if (px < market_price) {
            low = mid;
        } else {
            high = mid;
        }
    }
    return 0.5 * (low + high);
}

std::optional<double> atm_strike(double spot, const std::vector<double>& strikes) {
    if (strikes.empty() || !std::isfinite(spot)) {
        return std::nullopt;
    }
    auto best = strikes.begin();
    for (auto it = strikes.begin() + 1; it != strikes.end(); ++it) {
        if (std::abs(*it - spot) < std::abs(*best - spot)) {
            best = it;
        }
    }
    return *best;
}

std::vector<OptionAnalyticsRow>
compute_option_chain(double                                         spot,
                     const std::vector<double>&                     strikes,
                     double                                         time_to_expiry,
                     double                                         risk_free_rate,
                     double                                         dividend_yield,
                     double                                         volatility,
                     const std::unordered_map<std::string, double>& market_prices,
                     std::string_view                               underlying,
                     int                                            atm_window) {
    std::vector<OptionAnalyticsRow> out;
    if (strikes.empty()) {
        return out;
    }

    std::vector<double> sorted = strikes;
    std::sort(sorted.begin(), sorted.end());
    const auto atm = atm_strike(spot, sorted);
    if (!atm.has_value()) {
        return out;
    }

    auto      center = std::lower_bound(sorted.begin(), sorted.end(), *atm);
    const int idx    = static_cast<int>(center - sorted.begin());
    const int first  = std::max(0, idx - std::max(0, atm_window));
    const int last   = std::min(static_cast<int>(sorted.size()) - 1, idx + std::max(0, atm_window));

    for (int i = first; i <= last; ++i) {
        const double strike = sorted[static_cast<std::size_t>(i)];
        for (OptionRight right : {OptionRight::CALL, OptionRight::PUT}) {
            OptionAnalyticsRow row;
            row.right          = right;
            row.strike         = strike;
            row.time_to_expiry = time_to_expiry;
            row.instrument_id = std::string(underlying) + (right == OptionRight::CALL ? "C" : "P") +
                                std::to_string(static_cast<std::int64_t>(std::llround(strike)));
            OptionSpec spec;
            spec.spot           = spot;
            spec.strike         = strike;
            spec.time_to_expiry = time_to_expiry;
            spec.risk_free_rate = risk_free_rate;
            spec.dividend_yield = dividend_yield;
            spec.volatility     = volatility;
            spec.right          = right;
            row.valuation       = black_scholes(spec);

            if (auto it = market_prices.find(row.instrument_id); it != market_prices.end()) {
                row.market_price     = it->second;
                row.has_market_price = std::isfinite(it->second) && it->second > 0;
                if (row.has_market_price) {
                    row.implied_volatility = implied_volatility(row.market_price, spec);
                }
            }
            out.push_back(std::move(row));
        }
    }
    return out;
}

} // namespace tqsdk

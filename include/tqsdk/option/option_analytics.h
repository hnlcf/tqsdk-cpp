#pragma once

#include "tqsdk/pch.h"

namespace tqsdk {

enum class OptionRight {
    CALL,
    PUT
};

struct OptionGreeks {
    double delta = 0.0;
    double gamma = 0.0;
    /// 波动率变化 1.0（100%）的期权价格变化；乘 0.01 即 1 个波动率点。
    double vega = 0.0;
    /// 每年时间衰减。
    double theta = 0.0;
    double rho   = 0.0;
};

struct OptionValuation {
    double       price      = 0.0;
    double       intrinsic  = 0.0;
    double       time_value = 0.0;
    OptionGreeks greeks;
};

struct OptionSpec {
    double      spot           = 0.0;
    double      strike         = 0.0;
    double      time_to_expiry = 1.0; ///< 年
    double      risk_free_rate = 0.0;
    double      dividend_yield = 0.0;
    double      volatility     = 0.2;
    OptionRight right          = OptionRight::CALL;
};

/// Black-Scholes 欧式期权定价与 Greeks。
OptionValuation black_scholes(const OptionSpec& spec);

/// 根据市场价格反解隐含波动率；无解返回空。
std::optional<double> implied_volatility(double     market_price,
                                         OptionSpec spec,
                                         double     tolerance      = 1e-8,
                                         int        max_iterations = 100);

/// 在 strike 列表中选择离现货最近的执行价；空列表返回空。
std::optional<double> atm_strike(double spot, const std::vector<double>& strikes);

struct OptionAnalyticsRow {
    std::string           instrument_id;
    OptionRight           right            = OptionRight::CALL;
    double                strike           = 0.0;
    double                time_to_expiry   = 0.0;
    double                market_price     = 0.0;
    bool                  has_market_price = false;
    OptionValuation       valuation;
    std::optional<double> implied_volatility;
};

/// 对一组行权价计算 ATM 附近的看涨/看跌期权估值。
///
/// `market_prices` 以 instrument_id 为键；缺失时仍返回理论价，但
/// `has_market_price=false` 且不计算 IV。
std::vector<OptionAnalyticsRow>
compute_option_chain(double                                         spot,
                     const std::vector<double>&                     strikes,
                     double                                         time_to_expiry,
                     double                                         risk_free_rate,
                     double                                         dividend_yield,
                     double                                         volatility,
                     const std::unordered_map<std::string, double>& market_prices = {},
                     std::string_view                               underlying    = {},
                     int                                            atm_window    = 5);

} // namespace tqsdk

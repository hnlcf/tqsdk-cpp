#include "tqsdk/trading/risk_manager.h"

#include <ctime>

namespace tqsdk {

namespace {

std::string china_day_key() {
    const auto now = std::chrono::system_clock::now();
    const auto seconds =
        std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count() + 8 * 3600;
    const std::time_t t = static_cast<std::time_t>(seconds);
    std::tm           tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[16]{};
    std::strftime(buf, sizeof(buf), "%Y%m%d", &tm);
    return buf;
}

bool is_open(const InsertOrderReq& req) {
    return req.offset == Offset::OPEN;
}

} // namespace

OpenCountsLimitRule::OpenCountsLimitRule(std::uint64_t limit, std::vector<std::string> symbols)
    : limit_(limit),
      symbols_(symbols.begin(), symbols.end()) {}

std::optional<std::string> OpenCountsLimitRule::check_insert(const InsertOrderReq& req) {
    if (!is_open(req) || !symbols_.contains(req.instrument_id)) {
        return std::nullopt;
    }
    const auto count = counts_[req.instrument_id];
    if (count >= limit_) {
        return "触发风控：合约 " + req.instrument_id + " 交易日内开仓次数上限 " +
               std::to_string(limit_);
    }
    return std::nullopt;
}

void OpenCountsLimitRule::on_insert(const InsertOrderReq& req) {
    if (is_open(req) && symbols_.contains(req.instrument_id)) {
        ++counts_[req.instrument_id];
    }
}

void OpenCountsLimitRule::on_new_trading_day() {
    counts_.clear();
}

OpenVolumesLimitRule::OpenVolumesLimitRule(std::uint64_t limit, std::vector<std::string> symbols)
    : limit_(limit),
      symbols_(symbols.begin(), symbols.end()) {}

std::optional<std::string> OpenVolumesLimitRule::check_insert(const InsertOrderReq& req) {
    if (!is_open(req) || !symbols_.contains(req.instrument_id)) {
        return std::nullopt;
    }
    const auto current = volumes_[req.instrument_id];
    if (current + req.volume > limit_) {
        return "触发风控：合约 " + req.instrument_id + " 交易日内开仓手数上限 " +
               std::to_string(limit_);
    }
    return std::nullopt;
}

void OpenVolumesLimitRule::on_insert(const InsertOrderReq& req) {
    if (is_open(req) && symbols_.contains(req.instrument_id)) {
        volumes_[req.instrument_id] += req.volume;
    }
}

void OpenVolumesLimitRule::on_new_trading_day() {
    volumes_.clear();
}

OrderRateLimitRule::OrderRateLimitRule(std::uint64_t limit, std::chrono::milliseconds window)
    : limit_(limit),
      window_(window) {}

std::optional<std::string> OrderRateLimitRule::check_insert(const InsertOrderReq&) {
    const auto cutoff = std::chrono::steady_clock::now() - window_;
    while (!accepted_.empty() && accepted_.front() < cutoff) {
        accepted_.pop_front();
    }
    if (accepted_.size() >= limit_) {
        return "触发风控：报单频率超过 " + std::to_string(limit_) + " 笔/" +
               std::to_string(window_.count()) + "ms";
    }
    return std::nullopt;
}

void OrderRateLimitRule::on_insert(const InsertOrderReq&) {
    accepted_.push_back(std::chrono::steady_clock::now());
}

void RiskManager::add(std::shared_ptr<RiskRule> rule) {
    if (!rule) {
        return;
    }
    std::lock_guard lock(mutex_);
    if (std::find(rules_.begin(), rules_.end(), rule) == rules_.end()) {
        rules_.push_back(std::move(rule));
    }
}

void RiskManager::clear() {
    std::lock_guard lock(mutex_);
    rules_.clear();
}

void RiskManager::remove(const std::shared_ptr<RiskRule>& rule) {
    std::lock_guard lock(mutex_);
    rules_.erase(std::remove(rules_.begin(), rules_.end(), rule), rules_.end());
}

std::size_t RiskManager::size() const {
    std::lock_guard lock(mutex_);
    return rules_.size();
}

void RiskManager::rollover_if_needed_locked() {
    const auto day = china_day_key();
    if (trading_day_.empty()) {
        trading_day_ = day;
        return;
    }
    if (day == trading_day_) {
        return;
    }
    trading_day_ = day;
    for (auto& rule : rules_) {
        rule->on_new_trading_day();
    }
}

std::optional<std::string> RiskManager::check_insert(const InsertOrderReq& req) {
    std::lock_guard lock(mutex_);
    rollover_if_needed_locked();
    for (const auto& rule : rules_) {
        if (auto error = rule->check_insert(req); error.has_value()) {
            return error;
        }
    }
    return std::nullopt;
}

void RiskManager::on_insert(const InsertOrderReq& req) {
    std::lock_guard lock(mutex_);
    rollover_if_needed_locked();
    for (auto& rule : rules_) {
        rule->on_insert(req);
    }
}

std::optional<std::string> RiskManager::check_cancel(const CancelOrderReq& req) {
    std::lock_guard lock(mutex_);
    rollover_if_needed_locked();
    for (const auto& rule : rules_) {
        if (auto error = rule->check_cancel(req); error.has_value()) {
            return error;
        }
    }
    return std::nullopt;
}

void RiskManager::on_cancel(const CancelOrderReq& req) {
    std::lock_guard lock(mutex_);
    rollover_if_needed_locked();
    for (auto& rule : rules_) {
        rule->on_cancel(req);
    }
}

} // namespace tqsdk

#pragma once

#include "tqsdk/pch.h"

#include "tqsdk/gateway/trading_gateway.h"

#include <deque>

namespace tqsdk {

/// 单条风控规则。规则实现应保持轻量、无阻塞，并只在下单/撤单路径调用。
class RiskRule {
public:
    virtual ~RiskRule() = default;

    /// 返回空表示允许；返回字符串表示拒绝原因。
    virtual std::optional<std::string> check_insert(const InsertOrderReq&) { return std::nullopt; }
    virtual void                       on_insert(const InsertOrderReq&) {}
    virtual std::optional<std::string> check_cancel(const CancelOrderReq&) { return std::nullopt; }
    virtual void                       on_cancel(const CancelOrderReq&) {}
    virtual void                       on_new_trading_day() {}
};

/// 交易日内某合约/一组合约的开仓次数限制。
class OpenCountsLimitRule final : public RiskRule {
public:
    OpenCountsLimitRule(std::uint64_t limit, std::vector<std::string> symbols);
    std::optional<std::string> check_insert(const InsertOrderReq& req) override;
    void                       on_insert(const InsertOrderReq& req) override;
    void                       on_new_trading_day() override;

private:
    std::uint64_t                                  limit_;
    std::unordered_set<std::string>                symbols_;
    std::unordered_map<std::string, std::uint64_t> counts_;
};

/// 交易日内某合约/一组合约的开仓手数限制。
class OpenVolumesLimitRule final : public RiskRule {
public:
    OpenVolumesLimitRule(std::uint64_t limit, std::vector<std::string> symbols);
    std::optional<std::string> check_insert(const InsertOrderReq& req) override;
    void                       on_insert(const InsertOrderReq& req) override;
    void                       on_new_trading_day() override;

private:
    std::uint64_t                                  limit_;
    std::unordered_set<std::string>                symbols_;
    std::unordered_map<std::string, std::uint64_t> volumes_;
};

/// 报单频率限制：window 内最多 limit 笔（只统计通过风控的报单）。
class OrderRateLimitRule final : public RiskRule {
public:
    OrderRateLimitRule(std::uint64_t limit, std::chrono::milliseconds window);
    std::optional<std::string> check_insert(const InsertOrderReq& req) override;
    void                       on_insert(const InsertOrderReq& req) override;

private:
    std::uint64_t                                     limit_;
    std::chrono::milliseconds                         window_;
    std::deque<std::chrono::steady_clock::time_point> accepted_;
};

/// 下单/撤单风控管理器。
///
/// 当前框架是单账户；多账户扩展时，可在 RiskRule 中增加 account_key，
/// 或为每个 TradingGateway 各持有一个 RiskManager。
class RiskManager {
public:
    RiskManager() = default;

    void        add(std::shared_ptr<RiskRule> rule);
    void        remove(const std::shared_ptr<RiskRule>& rule);
    void        clear();
    std::size_t size() const;

    std::optional<std::string> check_insert(const InsertOrderReq& req);
    void                       on_insert(const InsertOrderReq& req);
    std::optional<std::string> check_cancel(const CancelOrderReq& req);
    void                       on_cancel(const CancelOrderReq& req);

private:
    void rollover_if_needed_locked();

    mutable std::mutex                     mutex_;
    std::vector<std::shared_ptr<RiskRule>> rules_;
    std::string                            trading_day_;
};

} // namespace tqsdk

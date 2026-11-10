#pragma once

#include "tqsdk/pch.h"
#include "tqsdk/tqapi.h"

namespace tqsdk {

/// 多账户门面。
///
/// 每个账户持有独立的 TqApi/DataManager/TradingGateway；本类只做按账户路由
/// 和跨账户快照聚合，不把不同账户的持仓/资金混在同一个 DataManager 中。
/// 这样账户隔离边界与单账户 TqApi 完全一致，也便于后续接入独立网关。
class MultiAccountManager {
public:
    using AccountId = std::string;

    MultiAccountManager() = default;

    /// 注册账户；account_id 重复时保留原账户并返回 false。
    bool                   add_account(AccountId account_id, std::shared_ptr<TqApi> api);
    bool                   remove_account(const AccountId& account_id);
    std::shared_ptr<TqApi> find_account(const AccountId& account_id) const;
    std::vector<AccountId> account_ids() const;
    std::string            default_account_id() const;

    asio::awaitable<Order*> insert_order(const AccountId& account_id, const InsertOrderReq& req);
    asio::awaitable<CommandAck> cancel_order(const AccountId&      account_id,
                                             const CancelOrderReq& req);

    /// 关闭全部账户的 TqApi（幂等）。
    ///
    /// Closed TqApis are retired into an internal vector instead of being
    /// destroyed immediately: their coroutines may still be queued on the
    /// io_context after close(), and destroying the API before draining them
    /// causes use-after-free/double-free. The vector is released with this
    /// manager (after the caller has drained io_context).
    void close();

    /// 账户/持仓/委托/成交快照聚合；键为 account_id。
    nlohmann::json snapshot() const;

private:
    mutable std::mutex                                    mutex_;
    std::unordered_map<AccountId, std::shared_ptr<TqApi>> accounts_;
    std::vector<std::shared_ptr<TqApi>>                   retired_apis_;
    std::string                                           default_account_id_;
};

} // namespace tqsdk

#include "tqsdk/multiaccount/multi_account_manager.h"

namespace tqsdk {

bool MultiAccountManager::add_account(AccountId account_id, std::shared_ptr<TqApi> api) {
    if (account_id.empty() || !api)
        return false;
    std::lock_guard lock(mutex_);
    const bool      inserted = accounts_.try_emplace(account_id, std::move(api)).second;
    if (inserted && default_account_id_.empty())
        default_account_id_ = account_id;
    return inserted;
}

bool MultiAccountManager::remove_account(const AccountId& account_id) {
    std::shared_ptr<TqApi> retiring;
    AccountRemovedHandler  handler;
    {
        std::lock_guard lock(mutex_);
        auto            it = accounts_.find(account_id);
        if (it == accounts_.end())
            return false;
        handler  = account_removed_handler_;
        retiring = std::move(it->second);
        accounts_.erase(it);
        // Retain until io_context drain; see close().
        retired_apis_.push_back(retiring);
    }
    // Stop downstream clients before the API disappears.
    if (handler) {
        try {
            handler(account_id);
        } catch (...) {
        }
    }
    if (retiring)
        retiring->close();
    return true;
}

void MultiAccountManager::set_account_removed_handler(AccountRemovedHandler handler) {
    std::lock_guard lock(mutex_);
    account_removed_handler_ = std::move(handler);
}

std::shared_ptr<TqApi> MultiAccountManager::find_account(const AccountId& account_id) const {
    std::lock_guard lock(mutex_);
    auto            it = accounts_.find(account_id);
    return it == accounts_.end() ? nullptr : it->second;
}

std::string MultiAccountManager::default_account_id() const {
    std::lock_guard lock(mutex_);
    return default_account_id_;
}

std::vector<MultiAccountManager::AccountId> MultiAccountManager::account_ids() const {
    std::lock_guard        lock(mutex_);
    std::vector<AccountId> out;
    out.reserve(accounts_.size());
    for (const auto& [id, _] : accounts_)
        out.push_back(id);
    std::sort(out.begin(), out.end());
    return out;
}

asio::awaitable<Order*> MultiAccountManager::insert_order(const AccountId&      account_id,
                                                          const InsertOrderReq& req) {
    auto api = find_account(account_id);
    if (!api)
        co_return nullptr;
    co_return co_await api->insert_order(req);
}

asio::awaitable<CommandAck> MultiAccountManager::cancel_order(const AccountId&      account_id,
                                                              const CancelOrderReq& req) {
    auto api = find_account(account_id);
    if (!api) {
        co_return CommandAck{false, req.order_id, "account not found"};
    }
    co_return co_await api->cancel_order(req);
}

void MultiAccountManager::close() {
    std::vector<std::pair<AccountId, std::shared_ptr<TqApi>>> closing;
    AccountRemovedHandler                                     handler;
    {
        std::lock_guard lock(mutex_);
        closing.reserve(accounts_.size());
        handler = account_removed_handler_;
        for (auto& [id, api] : accounts_)
            closing.emplace_back(id, std::move(api));
        accounts_.clear();
        default_account_id_.clear();
        for (const auto& [id, api] : closing)
            retired_apis_.push_back(api);
    }
    for (const auto& [id, api] : closing) {
        if (handler) {
            try {
                handler(id);
            } catch (...) {
            }
        }
        if (api)
            api->close();
    }
}

namespace {

nlohmann::json account_json(const TqApi& api) {
    const auto& a   = api.get_account();
    auto        num = [](double v) {
        return std::isfinite(v) ? nlohmann::json(v) : nlohmann::json(nullptr);
    };
    return nlohmann::json{
        {"account_id", a.account_id},
        {"balance", num(a.balance)},
        {"available", num(a.available)},
        {"margin", num(a.margin)},
        {"float_profit", num(a.float_profit)},
        {"close_profit", num(a.close_profit)},
        {"commission", num(a.commission)},
        {"risk_ratio", num(a.risk_ratio)},
    };
}

nlohmann::json positions_json(const TqApi& api) {
    auto out = nlohmann::json::array();
    for (const auto& [symbol, p] : api.data().positions) {
        out.push_back(nlohmann::json{{"instrument_id", symbol},
                                     {"volume_long", p.volume_long},
                                     {"volume_short", p.volume_short},
                                     {"open_price_long", p.open_price_long},
                                     {"open_price_short", p.open_price_short},
                                     {"float_profit", p.float_profit},
                                     {"last_price", p.last_price}});
    }
    return out;
}

nlohmann::json orders_json(const TqApi& api) {
    auto out = nlohmann::json::array();
    for (const auto& [id, o] : api.data().orders) {
        out.push_back(nlohmann::json{{"order_id", id},
                                     {"instrument_id", o.instrument_id},
                                     {"direction", magic_enum::enum_name(o.direction)},
                                     {"offset", magic_enum::enum_name(o.offset)},
                                     {"volume_orign", o.volume_orign},
                                     {"volume_left", o.volume_left},
                                     {"status", magic_enum::enum_name(o.status)},
                                     {"is_dead", o.is_dead},
                                     {"is_error", o.is_error},
                                     {"last_msg", o.last_msg}});
    }
    return out;
}

nlohmann::json trades_json(const TqApi& api) {
    auto out = nlohmann::json::array();
    for (const auto& [id, t] : api.data().trades) {
        out.push_back(nlohmann::json{{"trade_id", id},
                                     {"order_id", t.order_id},
                                     {"instrument_id", t.instrument_id},
                                     {"direction", magic_enum::enum_name(t.direction)},
                                     {"offset", magic_enum::enum_name(t.offset)},
                                     {"price", t.price},
                                     {"volume", t.volume}});
    }
    return out;
}

} // namespace

nlohmann::json MultiAccountManager::snapshot() const {
    std::vector<std::pair<AccountId, std::shared_ptr<TqApi>>> copy;
    {
        std::lock_guard lock(mutex_);
        copy.reserve(accounts_.size());
        for (const auto& [id, api] : accounts_)
            copy.emplace_back(id, api);
    }
    std::sort(
        copy.begin(), copy.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

    auto out = nlohmann::json::object();
    for (const auto& [id, api] : copy) {
        out[id] = nlohmann::json{{"account", account_json(*api)},
                                 {"positions", positions_json(*api)},
                                 {"orders", orders_json(*api)},
                                 {"trades", trades_json(*api)}};
    }
    return out;
}

} // namespace tqsdk

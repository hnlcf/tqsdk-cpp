#pragma once

#include "tqsdk/pch.h"

#include "tqsdk/multiaccount/multi_account_manager.h"
#include "tqsdk/tqapi.h"

namespace tqsdk {
namespace serve {

struct TaskRegistry;

/// Resolved account context for one HTTP request.
///
/// `id` is the concrete account key (never empty); `api` is owned by the
/// MultiAccountManager and must outlive the request.
struct AccountScope {
    std::string   id;
    TqApi*        api   = nullptr;
    TaskRegistry* tasks = nullptr;

    explicit operator bool() const noexcept { return api != nullptr; }
};

/// Thrown when `?account=` names an account that is not registered.
class AccountNotFound : public std::runtime_error {
public:
    explicit AccountNotFound(std::string account_id)
        : std::runtime_error("account not found: " + account_id),
          account_id(std::move(account_id)) {}

    std::string account_id;
};

/// Resolve `?account=` once, in one place.
///
/// Before multi-account was introduced, `account_api()` simply returned the
/// primary TqApi for any request. Keep that compatibility when no manager is
/// configured; with a manager, an empty query selects the default account and
/// an unknown id raises AccountNotFound instead of silently reading the wrong
/// account.
class AccountRouter {
public:
    AccountRouter(TqApi* primary, MultiAccountManager* accounts)
        : primary_(primary),
          accounts_(accounts) {}

    /// Resolve or throw AccountNotFound. Empty `requested` means default.
    AccountScope resolve(std::string_view requested) const;

    /// Non-throwing variant used by routing code that wants to return 404.
    std::optional<AccountScope> try_resolve(std::string_view requested) const;

    /// Concrete default account id; never empty.
    std::string default_id() const;

    /// Registered account ids (or {"default"} when no manager is configured).
    std::vector<std::string> account_ids() const;

private:
    TqApi*               primary_  = nullptr;
    MultiAccountManager* accounts_ = nullptr;
};

} // namespace serve
} // namespace tqsdk

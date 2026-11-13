#include "tqsdk/serve/account_router.h"

namespace tqsdk {
namespace serve {

std::string AccountRouter::default_id() const {
    if (accounts_ == nullptr) {
        return "default";
    }
    auto id = accounts_->default_account_id();
    return id.empty() ? "default" : id;
}

std::optional<AccountScope> AccountRouter::resolve(std::string_view requested) const {
    if (accounts_ == nullptr) {
        return AccountScope{requested.empty() ? std::string("default") : std::string(requested),
                            primary_};
    }

    const std::string id  = requested.empty() ? default_id() : std::string(requested);
    auto              api = accounts_->find_account(id);
    if (!api) {
        return std::nullopt;
    }
    return AccountScope{id, api.get()};
}

std::optional<AccountScope> AccountRouter::try_resolve(std::string_view requested) const {
    return resolve(requested);
}

std::vector<std::string> AccountRouter::account_ids() const {
    if (accounts_ == nullptr) {
        return {"default"};
    }
    return accounts_->account_ids();
}

} // namespace serve
} // namespace tqsdk

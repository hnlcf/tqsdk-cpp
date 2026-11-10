#pragma once

#include "tqsdk/pch.h"

#include "tqsdk/trading/order_tasks.h"

namespace tqsdk {
namespace serve {

/// Per-account task ownership. Handlers resolve the account once and then see
/// only that account's tasks; task ids are intentionally local to the account.
struct TaskRegistry {
    std::uint64_t                                                     seq = 0;
    std::unordered_map<std::string, std::shared_ptr<TargetPosTask>>   targets;
    std::unordered_map<std::string, std::shared_ptr<InsertOrderTask>> orders;
};

} // namespace serve
} // namespace tqsdk

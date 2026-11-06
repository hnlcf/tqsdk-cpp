#include "tqsdk/data/diff_merge_engine.h"
#include "tqsdk/data/field_table.h"
#include "tqsdk/domain/enum_util.h"
#include "tqsdk/types.h"

namespace tqsdk {

void DiffMergeEngine::apply(const nlohmann::json& push) {
    if (!push.is_object())
        return;
    for (auto it = push.begin(); it != push.end(); ++it) {
        std::string_view k = it.key();
        SPDLOG_DEBUG("[diff] Merge {} event", k);
        if (k == "quotes") {
            merge_quotes(it.value());
        } else if (k == "trade") {
            merge_trade(it.value());
        } else if (k == "klines") {
            merge_klines(it.value());
        } else {
            // 未知顶层路由忽略
            SPDLOG_ERROR("Unimplemented route for merging: {}", k);
            continue;
        }
    }
}

} // namespace tqsdk

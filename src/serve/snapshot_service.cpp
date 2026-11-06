#include "tqsdk/serve/snapshot_service.h"

#include "tqsdk/serve/json_convert.h"
#include "tqsdk/tqapi.h"

namespace tqsdk::serve {

nlohmann::json account_snapshot(const TqApi& api) {
    return account_to_json(api.data().account);
}

nlohmann::json orders_snapshot(const TqApi& api) {
    nlohmann::json out = nlohmann::json::object();
    for (const auto& [order_id, order] : api.data().orders) {
        out[order_id] = order_to_json(order);
    }
    return nlohmann::json{{"orders", std::move(out)}};
}

nlohmann::json trades_snapshot(const TqApi& api) {
    nlohmann::json out = nlohmann::json::object();
    for (const auto& [trade_id, trade] : api.data().trades) {
        out[trade_id] = trade_to_json(trade);
    }
    return nlohmann::json{{"trades", std::move(out)}};
}

nlohmann::json positions_snapshot(const TqApi& api, bool hide_empty) {
    nlohmann::json out = nlohmann::json::object();
    for (const auto& [symbol, position] : api.data().positions) {
        if (hide_empty && position.volume_long == 0 && position.volume_short == 0) {
            continue;
        }
        out[symbol] = position_to_json(position);
    }
    return nlohmann::json{{"positions", std::move(out)}};
}

nlohmann::json trading_snapshot(const TqApi& api, bool hide_empty_positions) {
    nlohmann::json out = nlohmann::json::object();
    out["account"]     = account_snapshot(api);
    out["orders"]      = orders_snapshot(api)["orders"];
    out["trades"]      = trades_snapshot(api)["trades"];
    out["positions"]   = positions_snapshot(api, hide_empty_positions)["positions"];
    return out;
}

} // namespace tqsdk::serve

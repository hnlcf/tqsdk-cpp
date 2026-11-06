#pragma once

#include "tqsdk/pch.h"

#include "tqsdk/serve/json_convert.h"

#include <map>
#include <set>
#include <unordered_map>

namespace tqsdk {
class InstrumentCatalog;
class TqApi;
} // namespace tqsdk

namespace tqsdk::serve {

/// 一个 WebSocket 客户端当前订阅的实体集合。
struct WsEntitySelection {
    std::set<std::string>                          symbols;
    std::map<std::pair<std::string, int64_t>, int> klines; // symbol,duration -> view width
    std::set<std::string>                          topics;
};

/// WebSocket 快照/增量过滤器。
///
/// 该类负责三件事，和网络连接生命周期解耦：
///   1. 按客户端订阅裁剪 snapshot；
///   2. 按 topics 裁剪全局 changed_fields diff；
///   3. 为每个客户端保存最近下发的实体 JSON，过滤 CTP 全量查询造成的伪 diff。
///
/// 每个 WebSocket 客户端持有独立实例，因此快照更新不会影响其他客户端的
/// 已发送缓存（避免多客户端下漏发真实变化）。
class WsEntityFilter {
public:
    WsEntityFilter() = default;

    WsEntityFilter(const WsEntityFilter&)            = delete;
    WsEntityFilter& operator=(const WsEntityFilter&) = delete;

    nlohmann::json snapshot(const TqApi&             api,
                            const WsEntitySelection& selection,
                            const InstrumentCatalog* catalog = nullptr) const;

    nlohmann::json filter(const WsEntitySelection& selection, const nlohmann::json& diff) const;

    void reset();

private:
    mutable std::unordered_map<std::string, std::string> sent_order_json_;
    mutable std::unordered_map<std::string, std::string> sent_trade_json_;
    mutable std::unordered_map<std::string, std::string> sent_position_json_;
    mutable std::string                                  sent_account_json_;
};

} // namespace tqsdk::serve

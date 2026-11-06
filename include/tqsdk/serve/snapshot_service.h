#pragma once

#include "tqsdk/pch.h"

namespace tqsdk {
class TqApi;
} // namespace tqsdk

namespace tqsdk::serve {

/// 交易/账户快照的 REST/WS 共用投影。
///
/// 只负责把 DataManager 的强类型实体投影成 wire JSON；不负责过滤客户端
/// 订阅、不负责缓存，也不修改 DataManager。这样 HTTP 与 WebSocket 共享
/// 字段映射和空仓位策略，避免两套循环逐渐漂移。
nlohmann::json account_snapshot(const TqApi& api);
nlohmann::json orders_snapshot(const TqApi& api);
nlohmann::json trades_snapshot(const TqApi& api);
nlohmann::json positions_snapshot(const TqApi& api, bool hide_empty = true);
nlohmann::json trading_snapshot(const TqApi& api, bool hide_empty_positions = true);

} // namespace tqsdk::serve

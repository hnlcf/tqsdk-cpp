#pragma once

#include "tqsdk/pch.h"

#include "tqsdk/domain/account.h"
#include "tqsdk/domain/kline.h"
#include "tqsdk/domain/order.h"
#include "tqsdk/domain/position.h"
#include "tqsdk/domain/quote.h"
#include "tqsdk/domain/trade.h"

namespace tqsdk {
class InstrumentCatalog;
namespace serve {

// ============================================================================
// DataManager 实体 -> JSON
// ----------------------------------------------------------------------------
// 供 REST（http_server）与 WebSocket（ws_stream）共用，避免两套字段映射漂移。
// 约定：
//   - 非有限 double（kUnknownDouble=NaN）一律输出 null，前端用 null 表示"未知"。
//   - * _diff 版本只输出 changed_fields 中置位的字段（WebSocket 增量推送用）。
//   - K 线增量以 {"klines":{symbol:{duration_ns:{"reset":bool,"bars":[...]}}}}
//     形式表达；reset=true 时 bars 是全量，否则只含最后一根。
// ============================================================================

/// 可选 catalog：把 price_tick / volume_multiple / price_decs 等静态字段
/// 合并进行情 JSON，避免 CTP/Tq 推送只带动态字段时前端拿到 null。
nlohmann::json quote_to_json(const Quote& q, const InstrumentCatalog* catalog = nullptr);
nlohmann::json quote_diff_to_json(const Quote& q, const InstrumentCatalog* catalog = nullptr);

// REST 形态：{"symbol","duration_ns","bars":[...]}
nlohmann::json kline_to_json(const KlineStream& s);
// WebSocket 形态：{"klines":{symbol:{duration_ns:{"reset":bool,"bars":[...]}}}}
nlohmann::json kline_ws_json(const KlineStream& s, bool force_reset = false);

nlohmann::json account_to_json(const Account& a);
nlohmann::json order_to_json(const Order& o);
nlohmann::json trade_to_json(const Trade& t);
nlohmann::json position_to_json(const Position& p);

} // namespace serve
} // namespace tqsdk

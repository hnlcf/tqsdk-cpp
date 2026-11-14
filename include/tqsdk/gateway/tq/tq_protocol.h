#pragma once

#include "tqsdk/pch.h"

#include "tqsdk/types.h"

namespace tqsdk::tq {

/// 天勤协议所有已知 aid。
///
/// 客户端 -> 服务端：
///   req_login / confirm_settlement / subscribe_quote / ins_query / set_chart /
///   set_chart_data / peek_message / insert_order / cancel_order /
///   pre_insert_order / set_risk_management_rule / subscribe_trading_status /
///   set_report_data / ratio / heartbeat / terminate
///
/// 服务端 -> 客户端：
///   rtn_data / rtn_brokers / rsp_login / rsp_quotes
enum class Aid : std::uint8_t {
    ReqLogin,
    ConfirmSettlement,
    SubscribeQuote,
    InsQuery,
    SetChart,
    SetChartData,
    PeekMessage,
    InsertOrder,
    CancelOrder,
    PreInsertOrder,
    SetRiskManagementRule,
    SubscribeTradingStatus,
    SetReportData,
    Ratio,
    Heartbeat,
    Terminate,
    RtnData,
    RtnBrokers,
    RspLogin,
    RspQuotes,
    Unknown,
};

constexpr std::string_view aid_name(Aid aid) noexcept {
    switch (aid) {
    case Aid::ReqLogin:
        return "req_login";
    case Aid::ConfirmSettlement:
        return "confirm_settlement";
    case Aid::SubscribeQuote:
        return "subscribe_quote";
    case Aid::InsQuery:
        return "ins_query";
    case Aid::SetChart:
        return "set_chart";
    case Aid::SetChartData:
        return "set_chart_data";
    case Aid::PeekMessage:
        return "peek_message";
    case Aid::InsertOrder:
        return "insert_order";
    case Aid::CancelOrder:
        return "cancel_order";
    case Aid::PreInsertOrder:
        return "pre_insert_order";
    case Aid::SetRiskManagementRule:
        return "set_risk_management_rule";
    case Aid::SubscribeTradingStatus:
        return "subscribe_trading_status";
    case Aid::SetReportData:
        return "set_report_data";
    case Aid::Ratio:
        return "ratio";
    case Aid::Heartbeat:
        return "heartbeat";
    case Aid::Terminate:
        return "terminate";
    case Aid::RtnData:
        return "rtn_data";
    case Aid::RtnBrokers:
        return "rtn_brokers";
    case Aid::RspLogin:
        return "rsp_login";
    case Aid::RspQuotes:
        return "rsp_quotes";
    case Aid::Unknown:
        return "unknown";
    }
    return "unknown";
}

inline Aid parse_aid(std::string_view name) noexcept {
    for (Aid aid : {Aid::ReqLogin,
                    Aid::ConfirmSettlement,
                    Aid::SubscribeQuote,
                    Aid::InsQuery,
                    Aid::SetChart,
                    Aid::SetChartData,
                    Aid::PeekMessage,
                    Aid::InsertOrder,
                    Aid::CancelOrder,
                    Aid::PreInsertOrder,
                    Aid::SetRiskManagementRule,
                    Aid::SubscribeTradingStatus,
                    Aid::SetReportData,
                    Aid::Ratio,
                    Aid::Heartbeat,
                    Aid::Terminate,
                    Aid::RtnData,
                    Aid::RtnBrokers,
                    Aid::RspLogin,
                    Aid::RspQuotes}) {
        if (aid_name(aid) == name) {
            return aid;
        }
    }
    return Aid::Unknown;
}

struct ConfirmSettlement {
    static constexpr Aid kAid = Aid::ConfirmSettlement;
};
inline void to_json(nlohmann::json& j, const ConfirmSettlement&) {
    j = nlohmann::json{{"aid", aid_name(ConfirmSettlement::kAid)}};
}
inline void from_json(const nlohmann::json&, ConfirmSettlement&) {}

struct PeekMessage {
    static constexpr Aid kAid = Aid::PeekMessage;
};
inline void to_json(nlohmann::json& j, const PeekMessage&) {
    j = nlohmann::json{{"aid", aid_name(PeekMessage::kAid)}};
}
inline void from_json(const nlohmann::json&, PeekMessage&) {}

struct Heartbeat {
    static constexpr Aid kAid = Aid::Heartbeat;
};
inline void to_json(nlohmann::json& j, const Heartbeat&) {
    j = nlohmann::json{{"aid", aid_name(Heartbeat::kAid)}};
}
inline void from_json(const nlohmann::json&, Heartbeat&) {}

struct Terminate {
    static constexpr Aid kAid = Aid::Terminate;
};
inline void to_json(nlohmann::json& j, const Terminate&) {
    j = nlohmann::json{{"aid", aid_name(Terminate::kAid)}};
}
inline void from_json(const nlohmann::json&, Terminate&) {}

/// 账户登录。不同 OTG backend 的字段是并集：
/// - 普通 TqAccount：bid/user_name/password（可选 front/broker_id）；
/// - TqCtp/TqJees/TqRohon：app_id/auth_code/backend/front；
/// - TqIS/TqO32：trading_fronts/license_file 等。
struct LoginRequest {
    static constexpr Aid kAid = Aid::ReqLogin;

    /// broker_name（天勤元数据键）放在 bid；broker_id 是可选的前置/柜台 id。
    std::string                   bid;
    std::string                   user_name;
    std::string                   password;
    std::optional<std::string>    client_mac_address;
    std::optional<std::string>    client_app_id;
    std::optional<std::string>    client_system_info;
    std::optional<std::string>    broker_id;
    std::optional<std::string>    front;
    std::optional<std::string>    backend;
    std::optional<std::string>    app_id;
    std::optional<std::string>    auth_code;
    std::optional<std::string>    license_file;
    std::optional<nlohmann::json> trading_fronts;
};

inline void to_json(nlohmann::json& j, const LoginRequest& r) {
    j        = nlohmann::json{{"aid", aid_name(LoginRequest::kAid)},
                              {"bid", r.bid},
                              {"user_name", r.user_name},
                              {"password", r.password}};
    auto put = [&j](const char* key, const auto& value) {
        if (value.has_value()) {
            j[key] = *value;
        }
    };
    put("client_mac_address", r.client_mac_address);
    put("client_app_id", r.client_app_id);
    put("client_system_info", r.client_system_info);
    put("broker_id", r.broker_id);
    put("front", r.front);
    put("backend", r.backend);
    put("app_id", r.app_id);
    put("auth_code", r.auth_code);
    put("license_file", r.license_file);
    put("trading_fronts", r.trading_fronts);
}
inline void from_json(const nlohmann::json& j, LoginRequest& r) {
    r.bid       = j.value("bid", std::string{});
    r.user_name = j.value("user_name", std::string{});
    r.password  = j.value("password", std::string{});
    auto get    = [&j](const char* key) -> std::optional<std::string> {
        if (auto it = j.find(key); it != j.end() && it->is_string()) {
            return it->get<std::string>();
        }
        return std::nullopt;
    };
    r.client_mac_address = get("client_mac_address");
    r.client_app_id      = get("client_app_id");
    r.client_system_info = get("client_system_info");
    r.broker_id          = get("broker_id");
    r.front              = get("front");
    r.backend            = get("backend");
    r.app_id             = get("app_id");
    r.auth_code          = get("auth_code");
    r.license_file       = get("license_file");
    if (auto it = j.find("trading_fronts"); it != j.end()) {
        r.trading_fronts = *it;
    }
}

struct SubscribeQuote {
    static constexpr Aid kAid = Aid::SubscribeQuote;
    std::string          ins_list;
};
inline void to_json(nlohmann::json& j, const SubscribeQuote& r) {
    j = nlohmann::json{{"aid", aid_name(SubscribeQuote::kAid)}, {"ins_list", r.ins_list}};
}
inline void from_json(const nlohmann::json& j, SubscribeQuote& r) {
    r.ins_list = j.value("ins_list", std::string{});
}

struct InsQuery {
    static constexpr Aid          kAid = Aid::InsQuery;
    std::string                   query_id;
    std::string                   query;
    std::optional<nlohmann::json> variables;
};
inline void to_json(nlohmann::json& j, const InsQuery& r) {
    j = nlohmann::json{
        {"aid", aid_name(InsQuery::kAid)}, {"query_id", r.query_id}, {"query", r.query}};
    if (r.variables) {
        j["variables"] = *r.variables;
    }
}
inline void from_json(const nlohmann::json& j, InsQuery& r) {
    r.query_id = j.value("query_id", std::string{});
    r.query    = j.value("query", std::string{});
    if (auto it = j.find("variables"); it != j.end()) {
        r.variables = *it;
    }
}

struct SetChart {
    static constexpr Aid        kAid = Aid::SetChart;
    std::string                 chart_id;
    std::string                 ins_list;
    std::int64_t                duration   = 0;
    int                         view_width = 200;
    std::optional<std::int64_t> focus_datetime;
    std::optional<int>          focus_position;
};
inline void to_json(nlohmann::json& j, const SetChart& r) {
    j = nlohmann::json{{"aid", aid_name(SetChart::kAid)},
                       {"chart_id", r.chart_id},
                       {"ins_list", r.ins_list},
                       {"duration", r.duration},
                       {"view_width", r.view_width}};
    if (r.focus_datetime) {
        j["focus_datetime"] = *r.focus_datetime;
    }
    if (r.focus_position) {
        j["focus_position"] = *r.focus_position;
    }
}
inline void from_json(const nlohmann::json& j, SetChart& r) {
    r.chart_id   = j.value("chart_id", std::string{});
    r.ins_list   = j.value("ins_list", std::string{});
    r.duration   = j.value("duration", std::int64_t{0});
    r.view_width = j.value("view_width", 200);
    if (auto it = j.find("focus_datetime"); it != j.end() && it->is_number_integer()) {
        r.focus_datetime = it->get<std::int64_t>();
    }
    if (auto it = j.find("focus_position"); it != j.end() && it->is_number_integer()) {
        r.focus_position = it->get<int>();
    }
}

struct SetChartData {
    static constexpr Aid kAid = Aid::SetChartData;
    std::string          symbol;
    std::int64_t         dur_nano = 0;
    nlohmann::json       datas    = nlohmann::json::object();
};
inline void to_json(nlohmann::json& j, const SetChartData& r) {
    j = nlohmann::json{{"aid", aid_name(SetChartData::kAid)},
                       {"symbol", r.symbol},
                       {"dur_nano", r.dur_nano},
                       {"datas", r.datas}};
}
inline void from_json(const nlohmann::json& j, SetChartData& r) {
    r.symbol   = j.value("symbol", std::string{});
    r.dur_nano = j.value("dur_nano", std::int64_t{0});
    r.datas    = j.value("datas", nlohmann::json::object());
}

struct SubscribeTradingStatus {
    static constexpr Aid kAid = Aid::SubscribeTradingStatus;
    std::string          ins_list;
};
inline void to_json(nlohmann::json& j, const SubscribeTradingStatus& r) {
    j = nlohmann::json{{"aid", aid_name(SubscribeTradingStatus::kAid)}, {"ins_list", r.ins_list}};
}
inline void from_json(const nlohmann::json& j, SubscribeTradingStatus& r) {
    r.ins_list = j.value("ins_list", std::string{});
}

/// 期货报单请求。字段名完全对齐官方 OTG 交易报文。
struct InsertOrder {
    static constexpr Aid  kAid = Aid::InsertOrder;
    std::string           account_key;
    std::string           user_id;
    std::string           order_id;
    std::string           exchange_id;
    std::string           instrument_id;
    Direction             direction  = Direction::BUY;
    Offset                offset     = Offset::OPEN;
    std::uint64_t         volume     = 0;
    PriceType             price_type = PriceType::LIMIT;
    std::optional<double> limit_price;
    std::string           time_condition   = "GFD";
    std::string           volume_condition = "ANY";
    std::string           offset_leg2;
    std::string           hedge_flag;
    std::string           contingent_condition;
};
inline void to_json(nlohmann::json& j, const InsertOrder& r) {
    j = nlohmann::json{{"aid", aid_name(InsertOrder::kAid)},
                       {"account_key", r.account_key},
                       {"user_id", r.user_id},
                       {"order_id", r.order_id},
                       {"exchange_id", r.exchange_id},
                       {"instrument_id", r.instrument_id},
                       {"direction", r.direction},
                       {"offset", r.offset},
                       {"volume", r.volume},
                       {"price_type", r.price_type},
                       {"time_condition", r.time_condition},
                       {"volume_condition", r.volume_condition}};
    if (r.limit_price) {
        j["limit_price"] = *r.limit_price;
    }
    if (!r.offset_leg2.empty()) {
        j["offset_leg2"] = r.offset_leg2;
    }
    if (!r.hedge_flag.empty()) {
        j["hedge_flag"] = r.hedge_flag;
    }
    if (!r.contingent_condition.empty()) {
        j["contingent_condition"] = r.contingent_condition;
    }
}
inline void from_json(const nlohmann::json& j, InsertOrder& r) {
    r.account_key   = j.value("account_key", std::string{});
    r.user_id       = j.value("user_id", std::string{});
    r.order_id      = j.value("order_id", std::string{});
    r.exchange_id   = j.value("exchange_id", std::string{});
    r.instrument_id = j.value("instrument_id", std::string{});
    r.direction     = j.value("direction", Direction::BUY);
    r.offset        = j.value("offset", Offset::OPEN);
    r.volume        = j.value("volume", std::uint64_t{0});
    r.price_type    = j.value("price_type", PriceType::LIMIT);
    if (auto it = j.find("limit_price"); it != j.end() && !it->is_null()) {
        r.limit_price = it->get<double>();
    }
    r.time_condition       = j.value("time_condition", std::string{"GFD"});
    r.volume_condition     = j.value("volume_condition", std::string{"ANY"});
    r.offset_leg2          = j.value("offset_leg2", std::string{});
    r.hedge_flag           = j.value("hedge_flag", std::string{});
    r.contingent_condition = j.value("contingent_condition", std::string{});
}

struct PreInsertOrder {
    static constexpr Aid kAid = Aid::PreInsertOrder;
    std::string          account_key;
    std::string          user_id;
    std::string          order_id;
    std::string          exchange_id;
    std::string          instrument_id;
    Direction            direction            = Direction::BUY;
    Offset               offset               = Offset::OPEN;
    std::uint64_t        volume               = 1;
    PriceType            price_type           = PriceType::LIMIT;
    double               limit_price          = 0.0;
    std::string          time_condition       = "GFD";
    std::string          volume_condition     = "ANY";
    std::string          hedge_flag           = "SPECULATION";
    std::string          contingent_condition = "IMMEDIATELY";
};
inline void to_json(nlohmann::json& j, const PreInsertOrder& r) {
    j = nlohmann::json{{"aid", aid_name(PreInsertOrder::kAid)},
                       {"account_key", r.account_key},
                       {"user_id", r.user_id},
                       {"order_id", r.order_id},
                       {"exchange_id", r.exchange_id},
                       {"instrument_id", r.instrument_id},
                       {"direction", r.direction},
                       {"offset", r.offset},
                       {"limit_price", r.limit_price},
                       {"volume", r.volume},
                       {"price_type", r.price_type},
                       {"volume_condition", r.volume_condition},
                       {"time_condition", r.time_condition},
                       {"hedge_flag", r.hedge_flag},
                       {"contingent_condition", r.contingent_condition}};
}
inline void from_json(const nlohmann::json& j, PreInsertOrder& r) {
    r.account_key          = j.value("account_key", std::string{});
    r.user_id              = j.value("user_id", std::string{});
    r.order_id             = j.value("order_id", std::string{});
    r.exchange_id          = j.value("exchange_id", std::string{});
    r.instrument_id        = j.value("instrument_id", std::string{});
    r.direction            = j.value("direction", Direction::BUY);
    r.offset               = j.value("offset", Offset::OPEN);
    r.limit_price          = j.value("limit_price", 0.0);
    r.volume               = j.value("volume", std::uint64_t{1});
    r.price_type           = j.value("price_type", PriceType::LIMIT);
    r.volume_condition     = j.value("volume_condition", std::string{"ANY"});
    r.time_condition       = j.value("time_condition", std::string{"GFD"});
    r.hedge_flag           = j.value("hedge_flag", std::string{"SPECULATION"});
    r.contingent_condition = j.value("contingent_condition", std::string{"IMMEDIATELY"});
}

struct CancelOrder {
    static constexpr Aid kAid = Aid::CancelOrder;
    std::string          account_key;
    std::string          user_id;
    std::string          order_id;
};
inline void to_json(nlohmann::json& j, const CancelOrder& r) {
    j = nlohmann::json{{"aid", aid_name(CancelOrder::kAid)},
                       {"account_key", r.account_key},
                       {"user_id", r.user_id},
                       {"order_id", r.order_id}};
}
inline void from_json(const nlohmann::json& j, CancelOrder& r) {
    r.account_key = j.value("account_key", std::string{});
    r.user_id     = j.value("user_id", std::string{});
    r.order_id    = j.value("order_id", std::string{});
}

struct SetRiskManagementRule {
    static constexpr Aid kAid = Aid::SetRiskManagementRule;
    std::string          account_key;
    std::string          user_id;
    std::string          exchange_id;
    bool                 enable                = false;
    nlohmann::json       self_trade            = nlohmann::json::object();
    nlohmann::json       frequent_cancellation = nlohmann::json::object();
    nlohmann::json       trade_position_ratio  = nlohmann::json::object();
};
inline void to_json(nlohmann::json& j, const SetRiskManagementRule& r) {
    j = nlohmann::json{{"aid", aid_name(SetRiskManagementRule::kAid)},
                       {"account_key", r.account_key},
                       {"user_id", r.user_id},
                       {"exchange_id", r.exchange_id},
                       {"enable", r.enable},
                       {"self_trade", r.self_trade},
                       {"frequent_cancellation", r.frequent_cancellation},
                       {"trade_position_ratio", r.trade_position_ratio}};
}
inline void from_json(const nlohmann::json& j, SetRiskManagementRule& r) {
    r.account_key           = j.value("account_key", std::string{});
    r.user_id               = j.value("user_id", std::string{});
    r.exchange_id           = j.value("exchange_id", std::string{});
    r.enable                = j.value("enable", false);
    r.self_trade            = j.value("self_trade", nlohmann::json::object());
    r.frequent_cancellation = j.value("frequent_cancellation", nlohmann::json::object());
    r.trade_position_ratio  = j.value("trade_position_ratio", nlohmann::json::object());
}

struct SetReportData {
    static constexpr Aid kAid         = Aid::SetReportData;
    nlohmann::json       report_datas = nlohmann::json::array();
};
inline void to_json(nlohmann::json& j, const SetReportData& r) {
    j = nlohmann::json{{"aid", aid_name(SetReportData::kAid)}, {"report_datas", r.report_datas}};
}
inline void from_json(const nlohmann::json& j, SetReportData& r) {
    r.report_datas = j.value("report_datas", nlohmann::json::array());
}

struct Ratio {
    static constexpr Aid kAid  = Aid::Ratio;
    double               speed = 1.0;
};
inline void to_json(nlohmann::json& j, const Ratio& r) {
    j = nlohmann::json{{"aid", aid_name(Ratio::kAid)}, {"speed", r.speed}};
}
inline void from_json(const nlohmann::json& j, Ratio& r) {
    r.speed = j.value("speed", 1.0);
}

struct RtnData {
    static constexpr Aid kAid = Aid::RtnData;
    nlohmann::json       data = nlohmann::json::array();
};
inline void to_json(nlohmann::json& j, const RtnData& r) {
    j = nlohmann::json{{"aid", aid_name(RtnData::kAid)}, {"data", r.data}};
}
inline void from_json(const nlohmann::json& j, RtnData& r) {
    r.data = j.value("data", nlohmann::json::array());
}

struct RtnBrokers {
    static constexpr Aid kAid         = Aid::RtnBrokers;
    nlohmann::json       capabilities = nlohmann::json::object();
};
inline void to_json(nlohmann::json& j, const RtnBrokers& r) {
    j = nlohmann::json{{"aid", aid_name(RtnBrokers::kAid)}, {"capabilities", r.capabilities}};
}
inline void from_json(const nlohmann::json& j, RtnBrokers& r) {
    r.capabilities = j.value("capabilities", nlohmann::json::object());
}

/// 目前官方响应里字段不固定，保留完整 raw，避免猜错字段。
struct RspLogin {
    static constexpr Aid kAid = Aid::RspLogin;
    nlohmann::json       raw  = nlohmann::json::object();
};
inline void to_json(nlohmann::json& j, const RspLogin& r) {
    j        = r.raw;
    j["aid"] = aid_name(RspLogin::kAid);
}
inline void from_json(const nlohmann::json& j, RspLogin& r) {
    r.raw = j;
}

struct RspQuotes {
    static constexpr Aid kAid = Aid::RspQuotes;
    nlohmann::json       raw  = nlohmann::json::object();
};
inline void to_json(nlohmann::json& j, const RspQuotes& r) {
    j        = r.raw;
    j["aid"] = aid_name(RspQuotes::kAid);
}
inline void from_json(const nlohmann::json& j, RspQuotes& r) {
    r.raw = j;
}

using Message = std::variant<LoginRequest,
                             ConfirmSettlement,
                             SubscribeQuote,
                             InsQuery,
                             SetChart,
                             SetChartData,
                             PeekMessage,
                             InsertOrder,
                             CancelOrder,
                             PreInsertOrder,
                             SetRiskManagementRule,
                             SubscribeTradingStatus,
                             SetReportData,
                             Ratio,
                             Heartbeat,
                             Terminate,
                             RtnData,
                             RtnBrokers,
                             RspLogin,
                             RspQuotes>;

struct ParseError {
    std::string detail;
};
using ParseResult = std::expected<Message, ParseError>;

inline nlohmann::json serialize(const Message& message) {
    return std::visit([](const auto& value) { return nlohmann::json(value); }, message);
}

inline ParseResult parse(const nlohmann::json& j) noexcept {
    const auto aid = parse_aid(j.value("aid", std::string{}));
    try {
        switch (aid) {
        case Aid::ReqLogin:
            return j.get<LoginRequest>();
        case Aid::ConfirmSettlement:
            return ConfirmSettlement{};
        case Aid::SubscribeQuote:
            return j.get<SubscribeQuote>();
        case Aid::InsQuery:
            return j.get<InsQuery>();
        case Aid::SetChart:
            return j.get<SetChart>();
        case Aid::SetChartData:
            return j.get<SetChartData>();
        case Aid::PeekMessage:
            return PeekMessage{};
        case Aid::InsertOrder:
            return j.get<InsertOrder>();
        case Aid::CancelOrder:
            return j.get<CancelOrder>();
        case Aid::PreInsertOrder:
            return j.get<PreInsertOrder>();
        case Aid::SetRiskManagementRule:
            return j.get<SetRiskManagementRule>();
        case Aid::SubscribeTradingStatus:
            return j.get<SubscribeTradingStatus>();
        case Aid::SetReportData:
            return j.get<SetReportData>();
        case Aid::Ratio:
            return j.get<Ratio>();
        case Aid::Heartbeat:
            return Heartbeat{};
        case Aid::Terminate:
            return Terminate{};
        case Aid::RtnData:
            return j.get<RtnData>();
        case Aid::RtnBrokers:
            return j.get<RtnBrokers>();
        case Aid::RspLogin:
            return j.get<RspLogin>();
        case Aid::RspQuotes:
            return j.get<RspQuotes>();
        case Aid::Unknown:
            return std::unexpected(
                ParseError{fmt::format("unknown TQ aid: {}", j.value("aid", std::string{}))});
        }
    } catch (const std::exception& e) {
        return std::unexpected(ParseError{e.what()});
    }
    return std::unexpected(ParseError{"unknown TQ aid"});
}

} // namespace tqsdk::tq

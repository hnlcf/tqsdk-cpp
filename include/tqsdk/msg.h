#pragma once

#include "tqsdk/pch.h"
#include "tqsdk/util/serde.h"

namespace tqsdk {
namespace msg {
namespace detail {

using json = ::nlohmann::json;

// 字段描述：名字 + 成员指针 + 是否可选
template <typename Owner, typename V>
struct Field {
    const char* name;
    V Owner::* ptr;
    bool       optional = false;
};

template <typename Owner, typename V>
constexpr Field<Owner, V> field(const char* name, V Owner::* p, bool opt = false) {
    return {name, p, opt};
}

// ---------- 序列化 ----------
template <typename Msg, typename Tuple, std::size_t... I>
void to_json_fields(json& j, const Msg& m, const Tuple& t, std::index_sequence<I...>) {
    ((j[std::get<I>(t).name] = m.*(std::get<I>(t).ptr)), ...);
}

template <typename Msg>
void message_to_json(json& j, const Msg& m) {
    auto t   = Msg::fields();
    j        = json::object(); // 清空，避免残留
    j["aid"] = Msg::aid;       // 自动注入判别字段
    to_json_fields(j, m, t, std::make_index_sequence<std::tuple_size_v<decltype(t)>>{});
}

// ---------- 反序列化 ----------
template <typename Msg, typename V>
void read_field(const json& j, Msg& m, const Field<Msg, V>& f) {
    if (!j.contains(f.name)) {
        if (!f.optional) {
            throw std::out_of_range(std::string("missing field: ") + f.name);
        }
        return; // 可选字段缺失则跳过
    }
    j.at(f.name).get_to(m.*(f.ptr));
}

template <typename Msg, typename Tuple, std::size_t... I>
void from_json_fields(const json& j, Msg& m, const Tuple& t, std::index_sequence<I...>) {
    (read_field(j, m, std::get<I>(t)), ...);
}

template <typename Msg>
void message_from_json(const json& j, Msg& m) {
    if (!j.is_object()) {
        throw std::invalid_argument(std::string(Msg::aid) + ": message must be object");
    }

    auto it = j.find("aid");
    if (it == j.end() || !it->is_string()) {
        throw std::invalid_argument("missing or invalid field: aid");
    }
    if (it->get<std::string>() != Msg::aid) // 防止类型错配
    {
        throw std::invalid_argument(std::string("aid mismatch: expected ") + Msg::aid + ", got " +
                                    it->get<std::string>());
    }

    auto t = Msg::fields();
    from_json_fields(j, m, t, std::make_index_sequence<std::tuple_size_v<decltype(t)>>{});
}

} // namespace detail

struct SetChart {
    static constexpr const char* aid = "set_chart";

    std::string chart_id;
    std::string ins_list;
    int64_t     duration   = 0;
    int         view_width = 0;

    static auto fields() {
        return std::make_tuple(detail::field("chart_id", &SetChart::chart_id),
                               detail::field("ins_list", &SetChart::ins_list),
                               detail::field("duration", &SetChart::duration),
                               detail::field("view_width", &SetChart::view_width));
    }
};

struct SubscribeQuote {
    static constexpr const char* aid = "subscribe_quote";

    std::string ins_list;

    static auto fields() {
        return std::make_tuple(detail::field("ins_list", &SubscribeQuote::ins_list));
    }
};

struct ReqLogin {
    static constexpr const char* aid = "req_login";

    std::string bid;
    std::string user_name;
    std::string password;

    static auto fields() {
        return std::make_tuple(detail::field("bid", &ReqLogin::bid),
                               detail::field("user_name", &ReqLogin::user_name),
                               detail::field("password", &ReqLogin::password));
    }
};

} // namespace msg
} // namespace tqsdk

// ---------- 生成 adl_serializer 特化 ----------
#define DEFINE_JSON_MESSAGE(MsgType)                                                               \
    namespace nlohmann {                                                                           \
    template <>                                                                                    \
    struct adl_serializer<MsgType> {                                                               \
        static void to_json(json& j, const MsgType& m) {                                           \
            ::tqsdk::msg::detail::message_to_json(j, m);                                           \
        }                                                                                          \
        static void from_json(const json& j, MsgType& m) {                                         \
            ::tqsdk::msg::detail::message_from_json(j, m);                                         \
        }                                                                                          \
    };                                                                                             \
    }

DEFINE_JSON_MESSAGE(tqsdk::msg::SetChart)
DEFINE_JSON_MESSAGE(tqsdk::msg::SubscribeQuote)
DEFINE_JSON_MESSAGE(tqsdk::msg::ReqLogin)

#pragma once

#include "tqsdk/pch.h"
#include "tqsdk/serve/serve_error.h"

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <cerrno>
#include <charconv>
#include <cstdlib>

// ============================================================================
// 轻量类型化路由层（axum 风格，构建在 Boost.Beast 之上）
// ----------------------------------------------------------------------------
// 目标：把"解析路径 / 取 body / 反序列化参数 / 组装响应"从业务里剥出去，
// 业务只需写一个 handler，参数按**类型**自动提取：
//
//   Router r;
//   r.get("/quote/{symbol}",
//       [&](std::string symbol) -> asio::awaitable<json> {
//           co_return quote_to_json(api.get_quote(symbol));
//       });
//   r.post("/order",
//       [&](InsertOrderReq body) -> asio::awaitable<json> {
//           auto* o = co_await api.insert_order(body);
//           co_return json{{"order_id", o->order_id}};
//       });
//   auto res = co_await r.dispatch(req);
//
// 参数提取规则（按 handler 形参类型，从左到右）：
//   - 算术类型(int/uint/int64/double...) / std::string  -> 依次取一个**路径参数**
//     （`{name}` 段；自动 URL 解码；算术用 std::from_chars，失败 -> 400）。
//   - Query<T>                                         -> 解析 ?a=1&b=2 后 get<T>()。
//   - 其它任意类型 T（需可 json.get<T>()）             -> 整个请求体 JSON 反序列化。
//
// 返回值约定（handler 必须返回 asio::awaitable<T>，保持与全项目协程风格一致）：
//   - T == json       -> 200 + JSON body
//   - T == Response   -> 直接采用其 status/body（可自定义状态码、头部）
//   - T == void       -> 204 No Content
//   抛 HttpError{status, body} -> 对应错误响应；其它异常 -> 500。
//
// 设计取舍：
//   - 不改动事件循环：全程 asio::awaitable，与现有单 io_context 模型一致，零新依赖。
//   - handler 形参类型通过 lambda 的 operator() 签名静态推导（无需注册 arity）。
//   - 路径参数按位置消费，顺序与形参中"路径类参数"出现顺序一致。
// ============================================================================

namespace tqsdk {
namespace serve {

namespace beast = boost::beast;
namespace http  = boost::beast::http;
namespace asio  = boost::asio;
using json      = nlohmann::json;

/// handler 返回此类型可自定义状态码与响应头。
struct Response {
    http::status status = http::status::ok;
    json         body   = json::object();
    // 可选附加头（如 Location / Set-Cookie）。
    std::vector<std::pair<std::string, std::string>> headers;
};

/// 兼容旧调用的路由错误别名；新代码可直接使用 ServeError。
using HttpError = ServeError;

/// 查询字符串提取器：形参写 Query<MyStruct> 即可把 ?a=1&b=x 映射成结构体。
template <typename>
inline constexpr bool is_expected_v = false;
template <typename T, typename E>
inline constexpr bool is_expected_v<std::expected<T, E>> = true;

template <typename T>
struct Query {
    using value_type = T;
    T value{};
};

// ----------------------------------------------------------------------------
// 内部实现
// ----------------------------------------------------------------------------
namespace detail {

// 取 lambda/functor 的 operator() 形参类型元组。
template <typename F>
struct fn_traits;
template <typename R, typename C, typename... A>
struct fn_traits<R (C::*)(A...)> {
    using args = std::tuple<A...>;
};
template <typename R, typename C, typename... A>
struct fn_traits<R (C::*)(A...) const> {
    using args = std::tuple<A...>;
};
template <typename R, typename C, typename... A>
struct fn_traits<R (C::*)(A...) noexcept> {
    using args = std::tuple<A...>;
};
template <typename R, typename C, typename... A>
struct fn_traits<R (C::*)(A...) const noexcept> {
    using args = std::tuple<A...>;
};

template <typename F>
using handler_args_t = typename fn_traits<decltype(&F::operator())>::args;

// 是否为 Query<...> 包装。
template <typename T>
struct is_query : std::false_type {};
template <typename T>
struct is_query<Query<T>> : std::true_type {};

// URL 百分号解码（'+' -> 空格）。
inline std::string url_decode(std::string_view s) {
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        return -1;
    };
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int hi = hex(s[i + 1]), lo = hex(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(s[i] == '+' ? ' ' : s[i]);
    }
    return out;
}

// 解析查询串为 json 对象（值以字符串存；数字/布尔在 get<T> 前做一次尝试）。
inline json parse_query(std::string_view target) {
    json q   = json::object();
    auto pos = target.find('?');
    if (pos == std::string_view::npos || pos + 1 >= target.size()) {
        return q;
    }
    std::string_view qs = target.substr(pos + 1);
    std::size_t      i  = 0;
    while (i < qs.size()) {
        std::size_t amp = qs.find('&', i);
        if (amp == std::string_view::npos) {
            amp = qs.size();
        }
        std::string_view kv = qs.substr(i, amp - i);
        auto             eq = kv.find('=');
        if (eq != std::string_view::npos) {
            std::string k = url_decode(kv.substr(0, eq));
            std::string v = url_decode(kv.substr(eq + 1));
            q[k]          = v;
        }
        i = amp + 1;
    }
    return q;
}

// 路径分段（去查询串、忽略空段）。
inline std::vector<std::string> split_path(std::string_view target) {
    auto q = target.find('?');
    if (q != std::string_view::npos) {
        target = target.substr(0, q);
    }
    std::vector<std::string> out;
    std::size_t              i = 0;
    while (i < target.size()) {
        while (i < target.size() && target[i] == '/') {
            ++i;
        }
        std::size_t j = i;
        while (j < target.size() && target[j] != '/') {
            ++j;
        }
        if (j > i) {
            out.emplace_back(target.substr(i, j - i));
        }
        i = j;
    }
    return out;
}

// 单次请求的处理上下文：携带请求、已捕获的路径参数、游标、惰性解析的 body。
struct RouteContext {
    const http::request<http::string_body>* req = nullptr;
    std::vector<std::string>                path_params; // 已按模式捕获
    std::size_t                             cursor = 0;  // 下一个待消费的路径参数
    std::optional<json>                     parsed_body; // 惰性解析缓存

    const json& body() {
        if (!parsed_body) {
            if (req->body().empty()) {
                parsed_body = json::object();
            } else {
                try {
                    parsed_body = json::parse(req->body());
                } catch (const std::exception& e) {
                    throw HttpError{http::status::bad_request,
                                    json{{"error", std::string("invalid json: ") + e.what()}}};
                }
            }
        }
        return *parsed_body;
    }
};

// 从路径段解析一个算术类型。
template <typename T>
T parse_path_number(std::string_view s) {
    if constexpr (std::is_integral_v<T>) {
        T value{};
        auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
        if (ec != std::errc{} || ptr != s.data() + s.size()) {
            throw HttpError{http::status::bad_request,
                            json{{"error", "invalid path parameter: " + std::string(s)}}};
        }
        return value;
    } else {
        std::string tmp(s);
        char*       end = nullptr;
        errno           = 0;
        double v        = std::strtod(tmp.c_str(), &end);
        if (end != tmp.c_str() + tmp.size() || errno == ERANGE) {
            throw HttpError{http::status::bad_request,
                            json{{"error", "invalid path parameter: " + tmp}}};
        }
        return static_cast<T>(v);
    }
}

// 单个形参的提取器（按类型分派）。
template <typename Arg>
Arg extract_arg(RouteContext& ctx) {
    if constexpr (is_query<std::remove_cvref_t<Arg>>::value) {
        using Inner    = typename std::remove_cvref_t<Arg>::value_type;
        json         q = parse_query(ctx.req->target());
        Query<Inner> out{};
        try {
            out.value = q.get<Inner>();
        } catch (const std::exception& e) {
            throw HttpError{http::status::bad_request,
                            json{{"error", std::string("bad query: ") + e.what()}}};
        }
        return out;
    } else if constexpr (std::is_same_v<std::remove_cvref_t<Arg>, std::string>) {
        if (ctx.cursor >= ctx.path_params.size()) {
            throw HttpError{http::status::bad_request, json{{"error", "missing path parameter"}}};
        }
        return url_decode(ctx.path_params[ctx.cursor++]);
    } else if constexpr (std::is_arithmetic_v<std::remove_cvref_t<Arg>>) {
        if (ctx.cursor >= ctx.path_params.size()) {
            throw HttpError{http::status::bad_request, json{{"error", "missing path parameter"}}};
        }
        return parse_path_number<std::remove_cvref_t<Arg>>(ctx.path_params[ctx.cursor++]);
    } else {
        try {
            return ctx.body().get<std::remove_cvref_t<Arg>>();
        } catch (const HttpError&) {
            throw;
        } catch (const std::exception& e) {
            throw HttpError{http::status::bad_request,
                            json{{"error", std::string("bad request body: ") + e.what()}}};
        }
    }
}

// 把 handler 返回值转成 Response。
template <typename T>
Response to_response(T&& v) {
    using U = std::remove_cvref_t<T>;
    if constexpr (std::is_same_v<U, Response>) {
        return std::forward<T>(v);
    } else {
        Response r;
        r.status = http::status::ok;
        r.body   = json(std::forward<T>(v));
        return r;
    }
}
inline Response to_response() { // void -> 204
    Response r;
    r.status = http::status::no_content;
    r.body   = json::object();
    return r;
}

// 展开元组提取形参并调用 handler，返回 Response。
template <typename F, typename... A, std::size_t... I>
asio::awaitable<Response>
call_extracted(F& f, RouteContext& ctx, std::tuple<A...>*, std::index_sequence<I...>) {
    // 列表初始化保证从左到右求值，故路径参数按形参出现顺序消费。
    std::tuple<std::remove_cvref_t<A>...> args{extract_arg<std::remove_cvref_t<A>>(ctx)...};
    // handler 必须返回 asio::awaitable<R>；从其 value_type 取 R（避免在未求值上下
    // 文中使用 co_await，部分编译器不允许）。
    using Ret = decltype(std::apply(std::declval<F&>(), std::move(args)));
    using R   = typename Ret::value_type;
    if constexpr (is_expected_v<R>) {
        auto result = co_await std::apply(f, std::move(args));
        if (!result) {
            Response out;
            out.status = result.error().status;
            out.body   = result.error().body;
            co_return out;
        }
        co_return to_response(*std::move(result));
    } else if constexpr (std::is_void_v<R>) {
        co_await std::apply(f, std::move(args));
        co_return to_response();
    } else {
        R&& ret = co_await std::apply(f, std::move(args));
        co_return to_response<R>(std::forward<R>(ret));
    }
}

template <typename F>
asio::awaitable<Response> invoke(F& f, RouteContext& ctx) {
    using Args = handler_args_t<F>;
    co_return co_await call_extracted(
        f, ctx, static_cast<Args*>(nullptr), std::make_index_sequence<std::tuple_size_v<Args>>{});
}

// 一个已路由条目：把具体 handler 类型擦除成统一签名的可调用体。
using Entry = std::function<asio::awaitable<Response>(RouteContext&)>;

} // namespace detail

/// 把 handler 包装成类型擦除后的路由条目（供 Router::add 的表形式使用）。
/// 例：{ http::verb::get, "/quote/{symbol}", make_entry([this](std::string s){...}) }
template <typename F>
detail::Entry make_entry(F handler) {
    return
        [h = std::move(handler)](detail::RouteContext& ctx) mutable -> asio::awaitable<Response> {
            co_return co_await detail::invoke(h, ctx);
        };
}

// ----------------------------------------------------------------------------
// Router
// ----------------------------------------------------------------------------
/// 类型化路由器：声明式注册路由，dispatch 时按形参类型自动提取参数。
class Router {
public:
    /// 一条路由定义（供 add() 的表形式使用）。
    struct RouteDef {
        http::verb    method;
        std::string   pattern;
        detail::Entry entry;
    };

    /// 以"路由表"形式一次性注册多条路由：
    ///   router_.add({
    ///       {http::verb::get,  "/quote/{symbol}", make_entry([this](std::string s){...})},
    ///       {http::verb::post, "/order",          make_entry([this](InsertOrderReq r){...})},
    ///   });
    void add(std::initializer_list<RouteDef> defs) {
        for (const auto& d : defs) {
            add_row(d.method, d.pattern, d.entry);
        }
    }

    /// 注册一条路由（便捷写法）。pattern 形如 "/quote/{symbol}"，`{name}` 为路径参数
    /// （名字仅作文档用途，实际按位置与形参对应）。handler 必须返回 asio::awaitable<T>。
    template <typename F>
    void route(http::verb method, std::string_view pattern, F handler) {
        add_row(method, pattern, make_entry(std::move(handler)));
    }

    template <typename F>
    void get(std::string_view pattern, F h) {
        route(http::verb::get, pattern, std::move(h));
    }
    template <typename F>
    void post(std::string_view pattern, F h) {
        route(http::verb::post, pattern, std::move(h));
    }
    template <typename F>
    void put(std::string_view pattern, F h) {
        route(http::verb::put, pattern, std::move(h));
    }
    template <typename F>
    void del(std::string_view pattern, F h) {
        route(http::verb::delete_, pattern, std::move(h));
    }

    /// 派发请求：命中路由 -> 调 handler；未命中 -> 404；handler 抛错 -> 对应状态码。
    asio::awaitable<Response> dispatch(const http::request<http::string_body>& req) {
        const auto segs = detail::split_path(req.target());
        for (auto& r : routes_) {
            if (r.method != req.method()) {
                continue;
            }
            if (r.segments.size() != segs.size()) {
                continue;
            }
            std::vector<std::string> captured;
            bool                     ok = true;
            for (std::size_t i = 0; i < segs.size(); ++i) {
                if (r.is_var[i]) {
                    captured.push_back(segs[i]);
                } else if (r.segments[i] != segs[i]) {
                    ok = false;
                    break;
                }
            }
            if (!ok) {
                continue;
            }

            detail::RouteContext ctx;
            ctx.req         = &req;
            ctx.path_params = std::move(captured);
            try {
                co_return co_await r.entry(ctx);
            } catch (const HttpError& e) {
                Response out;
                out.status = e.status;
                out.body   = e.body;
                co_return out;
            } catch (const std::exception& e) {
                Response out;
                out.status = http::status::internal_server_error;
                out.body   = json{{"error", e.what()}};
                co_return out;
            }
        }
        Response out;
        out.status = http::status::not_found;
        out.body   = json{{"error", "not found"}};
        co_return out;
    }

    bool empty() const noexcept { return routes_.empty(); }

private:
    void add_row(http::verb method, std::string_view pattern, detail::Entry entry) {
        Route r;
        r.method   = method;
        r.segments = detail::split_path(pattern);
        r.is_var.reserve(r.segments.size());
        for (auto& seg : r.segments) {
            bool var = seg.size() >= 2 && seg.front() == '{' && seg.back() == '}';
            r.is_var.push_back(var);
        }
        r.entry = std::move(entry);
        routes_.push_back(std::move(r));
    }

    struct Route {
        http::verb               method;
        std::vector<std::string> segments; // 每段原文（literal 或 "{var}"）
        std::vector<bool>        is_var;   // 该段是否为变量
        detail::Entry            entry;
    };
    std::vector<Route> routes_;
};

} // namespace serve
} // namespace tqsdk

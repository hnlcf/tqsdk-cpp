#include "tqsdk/pch.h"

#include "http_route_handlers.h"

#include "tqsdk/serve/json_convert.h"

namespace tqsdk {
namespace serve {

asio::awaitable<ServeResult<json>>
HttpServer::RouteHandlers::target_create(Query<AccountQuery> query, TargetTaskRequest request) {
    if (request.symbol.empty()) {
        co_return make_serve_error(http::status::bad_request,
                                   json{{"error", "symbol is required"}});
    }
    AccountScope scope    = account_scope(query.value.account);
    TqApi*       selected = scope.api;
    auto&        tasks    = *scope.tasks;
    selected->get_quote(request.symbol);
    TargetPosPriceMode mode = TargetPosPriceMode::ACTIVE;
    if (request.mode == "PASSIVE") {
        mode = TargetPosPriceMode::PASSIVE;
    } else if (request.mode == "CUSTOM") {
        co_return make_serve_error(http::status::bad_request,
                                   json{{"error", "CUSTOM mode requires a C++ price_provider"}});
    } else if (request.mode != "ACTIVE") {
        co_return make_serve_error(http::status::bad_request, json{{"error", "unknown task mode"}});
    }
    TargetPosOptions options;
    options.max_volume = request.max_volume;
    const auto id      = fmt::format("target-{}", ++tasks.seq);
    auto       task =
        std::make_shared<TargetPosTask>(*selected, request.symbol, mode, std::move(options));
    task->set_target_volume(request.target_volume);
    tasks.targets[id] = std::move(task);
    co_return json{{"success", true},
                   {"task_id", id},
                   {"account", scope.id},
                   {"symbol", request.symbol},
                   {"target_volume", request.target_volume}};
}

asio::awaitable<ServeResult<json>>
HttpServer::RouteHandlers::target_delete(std::string task_id, Query<AccountQuery> query) {
    AccountScope scope = account_scope(query.value.account);
    auto&        tasks = *scope.tasks;
    auto         it    = tasks.targets.find(task_id);
    if (it == tasks.targets.end()) {
        co_return make_serve_error(http::status::not_found,
                                   json{{"error", "target task not found"}});
    }
    it->second->cancel();
    tasks.targets.erase(it);
    co_return json{{"success", true}, {"task_id", task_id}, {"account", scope.id}};
}

asio::awaitable<ServeResult<json>>
HttpServer::RouteHandlers::order_task_create(Query<AccountQuery> query, OrderTaskRequest request) {
    if (request.order.instrument_id.empty() || request.order.volume == 0) {
        co_return make_serve_error(http::status::bad_request,
                                   json{{"error", "invalid order task"}});
    }
    AccountScope scope    = account_scope(query.value.account);
    TqApi*       selected = scope.api;
    auto&        tasks    = *scope.tasks;
    const auto   id       = fmt::format("order-{}", ++tasks.seq);
    auto         task     = std::make_shared<InsertOrderTask>(*selected, std::move(request.order));
    tasks.orders[id]      = std::move(task);
    co_return json{{"success", true}, {"task_id", id}, {"account", scope.id}};
}

asio::awaitable<ServeResult<json>>
HttpServer::RouteHandlers::order_task_delete(std::string task_id, Query<AccountQuery> query) {
    AccountScope scope = account_scope(query.value.account);
    auto&        tasks = *scope.tasks;
    auto         it    = tasks.orders.find(task_id);
    if (it == tasks.orders.end()) {
        co_return make_serve_error(http::status::not_found,
                                   json{{"error", "order task not found"}});
    }
    it->second->cancel();
    tasks.orders.erase(it);
    co_return json{{"success", true}, {"task_id", task_id}, {"account", scope.id}};
}

asio::awaitable<ServeResult<json>> HttpServer::RouteHandlers::tasks(Query<AccountQuery> query) {
    AccountScope scope = account_scope(query.value.account);
    auto&        tasks = *scope.tasks;

    json targets = json::array();
    for (const auto& [id, task] : tasks.targets) {
        targets.push_back(json{{"task_id", id},
                               {"account", scope.id},
                               {"active", task->active()},
                               {"target_volume", task->target_volume()}});
    }
    json orders = json::array();
    for (const auto& [id, task] : tasks.orders) {
        orders.push_back(json{{"task_id", id},
                              {"account", scope.id},
                              {"active", task->active()},
                              {"filled_volume", task->filled_volume()}});
    }
    co_return json{
        {"account", scope.id}, {"targets", std::move(targets)}, {"orders", std::move(orders)}};
}

asio::awaitable<ServeResult<json>>
HttpServer::RouteHandlers::risk_status(Query<AccountQuery> query) {
    TqApi* selected = account_scope(query.value.account).api;
    co_return json{{"rules", selected->risk_manager().size()}};
}

asio::awaitable<ServeResult<json>>
HttpServer::RouteHandlers::risk_clear(Query<AccountQuery> query) {
    TqApi* selected = account_scope(query.value.account).api;
    selected->risk_manager().clear();
    co_return json{{"success", true}, {"rules", 0}};
}

asio::awaitable<ServeResult<json>> HttpServer::RouteHandlers::risk_add(Query<AccountQuery> query,
                                                                       RiskRuleRequest request) {
    TqApi*                    selected = account_scope(query.value.account).api;
    std::shared_ptr<RiskRule> rule;
    if (request.type == "OPEN_COUNT_LIMIT") {
        rule = std::make_shared<OpenCountsLimitRule>(request.limit, request.symbols);
    } else if (request.type == "OPEN_VOLUME_LIMIT") {
        rule = std::make_shared<OpenVolumesLimitRule>(request.limit, request.symbols);
    } else if (request.type == "ORDER_RATE_LIMIT") {
        rule = std::make_shared<OrderRateLimitRule>(request.limit,
                                                    std::chrono::milliseconds(request.window_ms));
    } else {
        co_return make_serve_error(http::status::bad_request, json{{"error", "unknown risk rule"}});
    }
    selected->risk_manager().add(rule);
    co_return json{
        {"success", true}, {"type", request.type}, {"rules", selected->risk_manager().size()}};
}

} // namespace serve
} // namespace tqsdk

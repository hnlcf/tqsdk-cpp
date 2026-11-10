#include "tqsdk/pch.h"

#include "http_route_handlers.h"

#include "tqsdk/serve/json_convert.h"
#include "tqsdk/serve/snapshot_service.h"

namespace tqsdk {
namespace serve {

asio::awaitable<json> HttpServer::RouteHandlers::trading_snapshot(Query<AccountQuery> query) {
    TqApi* selected = account_api(query.value.account);
    co_return ::tqsdk::serve::trading_snapshot(*selected);
}

asio::awaitable<json> HttpServer::RouteHandlers::account(Query<AccountQuery> query) {
    TqApi* selected = account_api(query.value.account);
    co_return account_to_json(selected->get_account());
}

asio::awaitable<json> HttpServer::RouteHandlers::orders(Query<AccountQuery> query) {
    TqApi* selected = account_api(query.value.account);
    co_return orders_snapshot(*selected);
}

asio::awaitable<json> HttpServer::RouteHandlers::trades(Query<AccountQuery> query) {
    TqApi* selected = account_api(query.value.account);
    co_return trades_snapshot(*selected);
}

asio::awaitable<json> HttpServer::RouteHandlers::positions(Query<AccountQuery> query) {
    TqApi* selected = account_api(query.value.account);
    co_return positions_snapshot(*selected);
}

asio::awaitable<json> HttpServer::RouteHandlers::insert_order(Query<AccountQuery> query,
                                                              InsertOrderReq      request) {
    TqApi*     selected = account_api(query.value.account);
    Order*     order    = co_await selected->insert_order(request);
    const bool success  = order != nullptr && !order->is_error;
    co_return json{{"success", success},
                   {"order_id", order ? order->order_id : std::string{}},
                   {"error", order ? order->last_msg : std::string{"order rejected"}}};
}

asio::awaitable<json> HttpServer::RouteHandlers::cancel_order(std::string         order_id,
                                                              Query<AccountQuery> query) {
    TqApi*     selected = account_api(query.value.account);
    CommandAck ack      = co_await selected->cancel_order(CancelOrderReq{order_id, ""});
    co_return json{{"success", ack.success}, {"order_id", ack.order_id}, {"error", ack.error_msg}};
}

} // namespace serve
} // namespace tqsdk

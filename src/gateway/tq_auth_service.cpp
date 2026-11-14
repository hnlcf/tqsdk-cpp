#include "tqsdk/gateway/tq_auth_service.h"

#include "tqsdk/auth/tq_auth.h"

namespace tqsdk {

using json = nlohmann::json;

TqAuthService::TqAuthService(std::shared_ptr<TqAuth> auth, asio::any_io_executor ex)
    : auth_(std::move(auth)) {
    (void)ex;
}

TqAuthService::~TqAuthService() {
    worker_.join();
}

asio::awaitable<ipc::ServiceResponse> TqAuthService::handle(const ipc::ServiceRequest& request) {
    // co_spawn 到专属 worker；TqAuth 的同步网络调用不会占用 gateway_ex。
    co_return co_await asio::co_spawn(
        worker_,
        [this, request]() -> asio::awaitable<ipc::ServiceResponse> {
            co_return handle_sync(request);
        },
        asio::use_awaitable);
}

ipc::ServiceResponse TqAuthService::handle_sync(const ipc::ServiceRequest& request) {
    ipc::ServiceResponse response;
    response.rid = request.rid;

    if (!auth_) {
        response.error = "TqAuth is not configured";
        return response;
    }

    if (request.method == "snapshot") {
        const auto& cfg = auth_->config();
        std::string authorization;
        std::string auth_id;
        std::string product_type;
        int         expire_days = 0;
        if (cfg.enabled) {
            authorization = auth_->authorization_header();
            auth_id       = auth_->auth_id();
            product_type  = auth_->product_type();
            expire_days   = auth_->expire_days_left();
        }
        response.result = json{
            {"enabled", cfg.enabled},
            {"logged_in", auth_->logged_in()},
            {"user_name", cfg.user_name},
            {"user_agent", auth_->user_agent()},
            {"authorization", std::move(authorization)},
            {"auth_id", std::move(auth_id)},
            {"product_type", std::move(product_type)},
            {"expire_days", expire_days},
            {"grants", cfg.enabled ? auth_->grants_json() : std::string{}},
            {"last_error", auth_->last_error()},
        };
        response.ok = true;
        return response;
    }

    if (request.method == "login") {
        auth_->init(request.params.value("mode", std::string("real")));
        const auto result = auth_->login_result();
        if (!result) {
            response.error = result.error().detail;
            return response;
        }
        response.ok = true;
        return response;
    }

    if (request.method == "refresh") {
        if (!auth_->refresh()) {
            response.error = auth_->last_error();
            return response;
        }
        response.ok = true;
        return response;
    }

    if (request.method == "md_url") {
        auto url = auth_->get_md_url(request.params.value("stock", false),
                                     request.params.value("backtest", false));
        if (!url) {
            response.error = auth_->last_error();
            return response;
        }
        response.result = json{{"url", *url}};
        response.ok     = true;
        return response;
    }

    if (request.method == "td_url") {
        const auto broker_name = request.params.value("broker_name", std::string{});
        const auto account_id  = request.params.value("account_id", std::string{});
        auto       url         = auth_->get_td_url(broker_name, account_id);
        if (!url) {
            response.error = auth_->last_error();
            return response;
        }
        response.result = json{{"url", url->url},
                               {"broker_type", url->broker_type},
                               {"smtype", url->smtype},
                               {"smconfig", url->smconfig}};
        response.ok     = true;
        return response;
    }

    if (request.method == "add_account") {
        const auto account_id = request.params.value("account_id", std::string{});
        if (!auth_->add_account(account_id)) {
            response.error = auth_->last_error();
            return response;
        }
        response.ok = true;
        return response;
    }

    response.error = fmt::format("unknown auth method '{}'", request.method);
    return response;
}

} // namespace tqsdk

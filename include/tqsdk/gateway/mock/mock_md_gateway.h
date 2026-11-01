#pragma once

#include "tqsdk/pch.h"

#include "tqsdk/gateway/market_data_gateway.h"
#include "tqsdk/util/spawn_logged.h"

namespace tqsdk {

/// 脚本化行情网关：测试用 inject() 注入 diff，走与真网关完全相同的 output() 通道。
class MockMdGateway : public MarketDataGateway {
public:
    explicit MockMdGateway(asio::any_io_executor ex)
        : ex_(ex),
          ch_(ex) {}

    // 测试脚本调用：注入一条 diff（异步投递到通道，遵循 Channel 契约）。
    void inject(nlohmann::json diff) {
        MarketDataEvent ev{MarketDataEvent::Kind::Diff, std::move(diff), {}};
        co_spawn_logged(
            ex_,
            [this, e = std::move(ev)]() mutable -> asio::awaitable<void> {
                co_await ch_.async_send(std::move(e));
            },
            "MockMd.inject");
    }

    void inject_error(std::string msg) {
        MarketDataEvent ev{MarketDataEvent::Kind::Error, nlohmann::json{}, std::move(msg)};
        co_spawn_logged(
            ex_,
            [this, e = std::move(ev)]() mutable -> asio::awaitable<void> {
                co_await ch_.async_send(std::move(e));
            },
            "MockMd.inject_error");
    }

    asio::awaitable<void> start() override { co_return; }

    asio::awaitable<void> stop() override {
        ch_.close();
        co_return;
    }

    asio::awaitable<void> subscribe(const std::vector<std::string>& symbols) override {
        for (const auto& s : symbols) {
            subscribed_.insert(s);
        }
        co_return;
    }

    Channel<MarketDataEvent>& output() override { return ch_; }

    bool is_subscribed(std::string_view sym) const {
        return subscribed_.count(std::string(sym)) > 0;
    }

private:
    asio::any_io_executor           ex_;
    Channel<MarketDataEvent>        ch_;
    std::unordered_set<std::string> subscribed_;
};

} // namespace tqsdk

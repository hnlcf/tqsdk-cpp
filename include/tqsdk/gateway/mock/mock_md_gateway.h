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
            ++subscribe_events_;
        }
        co_return;
    }

    asio::awaitable<void>
    subscribe_kline(const std::string& symbol, int64_t duration_ns, int view_width) override {
        subscribed_klines_.insert(std::make_pair(symbol, duration_ns));
        kline_view_widths_[std::make_pair(symbol, duration_ns)] = view_width;
        co_return;
    }

    asio::awaitable<void>
    release(const std::vector<std::string>&                     symbols,
            const std::vector<std::pair<std::string, int64_t>>& klines) override {
        for (const auto& s : symbols) {
            subscribed_.erase(s);
            released_.insert(s);
            ++release_events_;
        }
        for (const auto& k : klines) {
            subscribed_klines_.erase(k);
            released_klines_.insert(k);
            ++release_events_;
        }
        co_return;
    }

    Channel<MarketDataEvent>& output() override { return ch_; }

    bool is_subscribed(std::string_view sym) const {
        return subscribed_.count(std::string(sym)) > 0;
    }

    bool is_kline_subscribed(std::string_view sym, int64_t duration_ns) const {
        return subscribed_klines_.count(std::make_pair(std::string(sym), duration_ns)) > 0;
    }

    int kline_view_width(std::string_view sym, int64_t duration_ns) const {
        auto it = kline_view_widths_.find(std::make_pair(std::string(sym), duration_ns));
        return it == kline_view_widths_.end() ? 0 : it->second;
    }

    bool was_released(std::string_view sym) const { return released_.count(std::string(sym)) > 0; }

    bool was_kline_released(std::string_view sym, int64_t duration_ns) const {
        return released_klines_.count(std::make_pair(std::string(sym), duration_ns)) > 0;
    }

    int subscribe_events() const noexcept { return subscribe_events_; }
    int release_events() const noexcept { return release_events_; }

private:
    asio::any_io_executor                          ex_;
    Channel<MarketDataEvent>                       ch_;
    std::unordered_set<std::string>                subscribed_;
    std::set<std::pair<std::string, int64_t>>      subscribed_klines_;
    std::map<std::pair<std::string, int64_t>, int> kline_view_widths_;
    std::unordered_set<std::string>                released_;
    std::set<std::pair<std::string, int64_t>>      released_klines_;
    int                                            subscribe_events_ = 0;
    int                                            release_events_   = 0;
};

} // namespace tqsdk

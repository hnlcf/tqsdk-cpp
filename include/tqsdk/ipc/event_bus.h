#pragma once

#include "tqsdk/pch.h"

#include "tqsdk/channel/channel.h"
#include "tqsdk/util/spawn_logged.h"

#include <boost/asio/experimental/awaitable_operators.hpp>

namespace tqsdk {
namespace ipc {

/// EventBus —— 单消费者网关输出 -> 多连接 sink 的扇出层。
///
/// 背景：`MarketDataGateway::output()` / `TradingGateway::event_output()` 是
/// **单消费者** Channel。gw 进程同时服务多个 node 连接时，不能每个
/// GatewayIpcServer 直接抢同一个通道（事件会被分摊而不是广播）。
///
/// 本类独占读取源通道，把每条事件以非阻塞 `Channel::try_send()` 投递到每个
/// 连接自己的有界 sink。慢连接只丢自己的最旧事件，不阻塞其他 node。
///
/// 线程/executor 约定：
///   - Bus、源通道、所有 sink 必须在同一 executor/strand 上；
///   - add_sink/remove_sink 只在该 strand 上调用；
///   - sink 的生命周期必须覆盖 remove_sink；GatewayIpcServer 在 close() 中移除。
template <typename T>
class EventBus : public std::enable_shared_from_this<EventBus<T>> {
public:
    EventBus(Channel<T>& source, asio::any_io_executor ex)
        : source_(source),
          ex_(std::move(ex)),
          stop_timer_(ex_) {}

    ~EventBus() { stop(); }

    EventBus(const EventBus&)            = delete;
    EventBus& operator=(const EventBus&) = delete;

    /// sink 用 shared_ptr 持有：remove_sink 后已 post 的事件仍能安全执行。
    /// add/remove 可从任意连接 strand 调用；用 mutex 保护 sink 列表。
    void add_sink(std::shared_ptr<Channel<T>> sink) {
        if (sink) {
            std::lock_guard<std::mutex> lock(sinks_mu_);
            sinks_.push_back(Sink{sink->executor(), std::move(sink)});
        }
    }

    void remove_sink(Channel<T>* sink) {
        std::lock_guard<std::mutex> lock(sinks_mu_);
        std::erase_if(sinks_, [sink](const Sink& s) { return s.channel.get() == sink; });
    }

    void start() {
        if (started_) {
            return;
        }
        started_  = true;
        auto self = this->shared_from_this();
        co_spawn_logged(
            ex_, [self]() -> asio::awaitable<void> { co_await self->run_loop(); }, "EventBus");
    }

    void stop() noexcept {
        if (stopped_.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        stop_timer_.cancel();
    }

private:
    asio::awaitable<void> wait_stop() {
        stop_timer_.expires_at(std::chrono::steady_clock::time_point::max());
        boost::system::error_code ec;
        co_await stop_timer_.async_wait(asio::redirect_error(asio::use_awaitable, ec));
        co_return;
    }

    asio::awaitable<void> run_loop() {
        using namespace boost::asio::experimental::awaitable_operators;

        while (!stopped_.load(std::memory_order_acquire)) {
            std::variant<std::optional<T>, std::monostate> raced;
            try {
                raced = co_await (source_.async_receive() || wait_stop());
            } catch (const boost::system::system_error& e) {
                if (e.code() == asio::error::operation_aborted) {
                    break;
                }
                SPDLOG_WARN("[EventBus] receive failed: {}", e.what());
                break;
            }
            if (std::holds_alternative<std::monostate>(raced)) {
                break; // stop()
            }
            auto item = std::move(std::get<0>(raced));
            if (!item.has_value()) {
                break; // source closed
            }
            std::vector<Sink> snapshot;
            {
                std::lock_guard<std::mutex> lock(sinks_mu_);
                snapshot = sinks_;
            }
            for (const auto& sink : snapshot) {
                // 每个 sink 一份副本，post 到它自己的 executor；
                // try_send 保证只在 sink strand 上修改队列。
                auto copy = std::make_shared<T>(*item);
                asio::post(sink.ex,
                           [ch = sink.channel, copy]() mutable { ch->try_send(std::move(*copy)); });
            }
        }
        co_return;
    }

    struct Sink {
        asio::any_io_executor       ex;
        std::shared_ptr<Channel<T>> channel;
    };

    Channel<T>&           source_;
    asio::any_io_executor ex_;
    asio::steady_timer    stop_timer_;
    std::mutex            sinks_mu_;
    std::vector<Sink>     sinks_;
    bool                  started_{false};
    std::atomic<bool>     stopped_{false};
};

} // namespace ipc
} // namespace tqsdk

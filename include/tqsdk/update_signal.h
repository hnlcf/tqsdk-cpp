// include/tqsdk/update_signal.h
#pragma once

#include "tqsdk/pch.h"

namespace tqsdk {

namespace asio = boost::asio;

// 单线程 io_context 下的"事件"原语：多次 notify 等价于一次（类似 condition_variable 的 notify_all）
class UpdateSignal {
public:
    explicit UpdateSignal(asio::any_io_executor ex)
        : ex_(std::move(ex)) {}

    void notify() noexcept {
        has_signal_ = true;
        if (waiter_) {
            try {
                waiter_->cancel();
            } catch (...) {
            }
            waiter_ = nullptr;
        }
    }

    asio::awaitable<void> wait() {
        if (has_signal_) {
            has_signal_ = false;
            co_return;
        }
        asio::steady_timer t(ex_);
        t.expires_at(std::chrono::steady_clock::time_point::max());
        waiter_ = &t;
        try {
            co_await t.async_wait(asio::use_awaitable);
        } catch (const boost::system::system_error& e) {
            if (e.code() != asio::error::operation_aborted) {
                SPDLOG_WARN("[UpdateSignal] wait failed: {}", e.what());
            }
        }
        waiter_     = nullptr;
        has_signal_ = false;
        co_return;
    }

private:
    asio::any_io_executor ex_;
    bool                  has_signal_ = false;
    asio::steady_timer*   waiter_     = nullptr;
};

} // namespace tqsdk

#pragma once

#include "tqsdk/pch.h"
#include "tqsdk/util/panic.h"

namespace tqsdk {

namespace asio = boost::asio;

/// 极简异步互斥：单线程 io_context 下串行化对同一资源的协程访问。
///
/// Beast 的 websocket::stream 不允许并发的 async_write，多个协程同时写会破坏
/// 帧序列甚至导致连接被服务器断开。所有对 ws 的写操作必须先 lock()。
///
/// 用法：
///   co_await mutex.lock();
///   try { co_await ws_->async_write(...); } catch (...) { mutex.unlock(); throw; }
///   mutex.unlock();
/// 为降低样板，推荐用 AsyncLockGuard（RAII）。
class AsyncMutex {
public:
    explicit AsyncMutex(asio::any_io_executor ex)
        : ex_(std::move(ex)) {}

    bool try_lock() {
        if (!locked_) {
            locked_ = true;
            return true;
        }
        return false;
    }

    asio::awaitable<void> lock() {
        while (locked_) {
            asio::steady_timer t(co_await asio::this_coro::executor);
            t.expires_at(std::chrono::steady_clock::time_point::max());
            waiters_.push_back(&t);
            try {
                co_await t.async_wait(asio::use_awaitable);
            } catch (const boost::system::system_error& e) {
                if (e.code() != asio::error::operation_aborted) {
                    TQSDK_PANIC("AsyncMutex wait failed: {}", e.what());
                }
                // operation_aborted = 被 unlock 唤醒
            }
            // 唤醒后从等待队列摘除（若还在）
            waiters_.remove(&t);
        }
        locked_ = true;
        co_return;
    }

    void unlock() {
        locked_ = false;
        if (!waiters_.empty()) {
            waiters_.front()->cancel();
        }
    }

    /// RAII：析构时自动 unlock。只能 move。
    class Guard {
    public:
        Guard() = default;
        explicit Guard(AsyncMutex& m)
            : m_(&m) {}
        Guard(Guard&& o) noexcept
            : m_(o.m_) {
            o.m_ = nullptr;
        }
        Guard& operator=(Guard&& o) noexcept {
            if (this != &o) {
                release();
                m_   = o.m_;
                o.m_ = nullptr;
            }
            return *this;
        }
        Guard(const Guard&)            = delete;
        Guard& operator=(const Guard&) = delete;
        ~Guard() { release(); }

    private:
        void release() {
            if (m_) {
                m_->unlock();
                m_ = nullptr;
            }
        }
        AsyncMutex* m_ = nullptr;
    };

    /// 加锁并返回 RAII guard。
    static asio::awaitable<Guard> acquire(AsyncMutex& m) {
        co_await m.lock();
        co_return Guard(m);
    }

private:
    asio::any_io_executor          ex_;
    bool                           locked_ = false;
    std::list<asio::steady_timer*> waiters_;
};

} // namespace tqsdk

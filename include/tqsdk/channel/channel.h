#pragma once

#include "tqsdk/pch.h"
#include "tqsdk/util/panic.h"

namespace tqsdk {

namespace asio = boost::asio;

template <typename T>
class Channel {
public:
    // capacity == 0 表示无界；capacity > 0 表示有界，满了 send 会阻塞
    explicit Channel(asio::any_io_executor ex, std::size_t capacity = 0);

    Channel(const Channel&)            = delete;
    Channel& operator=(const Channel&) = delete;
    Channel(Channel&&)                 = delete;
    Channel& operator=(Channel&&)      = delete;

    ~Channel();

    // 生产者：入队；有界满时挂起直到有空间或 close
    // close 后抛 system_error{operation_aborted}
    asio::awaitable<void> async_send(T item);

    // 非阻塞生产者：仅允许在 Channel 所属 executor 上调用。
    // 有界且已满时丢弃最旧元素（行情广播的“慢消费者不拖垮其他连接”语义）。
    // 返回 false 表示通道已关闭。
    bool try_send(T item);

    // 跨线程生产者：post 到 Channel 所属 executor 后 try_send。
    // 调用方必须保证 Channel 在 posted handler 执行前存活。
    void post_send(T item);

    // 跨线程关闭：post 到所属 executor 执行 close()。
    void post_close();

    // 消费者：取队首；空且未 close 时挂起
    // close 且空时返回 nullopt
    asio::awaitable<std::optional<T>> async_receive();

    // 关闭：唤醒所有挂起的 send/receive，之后再 send 抛异常，再 receive 得到 nullopt
    void close() noexcept;

    bool                  is_closed() const noexcept { return closed_; }
    std::size_t           size() const noexcept { return queue_.size(); }
    asio::any_io_executor executor() const { return ex_; }
    std::size_t           capacity() const noexcept { return capacity_; }

private:
    // 挂起者的身份：timer 挂在栈上，Guard 负责注册/反注册
    struct Waiter {
        asio::steady_timer           timer;
        std::list<Waiter*>::iterator self;

        explicit Waiter(const asio::any_io_executor& ex)
            : timer(ex) {
            timer.expires_at(std::chrono::steady_clock::time_point::max());
        }
    };

    struct WaiterEraseGuard {
        std::list<Waiter*>&                   list;
        typename std::list<Waiter*>::iterator it;

        ~WaiterEraseGuard() { list.erase(it); }
    };

    void wake_one(std::list<Waiter*>& list) noexcept;

    asio::any_io_executor ex_;
    std::size_t           capacity_;
    std::deque<T>         queue_;
    std::list<Waiter*>    recv_waiters_;
    std::list<Waiter*>    send_waiters_;
    bool                  closed_{false};
};

template <typename T>
Channel<T>::Channel(asio::any_io_executor ex, std::size_t capacity)
    : ex_(std::move(ex)),
      capacity_(capacity) {}

template <typename T>
Channel<T>::~Channel() {
    // 契约：析构时不应有挂起的协程；这是不可恢复的生命周期错误。
    if (!recv_waiters_.empty() || !send_waiters_.empty()) {
        TQSDK_PANIC("Channel destroyed with pending awaiters");
    }
}

template <typename T>
void Channel<T>::wake_one(std::list<Waiter*>& list) noexcept {
    if (list.empty()) {
        return;
    }

    Waiter* w = list.front();
    try {
        w->timer.cancel(); // 触发 operation_aborted，挂起的协程被恢复
    } catch (...) {
        // cancel 失败不应传播到 Channel 的关闭路径。
    }
}

// ---------- send ----------
template <typename T>
asio::awaitable<void> Channel<T>::async_send(T item) {
    if (closed_) {
        SPDLOG_WARN("Channel::async_send: closed");
        throw boost::system::system_error(asio::error::operation_aborted,
                                          "Channel::async_send: closed");
    }

    // 有界背压
    while (capacity_ > 0 && queue_.size() >= capacity_) {
        Waiter w(co_await asio::this_coro::executor);
        send_waiters_.push_back(&w);
        w.self = std::prev(send_waiters_.end());

        // RAII：无论正常返回还是异常，都从 send_waiters_ 里摘除自己
        WaiterEraseGuard erase_guard{send_waiters_, w.self};

        try {
            co_await w.timer.async_wait(asio::use_awaitable);
            // 若正常返回（无异常），说明被主动唤醒；继续 loop 检查空间
        } catch (const boost::system::system_error& e) {
            if (e.code() != asio::error::operation_aborted) {
                SPDLOG_ERROR("Channel::async_send: unexpected error: {}", e.what());
                throw;
            }

            // operation_aborted 是唤醒信号，继续
        }

        if (closed_) {
            SPDLOG_WARN("Channel::async_send: closed");
            throw boost::system::system_error(asio::error::operation_aborted,
                                              "Channel::async_send: closed");
        }
    }
    SPDLOG_TRACE("Channel::async_send: enqueue item, size: {}", queue_.size() + 1);
    queue_.push_back(std::move(item));

    wake_one(recv_waiters_);

    co_return;
}

// ---------- try_send ----------
template <typename T>
bool Channel<T>::try_send(T item) {
    if (closed_) {
        return false;
    }
    if (capacity_ > 0 && queue_.size() >= capacity_) {
        // 慢消费者：丢弃最旧元素，保留最新状态（行情/交易快照均为最新优先）。
        queue_.pop_front();
    }
    queue_.push_back(std::move(item));
    wake_one(recv_waiters_);
    return true;
}

// ---------- cross-thread post ----------
template <typename T>
void Channel<T>::post_send(T item) {
    asio::post(ex_, [this, item = std::move(item)]() mutable {
        if (!try_send(std::move(item))) {
            SPDLOG_TRACE("Channel::post_send: channel closed");
        }
    });
}

template <typename T>
void Channel<T>::post_close() {
    asio::post(ex_, [this]() { close(); });
}

// ---------- receive ----------
template <typename T>
asio::awaitable<std::optional<T>> Channel<T>::async_receive() {
    SPDLOG_TRACE("Channel::async_receive: size: {}, closed: {}", queue_.size(), closed_);

    // 捕获当前协程的取消状态：当本操作被外部（如 awaitable_operators 的 race、
    // 或上游取消）取消时，timer 的 async_wait 会以 operation_aborted 返回。
    // 我们据 cancellation_state 区分"被主动唤醒(wake_one)"与"被取消"，前者继续
    // 重试，后者必须向上抛出以终止本协程——否则取消永远无法让接收方退出。
    auto cs = co_await asio::this_coro::cancellation_state;

    while (queue_.empty() && !closed_) {
        Waiter w(co_await asio::this_coro::executor);
        recv_waiters_.push_back(&w);
        w.self = std::prev(recv_waiters_.end());

        WaiterEraseGuard erase_guard{recv_waiters_, w.self};

        try {
            co_await w.timer.async_wait(asio::use_awaitable);
        } catch (const boost::system::system_error& e) {
            if (e.code() != asio::error::operation_aborted) {
                SPDLOG_ERROR("Channel::async_receive: unexpected error: {}", e.what());
                throw;
            }
            // 区分取消 vs 普通唤醒
            if (cs.cancelled() != asio::cancellation_type::none) {
                SPDLOG_DEBUG("Channel::async_receive: cancelled");
                throw;
            }
            SPDLOG_DEBUG(
                "Channel::async_receive: woken up, size: {}, closed: {}", queue_.size(), closed_);
        }
    }

    if (queue_.empty()) {
        // closed 且空
        SPDLOG_WARN("Channel::async_receive: empty, closed: {}", closed_);
        co_return std::nullopt;
    }

    T item = std::move(queue_.front());
    queue_.pop_front();

    // 有界：唤醒一个等待空间的生产者
    wake_one(send_waiters_);

    co_return std::optional<T>{std::move(item)};
}

// ---------- close ----------
template <typename T>
void Channel<T>::close() noexcept {
    SPDLOG_DEBUG("Channel::close: closed: {}", closed_);
    if (closed_) {
        return;
    }
    closed_ = true;

    // 唤醒所有等待者；它们会分别在协程内自查 closed_ 并退出
    for (Waiter* w : recv_waiters_) {
        try {
            w->timer.cancel();
        } catch (...) {
        }
    }
    for (Waiter* w : send_waiters_) {
        try {
            w->timer.cancel();
        } catch (...) {
        }
    }
    // 注意：不 clear 列表；erase 由协程恢复后各自负责
}

} // namespace tqsdk

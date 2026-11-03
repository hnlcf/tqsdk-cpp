#include "tqsdk/gateway/ctp/ctp_task_queue.h"

namespace asio = boost::asio;

namespace tqsdk {

CtpTaskQueue::CtpTaskQueue(asio::any_io_executor ex)
    : ex_(std::move(ex)) {}

void CtpTaskQueue::push(std::function<void()> fn) {
    enqueue(std::move(fn), false);
}

void CtpTaskQueue::push_high(std::function<void()> fn) {
    enqueue(std::move(fn), true);
}

void CtpTaskQueue::enqueue(std::function<void()> fn, bool high) {
    if (!fn || stopped_.load(std::memory_order_acquire)) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (stopped_.load(std::memory_order_relaxed)) {
            return;
        }
        if (high) {
            high_queue_.push_back(std::move(fn));
        } else {
            queue_.push_back(std::move(fn));
        }
    }
    schedule();
}

void CtpTaskQueue::schedule() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (scheduled_ || (queue_.empty() && high_queue_.empty())) {
            return;
        }
        scheduled_ = true;
    }
    auto self = shared_from_this();
    boost::asio::post(ex_, [self]() { self->drain(); });
}

void CtpTaskQueue::drain() {
    // CTP 的全量合约查询会一次产生上万条回调；限制单次 drain 批量，
    // 定期让出 core strand，避免报单/撤单/心跳等协程被回调洪峰饿死。
    constexpr std::size_t kMaxBatch  = 256;
    std::size_t           dispatched = 0;

    for (;;) {
        if (stopped_.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lock(mu_);
            scheduled_ = false;
            return;
        }

        std::function<void()> fn;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (queue_.empty() && high_queue_.empty()) {
                scheduled_ = false;
                return;
            }
            auto& source = high_queue_.empty() ? queue_ : high_queue_;
            fn           = std::move(source.front());
            source.pop_front();
        }

        try {
            fn();
        } catch (const std::exception& e) {
            SPDLOG_ERROR("[CtpTaskQueue] task exception: {}", e.what());
        } catch (...) {
            SPDLOG_ERROR("[CtpTaskQueue] unknown task exception");
        }

        if (++dispatched >= kMaxBatch) {
            bool more = false;
            {
                std::lock_guard<std::mutex> lock(mu_);
                more       = !queue_.empty() || !high_queue_.empty();
                scheduled_ = false;
            }
            if (more) {
                schedule();
            }
            return;
        }
    }
}

void CtpTaskQueue::stop() noexcept {
    if (stopped_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    std::lock_guard<std::mutex> lock(mu_);
    queue_.clear();
    high_queue_.clear();
    scheduled_ = false;
}

} // namespace tqsdk

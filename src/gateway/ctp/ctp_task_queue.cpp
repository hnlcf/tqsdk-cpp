#include "tqsdk/gateway/ctp/ctp_task_queue.h"

namespace asio = boost::asio;

namespace tqsdk {

CtpTaskQueue::CtpTaskQueue(asio::any_io_executor ex)
    : ex_(std::move(ex)) {}

void CtpTaskQueue::push(std::function<void()> fn) {
    if (!fn || stopped_.load(std::memory_order_acquire)) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (stopped_.load(std::memory_order_relaxed)) {
            return;
        }
        queue_.push_back(std::move(fn));
    }
    schedule();
}

void CtpTaskQueue::schedule() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (scheduled_ || queue_.empty()) {
            return;
        }
        scheduled_ = true;
    }
    auto self = shared_from_this();
    boost::asio::post(ex_, [self]() { self->drain(); });
}

void CtpTaskQueue::drain() {
    for (;;) {
        if (stopped_.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lock(mu_);
            scheduled_ = false;
            return;
        }

        std::function<void()> fn;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (queue_.empty()) {
                scheduled_ = false;
                return;
            }
            fn = std::move(queue_.front());
            queue_.pop_front();
        }

        try {
            fn();
        } catch (const std::exception& e) {
            SPDLOG_ERROR("[CtpTaskQueue] task exception: {}", e.what());
        } catch (...) {
            SPDLOG_ERROR("[CtpTaskQueue] unknown task exception");
        }
    }
}

void CtpTaskQueue::stop() noexcept {
    if (stopped_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    std::lock_guard<std::mutex> lock(mu_);
    queue_.clear();
    scheduled_ = false;
}

} // namespace tqsdk

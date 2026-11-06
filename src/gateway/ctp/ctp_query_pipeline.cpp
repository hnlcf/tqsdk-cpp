#include "tqsdk/gateway/ctp/ctp_query_pipeline.h"

#include <spdlog/spdlog.h>

namespace tqsdk::ctp {

CtpQueryPipeline::CtpQueryPipeline(boost::asio::any_io_executor ex,
                                   Callbacks                    callbacks,
                                   std::chrono::milliseconds    query_timeout)
    : ex_(std::move(ex)),
      callbacks_(std::move(callbacks)),
      query_timeout_(query_timeout),
      pump_timer_(ex_),
      restart_timer_(ex_),
      watchdog_(ex_) {}

CtpQueryPipeline::~CtpQueryPipeline() {
    reset();
}

void CtpQueryPipeline::start(bool initial, std::vector<Job> jobs) {
    if (running_) {
        if (!initial) {
            restart_pending_ = true;
        }
        return;
    }

    ++generation_;
    cancel_timers();
    active_.reset();
    running_         = true;
    initial_         = initial;
    waiting_         = false;
    restart_pending_ = false;
    queue_.clear();
    for (auto& job : jobs) {
        queue_.push_back(std::move(job));
    }
    pending_ = static_cast<int>(queue_.size());
    if (pending_ == 0) {
        complete();
        return;
    }
    if (initial) {
        snapshot_ready_.store(false, std::memory_order_release);
        SPDLOG_INFO("[CtpQuery] initial snapshot: {} request(s)", pending_);
    } else {
        SPDLOG_DEBUG("[CtpQuery] refresh snapshot: {} request(s)", pending_);
    }
    pump();
}

void CtpQueryPipeline::enqueue_specs(const std::vector<std::string>& symbols) {
    if (!running_ || symbols.empty()) {
        return;
    }
    for (const auto& symbol : symbols) {
        queue_.push_back(Job{Kind::InstrumentSymbol, symbol});
    }
    pending_ += static_cast<int>(symbols.size());
    if (running_ && !waiting_) {
        pump();
    }
}

void CtpQueryPipeline::finish() {
    if (!running_) {
        return;
    }
    active_.reset();
    watchdog_.cancel();
    waiting_ = false;
    if (pending_ > 0) {
        --pending_;
    }
    if (pending_ == 0) {
        complete();
    } else {
        schedule_pump(std::chrono::milliseconds(300));
    }
}

void CtpQueryPipeline::fail(Kind kind) {
    if (!running_) {
        return;
    }
    if (callbacks_.failed) {
        callbacks_.failed(kind);
    }
    active_.reset();
    watchdog_.cancel();
    waiting_ = false;
    if (pending_ > 0) {
        --pending_;
    }
    if (pending_ == 0) {
        complete();
    } else {
        schedule_pump(std::chrono::milliseconds(300));
    }
}

void CtpQueryPipeline::reset() {
    ++generation_;
    running_         = false;
    initial_         = false;
    waiting_         = false;
    restart_pending_ = false;
    active_.reset();
    queue_.clear();
    pending_ = 0;
    cancel_timers();
}

void CtpQueryPipeline::schedule_restart(std::chrono::milliseconds delay) {
    if (running_) {
        restart_pending_ = true;
        return;
    }
    const uint64_t gen = generation_;
    restart_timer_.expires_after(delay);
    restart_timer_.async_wait([this, gen](boost::system::error_code ec) {
        if (ec || running_ || gen != generation_) {
            return;
        }
        if (callbacks_.restart) {
            callbacks_.restart();
        }
    });
}

bool CtpQueryPipeline::active_matches(Kind kind, int request_id) const noexcept {
    return active_.has_value() && active_->kind == kind && active_->request_id == request_id;
}

void CtpQueryPipeline::pump() {
    if (!running_ || waiting_ || queue_.empty()) {
        return;
    }

    Job job = queue_.front();
    queue_.pop_front();
    const int ret = callbacks_.send ? callbacks_.send(job) : -1;
    SPDLOG_DEBUG(
        "[CtpQuery] send kind={} req={} ret={}", static_cast<int>(job.kind), job.request_id, ret);
    if (ret == 0) {
        waiting_ = true;
        active_  = Active{job.kind, job.symbol, job.request_id};
        arm_watchdog();
        return;
    }

    // -2/-3: CTP 未处理请求超限/每秒请求超限，退避后重排队。
    if ((ret == -2 || ret == -3) && job.attempts < 8) {
        ++job.attempts;
        SPDLOG_WARN("[CtpQuery] kind={} throttled ret={}, retry {}/8",
                    static_cast<int>(job.kind),
                    ret,
                    job.attempts);
        queue_.push_front(std::move(job));
        schedule_pump(std::chrono::seconds(1));
        return;
    }

    SPDLOG_WARN("[CtpQuery] kind={} send failed ret={}", static_cast<int>(job.kind), ret);
    fail(job.kind);
}

void CtpQueryPipeline::schedule_pump(std::chrono::milliseconds delay) {
    if (!running_) {
        return;
    }
    const uint64_t gen = generation_;
    pump_timer_.expires_after(delay);
    pump_timer_.async_wait([this, gen](boost::system::error_code ec) {
        if (!ec && running_ && gen == generation_) {
            pump();
        }
    });
}

void CtpQueryPipeline::arm_watchdog() {
    if (!active_ || !running_) {
        return;
    }
    const uint64_t gen  = generation_;
    const int      req  = active_->request_id;
    const Kind     kind = active_->kind;
    watchdog_.expires_after(query_timeout_);
    watchdog_.async_wait([this, gen, req, kind](boost::system::error_code ec) {
        if (ec || !running_ || gen != generation_) {
            return;
        }
        if (!active_ || active_->request_id != req) {
            return;
        }
        SPDLOG_WARN("[CtpQuery] kind={} req={} timeout; skip", static_cast<int>(kind), req);
        active_.reset();
        waiting_ = false;
        fail(kind);
    });
}

void CtpQueryPipeline::complete() {
    const bool was_initial = initial_;
    if (was_initial) {
        SPDLOG_INFO("[CtpQuery] initial snapshot complete");
    } else {
        SPDLOG_DEBUG("[CtpQuery] refresh snapshot complete");
    }
    running_ = false;
    initial_ = false;
    waiting_ = false;
    active_.reset();
    queue_.clear();
    snapshot_ready_.store(true, std::memory_order_release);

    if (callbacks_.completed) {
        callbacks_.completed(was_initial);
    }
    if (restart_pending_) {
        restart_pending_ = false;
        schedule_restart();
    }
}

void CtpQueryPipeline::cancel_timers() {
    pump_timer_.cancel();
    restart_timer_.cancel();
    watchdog_.cancel();
}

} // namespace tqsdk::ctp

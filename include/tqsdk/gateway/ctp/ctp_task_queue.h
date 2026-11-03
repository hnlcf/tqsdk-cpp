#pragma once

#include "tqsdk/pch.h"

namespace tqsdk {

/// CTP SDK 回调线程 -> Asio executor 的任务队列。
///
/// CTP SPI 回调在 SDK 自管线程触发，不能直接修改网关/Channel 状态。
/// SPI 只把“拷贝后的 CTP 结构 + 回调”push 到这里，由 Asio core strand 串行执行。
class CtpTaskQueue : public std::enable_shared_from_this<CtpTaskQueue> {
public:
    explicit CtpTaskQueue(boost::asio::any_io_executor ex);

    CtpTaskQueue(const CtpTaskQueue&)            = delete;
    CtpTaskQueue& operator=(const CtpTaskQueue&) = delete;

    /// 任意线程可调用。异常在 drain 中捕获并记录，不向 CTP 线程抛出。
    void push(std::function<void()> fn);

    /// 高优先级任务（报单/成交/风控回报）优先于查询回调洪峰执行。
    void push_high(std::function<void()> fn);

    /// 丢弃尚未执行的任务；已 post 的 drain 会自行退出。
    void stop() noexcept;

    bool stopped() const noexcept { return stopped_.load(std::memory_order_acquire); }

private:
    void schedule();
    void drain();
    void enqueue(std::function<void()> fn, bool high);

    boost::asio::any_io_executor      ex_;
    std::mutex                        mu_;
    std::deque<std::function<void()>> queue_;
    std::deque<std::function<void()>> high_queue_;
    bool                              scheduled_ = false;
    std::atomic<bool>                 stopped_{false};
};

} // namespace tqsdk

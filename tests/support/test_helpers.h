#include "tqsdk/pch.h"

#pragma once

namespace tqsdk::test {

// 同步跑完一个协程；若协程抛异常，rethrow
inline void run_coro(boost::asio::io_context&     ctx,
                     boost::asio::awaitable<void> awa,
                     std::chrono::milliseconds    timeout = std::chrono::seconds(5)) {
    ctx.restart();
    std::exception_ptr eptr;
    bool               done = false;
    boost::asio::co_spawn(ctx, std::move(awa), [&](std::exception_ptr ep) {
        eptr = ep;
        done = true;
    });

    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
            break;
        ctx.restart();
        ctx.run_for(std::min<std::chrono::steady_clock::duration>(deadline - now,
                                                                  std::chrono::milliseconds(50)));
    }

    if (eptr)
        std::rethrow_exception(eptr);
    EXPECT_TRUE(done) << "coroutine timed out after " << timeout.count() << "ms";
}

// 让 ctx 跑一小段，用于"驱动挂起/恢复的时序"
inline void pump(boost::asio::io_context& ctx, int ms = 10) {
    ctx.restart();
    ctx.run_for(std::chrono::milliseconds(ms));
}

// 判断协程是否挂起：lambda 返回 "还在跑" 时可用
inline void
expect_pending(boost::asio::io_context& ctx, std::function<bool()> is_done, int ms = 30) {
    pump(ctx, ms);
    EXPECT_FALSE(is_done()) << "expected coroutine still pending";
}

} // namespace tqsdk::test

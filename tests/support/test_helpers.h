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

// 启动一个协程并返回可等待其结束的句柄（替代 asio::use_future）。
//
// 背景：asio::use_future 在部分 Boost/编译器组合下（如 Boost 1.92 + clang）
// 处理 void(exception_ptr) 完成签名时存在 promise_handler 生命周期缺陷，
// 会在 ASan 下崩溃。这里用显式完成处理函数 + 共享状态，
// 行为等价但完全受控，跨编译器稳定。
//
// 用法（传入返回 awaitable<void> 的可调用对象，与 co_spawn 一致）：
//   auto h = spawn_joined(io, [&]() -> awaitable<void> { ... });
//   io.run();                                 // 必须先跑完（会排干所有任务）
//   h.get();                                  // 协程抛异常则在此 rethrow
class JoinedSpawn {
public:
    template <typename Fn>
    JoinedSpawn(boost::asio::io_context& ctx, Fn&& fn) {
        boost::asio::co_spawn(ctx, std::forward<Fn>(fn), [st = state_](std::exception_ptr ep) {
            st->eptr = ep;
            st->done = true;
        });
    }

    // 等待完成（调方须先跑完 io_context）；异常在此 rethrow。
    void get() {
        EXPECT_TRUE(state_->done) << "spawned coroutine did not complete";
        if (state_->eptr) {
            std::rethrow_exception(state_->eptr);
        }
    }

private:
    struct State {
        std::exception_ptr eptr;
        bool               done = false;
    };
    std::shared_ptr<State> state_ = std::make_shared<State>();
};

// 接受 co_spawn 风格的可调用对象（lambda 返回 awaitable<void>）。
template <typename Fn>
JoinedSpawn spawn_joined(boost::asio::io_context& ctx, Fn&& fn) {
    return JoinedSpawn(ctx, std::forward<Fn>(fn));
}

} // namespace tqsdk::test

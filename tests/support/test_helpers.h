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

    // 是否已完成（不 rethrow、不触发失败）。用于"边驱动边检测"的循环。
    bool is_done() const noexcept { return state_->done; }

    // 若已完成且带异常则 rethrow；未完成不失败。
    void rethrow_if_error() {
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

// 边驱动 io_context 边等待谓词成立（或超时），返回是否在超时前成立。
//
// 为什么需要它：IpcSession 这类常驻对象的后台协程（读循环/心跳/看门狗）会长期
// 持有 pending 的 timer / socket 读，使 io_context 永远不会自己排空，
// 因此绝不能用无参 io.run() 来"等某个操作完成"——会永远阻塞。
// 只能分片 run_for，并在每片之间检查条件。
template <typename Pred>
bool drive_until(boost::asio::io_context&  ctx,
                 Pred&&                    pred,
                 std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return false;
        }
        ctx.restart();
        ctx.run_for(std::min<std::chrono::steady_clock::duration>(deadline - now,
                                                                  std::chrono::milliseconds(20)));
    }
    return true;
}

// 驱动 ctx 运行指定时长（不等待任何条件）。
inline void drive_for(boost::asio::io_context&  ctx,
                      std::chrono::milliseconds total = std::chrono::milliseconds(50)) {
    ctx.restart();
    ctx.run_for(total);
}

// 等待一组 JoinedSpawn 全部完成（同时驱动 ctx）。对常驻后台协程安全。
template <typename... Spawns>
void wait_all(boost::asio::io_context& ctx, std::chrono::milliseconds timeout, Spawns&... spawns) {
    auto all_done = [&]() { return (spawns.is_done() && ...); };
    drive_until(ctx, all_done, timeout);
}

} // namespace tqsdk::test

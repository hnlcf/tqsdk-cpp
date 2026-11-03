#pragma once

#include "tqsdk/pch.h"

namespace tqsdk {

/// 进程级运行参数（[runtime] 段）。
struct RuntimeConfig {
    /// io_context 工作线程数。1 表示单线程（默认）。
    /// 数据面组件统一绑定到 core strand；worker 线程数 > 1 时
    /// 传输层（HTTP accept/连接读写）可并行，core 仍严格串行。
    int threads = 1;
};

inline int normalize_worker_threads(int n) noexcept {
    if (n < 1) {
        return 1;
    }
    if (n > 64) {
        return 64;
    }
    return n;
}

/// 用 threads 个工作线程运行 io_context，直到 io.stop() 或自然排空。
/// 所有线程共享同一个 io_context；调用方需保证状态组件绑定到同一个 strand。
inline void run_io_context(asio::io_context& io, int threads) {
    const int                workers = normalize_worker_threads(threads);
    std::vector<std::thread> pool;
    pool.reserve(static_cast<std::size_t>(workers - 1));
    for (int i = 1; i < workers; ++i) {
        pool.emplace_back([&io]() {
            try {
                io.run();
            } catch (const std::exception& e) {
                SPDLOG_ERROR("[runtime] io worker exception: {}", e.what());
            }
        });
    }
    try {
        io.run();
    } catch (const std::exception& e) {
        SPDLOG_ERROR("[runtime] io main exception: {}", e.what());
    }
    for (auto& t : pool) {
        t.join();
    }
}

} // namespace tqsdk

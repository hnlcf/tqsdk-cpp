// TqSdk-Cpp 行情示例：连接天勤免费行情服务器，实时打印盘口与 K 线。
//
// 用途：交易日真实验证 quote / kline 链路。
// 构建：cmake -DTQSDK_BUILD_EXAMPLES=ON ...
// 运行：
//   ./tqsdk_demo                      # 默认 SHFE.rb2610 60s K 线
//   ./tqsdk_demo SHFE.cu2611 600000000000   # 自定义合约与周期(ns)
//
// 说明：
//   - 免费行情无需登录，服务器会下发匿名 rsp_login。
//   - 每收到一次更新，打印最新价、涨跌、成交量与最新一根 K 线。
//   - 运行约 20 秒或按 Ctrl+C 退出（本示例用定时器自动退出，便于脚本化验证）。

#include "tqsdk/pch.h"

#include "tqsdk/gateway/tq/tq_md_gateway.h"
#include "tqsdk/tqapi.h"
#include "tqsdk/util/log.h"

#include <ctime>

namespace asio = boost::asio;
using namespace tqsdk;

namespace {

std::string fmt_ts(int64_t ns) {
    // 纳秒时间戳 -> "YYYY-MM-DD HH:MM"（仅示例展示用，简化处理）。
    std::time_t secs = static_cast<std::time_t>(ns / 1'000'000'000LL);
    std::tm     tm{};
    gmtime_r(&secs, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
    return buf;
}

// 主协程：订阅 quote + kline，循环打印，直到 stop 信号或超时。
asio::awaitable<void> run(TqApi&               api,
                          std::string          symbol,
                          int64_t              duration_ns,
                          std::chrono::seconds duration,
                          asio::steady_timer&  stop_timer) {
    // 订阅一次：get_quote 返回稳定引用（EntityStore 用 unordered_map，引用跨 rehash 有效），
    // 之后循环里直接读 q，无需重复调用。
    Quote& q     = api.get_quote(symbol);
    auto*  kline = api.get_kline_serial(symbol, duration_ns, 20);

    std::atomic<bool> stopped{false};
    stop_timer.expires_after(duration);
    stop_timer.async_wait([&stopped](const boost::system::error_code& ec) {
        if (!ec) {
            SPDLOG_INFO("[demo] 计时结束，准备退出");
            stopped = true;
        }
    });

    int ticks = 0;
    while (!stopped) {
        SPDLOG_DEBUG("[demo] 等待行情更新...");
        bool got = co_await api.wait_update();
        SPDLOG_DEBUG("[demo] 收到行情更新，got={}", got);

        if (!got) {
            SPDLOG_INFO("[demo] api 已关闭，退出");
            break; // 已关闭
        }
        ++ticks;

        if (std::isfinite(q.last_price)) {
            SPDLOG_INFO("[quote] {} last={} bid1={} ask1={} vol={} turn={}",
                        symbol,
                        q.last_price,
                        q.bid_price1,
                        q.ask_price1,
                        q.volume,
                        q.turnover);
        }

        if (kline && !kline->bars.empty()) {
            const auto& b = kline->bars.back();
            SPDLOG_INFO("[kline] {} {} O={} H={} L={} C={} V={}",
                        symbol,
                        fmt_ts(b.datetime),
                        b.open,
                        b.high,
                        b.low,
                        b.close,
                        b.volume);
        }
    }
    SPDLOG_INFO("[demo] 共收到 {} 次更新，退出", ticks);
    co_return;
}

} // namespace

int main(int argc, char** argv) {
    LogOptions opts;
    opts.level = "trace";
    setup_logging(opts);

    const std::string symbol      = argc > 1 ? argv[1] : "SHFE.rb2610";
    const int64_t     duration_ns = argc > 2 ? std::stoll(argv[2]) : 60'000'000'000LL;

    asio::io_context ctx;

    auto md  = std::make_unique<TqMdGateway>(ctx.get_executor());
    auto api = std::make_unique<TqApi>(ctx.get_executor(), std::move(md), nullptr);
    api->start();

    asio::steady_timer stop_timer(ctx);
    std::string        sym = symbol;

    // 捕获 api 的裸指针供协程使用（api 生命周期覆盖整个 ctx.run()）。
    TqApi* api_ptr = api.get();
    asio::co_spawn(
        ctx,
        [api_ptr, sym, duration_ns, &stop_timer]() -> asio::awaitable<void> {
            co_await run(*api_ptr, sym, duration_ns, std::chrono::seconds(60 * 10), stop_timer);
        },
        [](std::exception_ptr ep) {
            if (ep) {
                try {
                    std::rethrow_exception(ep);
                } catch (const std::exception& e) {
                    SPDLOG_ERROR("[demo] 协程异常：{}", e.what());
                }
            }
        });

    ctx.run();

    // 收尾：先关闭 api，再把它剩下的协程跑完，避免挂起协程访问已析构对象。
    api->close();
    ctx.restart();
    ctx.run();

    return 0;
}

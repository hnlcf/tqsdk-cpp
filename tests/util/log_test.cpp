#include "tqsdk/pch.h"

#include "tqsdk/util/log.h"

#include <spdlog/sinks/ostream_sink.h>

using namespace tqsdk;

// parse_log_level：已知名称映射正确，未知回退 info 且返回 false。
TEST(LogUtil, ParseKnownLevels) {
    EXPECT_EQ(parse_log_level("debug").value(), spdlog::level::debug);
    EXPECT_EQ(parse_log_level("warn").value(), spdlog::level::warn);
    EXPECT_EQ(parse_log_level("warning").value(), spdlog::level::warn);
    EXPECT_EQ(parse_log_level("error").value(), spdlog::level::err);
    EXPECT_EQ(parse_log_level("critical").value(), spdlog::level::critical);
    EXPECT_EQ(parse_log_level("off").value(), spdlog::level::off);
}

TEST(LogUtil, ParseUnknownFallsBackToInfo) {
    EXPECT_FALSE(parse_log_level("bogus"));
    EXPECT_EQ(parse_log_level("info").value(), spdlog::level::info);
}

// setup_logging 应把默认 logger 级别设成请求的级别。
TEST(LogUtil, SetupSetsDefaultLevel) {
    LogOptions opts;
    opts.level = "trace";
    setup_logging(opts);
    EXPECT_EQ(spdlog::default_logger()->level(), spdlog::level::trace);

    // 恢复为测试默认 info，避免影响其它用例。
    LogOptions restore;
    restore.level = "info";
    setup_logging(restore);
    EXPECT_EQ(spdlog::default_logger()->level(), spdlog::level::info);
}

// off 级别下不应产生任何输出（用内存 sink 捕获）。
TEST(LogUtil, OffSuppressesOutput) {
    std::ostringstream oss;
    auto               sink   = std::make_shared<spdlog::sinks::ostream_sink_mt>(oss);
    auto               logger = std::make_shared<spdlog::logger>("t", sink);
    logger->set_level(spdlog::level::off);

    logger->info("should not appear");
    logger->error("neither should this");
    logger->flush();

    EXPECT_TRUE(oss.str().empty());

    // 恢复默认 logger，避免污染后续用例。
    tqsdk::setup_logging(LogOptions{});
}

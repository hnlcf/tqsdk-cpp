#include "tqsdk/pch.h"

#include "tqsdk/util/log.h"

#include <spdlog/sinks/ostream_sink.h>

#include <filesystem>
#include <fstream>

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
TEST(LogUtil, RedactsSecrets) {
    const auto redacted = redact_secrets(
        "password=supersecret token: abc client_secret=\"shh\" Authorization: Bearer xyz");
    EXPECT_EQ(redacted.find("supersecret"), std::string::npos);
    EXPECT_EQ(redacted.find("abc"), std::string::npos);
    EXPECT_EQ(redacted.find("shh"), std::string::npos);
    EXPECT_EQ(redacted.find("xyz"), std::string::npos);
    EXPECT_NE(redacted.find("***"), std::string::npos);
}

TEST(LogUtil, SetupRedactsFileSink) {
    const auto      path = std::filesystem::temp_directory_path() /
                           ("tqsdk_log_redact_" + std::to_string(::getpid()) + ".log");
    std::error_code ec;
    std::filesystem::remove(path, ec);
    LogOptions opts;
    opts.file = path.string();
    setup_logging(opts);
    spdlog::info("password=topsecret token=abc Authorization: Bearer xyz");
    spdlog::default_logger()->flush();
    std::ifstream input(path);
    ASSERT_TRUE(input.good());
    const std::string text((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
    EXPECT_EQ(text.find("topsecret"), std::string::npos);
    EXPECT_EQ(text.find("abc"), std::string::npos);
    EXPECT_EQ(text.find("xyz"), std::string::npos);
    setup_logging(LogOptions{});
    std::filesystem::remove(path, ec);
}

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

#include "tqsdk/pch.h"

#include "tqsdk/util/audit.h"

#include <filesystem>
#include <fstream>

using namespace tqsdk;

TEST(AuditLog, WritesJsonlAndRedactsTokenQuery) {
    const auto      path = std::filesystem::temp_directory_path() /
                           ("tqsdk_audit_" + std::to_string(::getpid()) + ".log");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    {
        audit::Options options;
        options.enabled = true;
        options.file    = path.string();
        audit::Log log(options);
        ASSERT_TRUE(log.enabled());
        log.record("order", "POST", "/order?token=secret-token&account=A", 200, "127.0.0.1:1");
    }

    std::ifstream input(path);
    ASSERT_TRUE(input.good());
    std::string line;
    ASSERT_TRUE(std::getline(input, line));
    EXPECT_EQ(line.find("secret-token"), std::string::npos);
    const auto row = nlohmann::json::parse(line);
    EXPECT_EQ(row["event"], "order");
    EXPECT_EQ(row["method"], "POST");
    EXPECT_EQ(row["status"], 200);
    EXPECT_NE(row["target"].get<std::string>().find("***"), std::string::npos);

    std::filesystem::remove(path, ec);
}

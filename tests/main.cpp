#include "tqsdk/pch.h"

#include "tqsdk/util/log.h"

// 自定义 gtest 入口：在跑测试前初始化统一日志。
// 级别可用环境变量 TQSDK_LOG_LEVEL 覆盖（如 TQSDK_LOG_LEVEL=debug 或 off）。
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);

    tqsdk::LogOptions opts;
    // 测试默认 info；若用户显式设了环境变量则以环境变量为准（setup_logging 内部处理）。
    opts.colored = true;
    tqsdk::setup_logging(opts);

    return RUN_ALL_TESTS();
}

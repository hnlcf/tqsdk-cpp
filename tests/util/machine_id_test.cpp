#include "tqsdk/pch.h"

#include "tqsdk/util/machine_id.h"

#include <gtest/gtest.h>

#include <regex>

TEST(MachineId, MacAddressFormat) {
    const auto mac = tqsdk::util::machine_mac_address();
    if (mac.empty()) {
        GTEST_SKIP() << "machine has no non-loopback MAC";
    }
    EXPECT_TRUE(std::regex_match(mac, std::regex(R"(^[0-9A-F]{2}(-[0-9A-F]{2}){5}$)"))) << mac;
}

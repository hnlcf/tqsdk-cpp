#include "tqsdk/pch.h"

#include "tqsdk/ipc/command_ack_cache.h"

TEST(CommandAckCache, ReusesAndEvicts) {
    tqsdk::ipc::CommandAckCache cache(2);
    tqsdk::CommandAck           ack;
    ack.success  = true;
    ack.order_id = "r1";
    cache.put("insert:r1", ack);
    auto got = cache.find("insert:r1");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->order_id, "r1");

    tqsdk::CommandAck second = ack;
    second.order_id          = "r2";
    cache.put("insert:r2", second);
    EXPECT_EQ(cache.size(), 2u);

    second.order_id = "r3";
    cache.put("insert:r3", second); // evict r1
    EXPECT_FALSE(cache.find("insert:r1").has_value());
    EXPECT_TRUE(cache.find("insert:r2").has_value());
    EXPECT_TRUE(cache.find("insert:r3").has_value());

    cache.clear();
    EXPECT_EQ(cache.size(), 0u);
}

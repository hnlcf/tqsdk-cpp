#include "tqsdk/pch.h"

#include "tqsdk/data/data_manager.h"

#include "support/test_helpers.h"

using namespace tqsdk;

TEST(DataManager, ClearAllChanges) {
    DataManager dm;

    auto& q = dm.quotes.get_or_create("X");
    dm.quotes.mark_changed(q, 0xFF);

    auto& o = dm.orders.get_or_create("oid1");
    dm.orders.mark_changed(o, 0xFF);

    auto& t = dm.trades.get_or_create("tid1");
    dm.trades.mark_changed(t, 0xFF);

    auto& p = dm.positions.get_or_create("X");
    dm.positions.mark_changed(p, 0xFF);

    dm.account.changed_fields = 0xFF;

    auto& k          = dm.klines.get_or_create("X", 60'000'000'000LL, 200);
    k.changed_fields = 1;

    dm.clear_all_changes();

    EXPECT_EQ(q.changed_fields, 0u);
    EXPECT_EQ(o.changed_fields, 0u);
    EXPECT_EQ(t.changed_fields, 0u);
    EXPECT_EQ(p.changed_fields, 0u);
    EXPECT_EQ(dm.account.changed_fields, 0u);
    EXPECT_EQ(k.changed_fields, 0u);
}

TEST(KlineRegistry, DistinctDuration) {
    KlineRegistry kr;
    auto&         k1 = kr.get_or_create("X", 60'000'000'000LL, 200);
    auto&         k2 = kr.get_or_create("X", 300'000'000'000LL, 200);
    EXPECT_NE(&k1, &k2);
    EXPECT_EQ(k1.duration_ns, 60'000'000'000LL);
    EXPECT_EQ(k2.duration_ns, 300'000'000'000LL);
    EXPECT_EQ(kr.size(), 2u);

    EXPECT_EQ(kr.find("X", 60'000'000'000LL), &k1);
    EXPECT_EQ(kr.find("X", 300'000'000'000LL), &k2);
    EXPECT_EQ(kr.find("X", 1LL), nullptr);
}

TEST(KlineRegistry, GetOrCreateReturnsSame) {
    KlineRegistry kr;
    auto&         a = kr.get_or_create("X", 60'000'000'000LL, 200);
    a.bars.push_back({});
    auto& b = kr.get_or_create("X", 60'000'000'000LL, 200);
    EXPECT_EQ(&a, &b);
    EXPECT_EQ(b.bars.size(), 1u);
}

#include "tqsdk/pch.h"

#include "tqsdk/data/entity_store.h"
#include "tqsdk/domain/quote.h"

#include "support/test_helpers.h"

using namespace tqsdk;

TEST(EntityStore, GetOrCreateIdempotent) {
    EntityStore<Quote> s;
    auto&              q1 = s.get_or_create("SHFE.rb2610");
    q1.last_price         = 3800;
    auto& q2              = s.get_or_create("SHFE.rb2610");
    EXPECT_EQ(&q1, &q2);
    EXPECT_EQ(q2.last_price, 3800);
    EXPECT_EQ(s.size(), 1u);
}

TEST(EntityStore, FindReturnsNull) {
    EntityStore<Quote> s;
    EXPECT_EQ(s.find("missing"), nullptr);
    s.get_or_create("X");
    EXPECT_NE(s.find("X"), nullptr);
    const auto& cs = s;
    EXPECT_NE(cs.find("X"), nullptr);
}

TEST(EntityStore, MarkChangedFirstTimeOnly) {
    EntityStore<Quote> s;
    auto&              q = s.get_or_create("X");

    s.mark_changed(q, 1);
    EXPECT_EQ(q.changed_fields, 1u);
    s.mark_changed(q, 2); // 已脏,不入列但 bits 仍累加
    EXPECT_EQ(q.changed_fields, 3u);
    s.mark_changed(q, 4);
    EXPECT_EQ(q.changed_fields, 7u);

    // 只清一次;若 dirty_list_ 有重复,这里会暴露(清完后仍非 0)
    s.clear_changes();
    EXPECT_EQ(q.changed_fields, 0u);
}

TEST(EntityStore, MarkChangedZeroIgnored) {
    EntityStore<Quote> s;
    auto&              q = s.get_or_create("X");
    s.mark_changed(q, 0);
    EXPECT_EQ(q.changed_fields, 0u);
    // dirty_list_ 应仍为空(否则 clear_changes 会访问不该访问的实体)
    s.clear_changes();
    EXPECT_EQ(q.changed_fields, 0u);
}

TEST(EntityStore, ClearOnlyAffectsDirty) {
    EntityStore<Quote> s;
    auto&              a = s.get_or_create("A");
    auto&              b = s.get_or_create("B");
    s.mark_changed(a, 1);
    // b 从未标记
    s.clear_changes();
    EXPECT_EQ(a.changed_fields, 0u);
    EXPECT_EQ(b.changed_fields, 0u);

    // 再次标记 b,仍能正常工作
    s.mark_changed(b, 2);
    EXPECT_EQ(b.changed_fields, 2u);
    s.clear_changes();
    EXPECT_EQ(b.changed_fields, 0u);
}

TEST(EntityStore, Iterate) {
    EntityStore<Quote> s;
    s.get_or_create("A");
    s.get_or_create("B");
    s.get_or_create("C");

    int n = 0;
    for (auto& [k, v] : s) {
        (void)k;
        (void)v;
        ++n;
    }
    EXPECT_EQ(n, 3);
}

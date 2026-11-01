#include "tqsdk/pch.h"

#include "tqsdk/data/diff_merge_engine.h"

#include "support/test_helpers.h"

using namespace tqsdk;
using json = nlohmann::json;

// ---- 便捷构造 ----
static json QP(std::string_view sym, json fields) {
    return json{{"quotes", {{std::string(sym), std::move(fields)}}}};
}

// ===================== Quote =====================

TEST(DiffMerge, QuoteSingleField) {
    DataManager     dm;
    DiffMergeEngine eng(dm);
    eng.apply(QP("SHFE.rb2610", {{"last_price", 3800.0}}));

    auto* q = dm.quotes.find("SHFE.rb2610");
    ASSERT_NE(q, nullptr);
    EXPECT_EQ(q->instrument_id, "SHFE.rb2610");
    EXPECT_EQ(q->last_price, 3800.0);
    EXPECT_EQ(q->changed_fields, (uint64_t)QuoteField::last_price);
}

TEST(DiffMerge, QuoteMultipleFields) {
    DataManager     dm;
    DiffMergeEngine eng(dm);
    eng.apply(
        QP("X", {{"last_price", 3800.0}, {"volume", 42}, {"datetime", "2026-10-22 10:00:00"}}));

    auto* q = dm.quotes.find("X");
    EXPECT_EQ(q->last_price, 3800.0);
    EXPECT_EQ(q->volume, 42u);
    EXPECT_EQ(q->datetime, "2026-10-22 10:00:00");
    EXPECT_EQ(q->changed_fields,
              (uint64_t)QuoteField::last_price | (uint64_t)QuoteField::volume |
                  (uint64_t)QuoteField::datetime);
}

TEST(DiffMerge, QuoteIntegerToDouble) {
    DataManager     dm;
    DiffMergeEngine eng(dm);
    eng.apply(QP("X", {{"last_price", 3800}})); // int 推给 double 字段
    auto* q = dm.quotes.find("X");
    EXPECT_EQ(q->last_price, 3800.0);
}

TEST(DiffMerge, QuoteUnknownFieldIgnored) {
    DataManager     dm;
    DiffMergeEngine eng(dm);
    eng.apply(QP("X", {{"nonexistent", 1}, {"volume", 42}}));
    auto* q = dm.quotes.find("X");
    EXPECT_EQ(q->volume, 42u);
    EXPECT_EQ(q->changed_fields, (uint64_t)QuoteField::volume);
}

TEST(DiffMerge, QuoteIncremental) {
    DataManager     dm;
    DiffMergeEngine eng(dm);
    eng.apply(QP("X", {{"last_price", 3800.0}, {"volume", 42}}));
    dm.clear_all_changes();

    auto* q = dm.quotes.find("X");
    EXPECT_EQ(q->changed_fields, 0u);

    eng.apply(QP("X", {{"last_price", 3801.0}}));
    EXPECT_EQ(q->last_price, 3801.0);
    EXPECT_EQ(q->volume, 42u); // 未重推,保留
    EXPECT_EQ(q->changed_fields, (uint64_t)QuoteField::last_price);
}

// ===================== Order =====================

TEST(DiffMerge, OrderLifecycle) {
    DataManager     dm;
    DiffMergeEngine eng(dm);

    eng.apply(json::parse(R"({
        "trade":{"SHFE.rb2610":{"user":{"orders":{"oid1":{
            "instrument_id":"SHFE.rb2610",
            "direction":"BUY","offset":"OPEN",
            "volume_orign":1,"volume_left":1,
            "status":"ALIVE","is_dead":false
        }}}}}
    })"));

    auto* o = dm.orders.find("oid1");
    ASSERT_NE(o, nullptr);
    EXPECT_EQ(o->order_id, "oid1");
    EXPECT_EQ(o->instrument_id, "SHFE.rb2610");
    EXPECT_EQ(o->direction, Direction::BUY);
    EXPECT_EQ(o->offset, Offset::OPEN);
    EXPECT_EQ(o->status, OrderStatus::ALIVE);
    EXPECT_FALSE(o->is_dead);

    dm.clear_all_changes();

    eng.apply(json::parse(R"({
        "trade":{"SHFE.rb2610":{"user":{"orders":{"oid1":{
            "volume_left":0,"status":"FINISHED","is_dead":true
        }}}}}
    })"));

    EXPECT_EQ(o->status, OrderStatus::FINISHED);
    EXPECT_TRUE(o->is_dead);
    EXPECT_EQ(o->volume_left, 0u);
    EXPECT_EQ(o->changed_fields,
              (uint64_t)OrderField::volume_left | (uint64_t)OrderField::status |
                  (uint64_t)OrderField::is_dead);
}

TEST(DiffMerge, OrderUnknownEnumDoesNotAbort) {
    DataManager     dm;
    DiffMergeEngine eng(dm);
    eng.apply(json::parse(R"({
        "trade":{"X":{"user":{"orders":{"oid1":{
            "direction":"MARTIAN","volume_left":5
        }}}}}
    })"));

    auto* o = dm.orders.find("oid1");
    ASSERT_NE(o, nullptr);
    EXPECT_EQ(o->direction, Direction::BUY); // 默认
    EXPECT_EQ(o->volume_left, 5u);           // 其他字段仍合并
    EXPECT_FALSE(o->changed_fields & (uint64_t)OrderField::direction);
}

// ===================== Trade =====================

TEST(DiffMerge, TradeNewAppend) {
    DataManager     dm;
    DiffMergeEngine eng(dm);
    eng.apply(json::parse(R"({
        "trade":{"SHFE.rb2610":{"user":{"trades":{"tid1":{
            "order_id":"oid1","instrument_id":"SHFE.rb2610",
            "direction":"BUY","offset":"OPEN",
            "volume":1,"price":3800.0,
            "trade_date_time":1700000000000000000
        }}}}}
    })"));

    auto* t = dm.trades.find("tid1");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->trade_id, "tid1");
    EXPECT_EQ(t->order_id, "oid1");
    EXPECT_EQ(t->volume, 1u);
    EXPECT_EQ(t->price, 3800.0);
    EXPECT_EQ(t->trade_date_time, 1700000000000000000ULL);
}

// ===================== Position =====================

TEST(DiffMerge, PositionMerge) {
    DataManager     dm;
    DiffMergeEngine eng(dm);
    eng.apply(json::parse(R"({
        "trade":{"":{"user":{"positions":{"SHFE.rb2610":{
            "volume_long":2,"volume_long_today":1,
            "open_price_long":3800.0,
            "last_price":3810.0,
            "float_profit_long":200.0
        }}}}}
    })"));

    auto* p = dm.positions.find("SHFE.rb2610");
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->instrument_id, "SHFE.rb2610");
    EXPECT_EQ(p->volume_long, 2u);
    EXPECT_EQ(p->volume_long_today, 1u);
    EXPECT_EQ(p->open_price_long, 3800.0);
    EXPECT_EQ(p->last_price, 3810.0);
    EXPECT_EQ(p->float_profit_long, 200.0);
    EXPECT_TRUE(p->changed_fields & (uint64_t)PositionField::volume_long);
    EXPECT_TRUE(p->changed_fields & (uint64_t)PositionField::last_price);
}

// ===================== Account =====================

TEST(DiffMerge, AccountBalance) {
    DataManager     dm;
    DiffMergeEngine eng(dm);
    eng.apply(json::parse(R"({
        "trade":{"":{"user":{"accounts":{"CNY":{
            "balance":1000000.0,"available":800000.0,
            "margin":150000.0,"risk_ratio":0.2
        }}}}}
    })"));

    EXPECT_EQ(dm.account.balance, 1000000.0);
    EXPECT_EQ(dm.account.available, 800000.0);
    EXPECT_EQ(dm.account.margin, 150000.0);
    EXPECT_EQ(dm.account.risk_ratio, 0.2);
    EXPECT_NE(dm.account.changed_fields, 0u);
}

// ===================== Kline =====================

TEST(DiffMerge, KlineUpdateAppend) {
    DataManager     dm;
    DiffMergeEngine eng(dm);

    auto one = [](int64_t dt, double c, double h) {
        return json::parse(std::string(R"({"klines":{"X":{"60000000000":[{"datetime":)") +
                           std::to_string(dt) + R"(,"open":3800,"high":)" + std::to_string(h) +
                           R"(,"low":3790,"close":)" + std::to_string(c) + R"(}]}}})");
    };

    eng.apply(one(192837300000000, 3805, 3810));
    auto* k = dm.klines.find("X", 60000000000LL);
    ASSERT_NE(k, nullptr);
    EXPECT_EQ(k->bars.size(), 1u);
    EXPECT_EQ(k->bars[0].close, 3805);
    EXPECT_TRUE(k->changed_fields & KlineChangeDetail::Appended);

    // 更新最后一根
    eng.apply(one(192837300000000, 3810, 3815));
    EXPECT_EQ(k->bars.size(), 1u);
    EXPECT_EQ(k->bars[0].close, 3810);
    EXPECT_TRUE(k->changed_fields & KlineChangeDetail::LastUpdated);

    // 追加新根
    eng.apply(one(192837400000000, 3815, 3820));
    EXPECT_EQ(k->bars.size(), 2u);
    EXPECT_EQ(k->bars.back().datetime, 192837400000000);
}

// ===================== 边界 =====================

TEST(DiffMerge, EmptyAndMalformed) {
    DataManager     dm;
    DiffMergeEngine eng(dm);
    eng.apply(json::parse("{}"));
    eng.apply(json::parse(R"({"quotes":{}})"));
    eng.apply(json::parse(R"({"unknown":{"a":1}})"));
    eng.apply(json::parse(R"({"quotes":{"X":null}})"));
    eng.apply(json::parse(R"({"quotes":{"X":{"volume":[]}}})"));
    eng.apply(json::parse(R"({"trade":{"X":null}})"));
    eng.apply(json::parse(R"({"klines":{"X":{"bad":[]}}})"));
    // 不崩即通过
}

TEST(DiffMerge, MixedRoutes) {
    DataManager     dm;
    DiffMergeEngine eng(dm);
    eng.apply(json::parse(R"({
        "quotes":{"SHFE.rb2610":{"last_price":3800.0}},
        "trade":{"SHFE.rb2610":{"user":{
            "orders":{"oid1":{"status":"ALIVE","is_dead":false}}
        }}}
    })"));
    EXPECT_NE(dm.quotes.find("SHFE.rb2610"), nullptr);
    EXPECT_NE(dm.orders.find("oid1"), nullptr);
}

TEST(DiffMerge, FullPushIntegration) {
    DataManager     dm;
    DiffMergeEngine eng(dm);
    eng.apply(json::parse(R"({
        "quotes":{"SHFE.rb2610":{"last_price":3800.0,"volume":42}},
        "trade":{"SHFE.rb2610":{"user":{
            "orders":{"oid1":{"symbol":"SHFE.rb2610","status":"ALIVE","is_dead":false}},
            "accounts":{"CNY":{"balance":1000000.0}}
        }}}
    })"));

    EXPECT_EQ(dm.quotes.find("SHFE.rb2610")->last_price, 3800.0);
    EXPECT_EQ(dm.orders.find("oid1")->status, OrderStatus::ALIVE);
    EXPECT_EQ(dm.account.balance, 1000000.0);

    dm.clear_all_changes();
    EXPECT_EQ(dm.quotes.find("SHFE.rb2610")->changed_fields, 0u);
    EXPECT_EQ(dm.orders.find("oid1")->changed_fields, 0u);
    EXPECT_EQ(dm.account.changed_fields, 0u);
}

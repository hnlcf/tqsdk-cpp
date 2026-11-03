#include "tqsdk/pch.h"

#include "tqsdk/gateway/ctp/ctp_order_store.h"

#include <unistd.h>
#include <fstream>

namespace {

std::string test_path(const char* tag) {
    return "/tmp/tqsdk_ctp_store_" + std::string(tag) + "_" + std::to_string(::getpid()) + ".json";
}

tqsdk::ctp::StoredOrder make_order(const std::string& rid, const std::string& ref) {
    tqsdk::ctp::StoredOrder o;
    o.rid           = rid;
    o.ref           = ref;
    o.instrument_id = "rb2701";
    o.exchange_id   = "SHFE";
    o.order_sys_id  = "12345";
    o.status        = "ALIVE";
    o.message       = "未成交";
    o.trading_day   = "20261103";
    o.front_id      = 1;
    o.session_id    = 42;
    o.ack_success   = true;
    o.order_json    = nlohmann::json{{"order_id", rid}, {"instrument_id", "SHFE.rb2701"}};
    return o;
}

} // namespace

TEST(CtpOrderStore, SaveReloadAndFind) {
    const std::string path = test_path("roundtrip");
    ::unlink(path.c_str());

    tqsdk::ctp::CtpOrderStore store(path);
    store.load();
    store.set_trading_day("20261103");
    store.upsert(make_order("rid_a", "1790000001"));
    store.upsert(make_order("rid_b", "1790000002"));

    tqsdk::ctp::CtpOrderStore reloaded(path);
    reloaded.load();
    EXPECT_EQ(reloaded.trading_day(), "20261103");
    EXPECT_EQ(reloaded.orders().size(), 2u);

    tqsdk::ctp::StoredOrder got;
    ASSERT_TRUE(reloaded.find("rid_a", got));
    EXPECT_EQ(got.ref, "1790000001");
    EXPECT_EQ(got.exchange_id, "SHFE");
    EXPECT_EQ(got.front_id, 1);
    EXPECT_TRUE(got.ack_success);
    EXPECT_EQ(got.order_json["instrument_id"], "SHFE.rb2701");
    EXPECT_FALSE(reloaded.find("missing", got));

    ::unlink(path.c_str());
}

TEST(CtpOrderStore, TradingDayRolloverClearsOrders) {
    const std::string path = test_path("rollover");
    ::unlink(path.c_str());

    tqsdk::ctp::CtpOrderStore store(path);
    store.set_trading_day("20261103");
    store.upsert(make_order("rid_a", "1790000001"));
    ASSERT_EQ(store.orders().size(), 1u);

    store.set_trading_day("20261104");
    EXPECT_TRUE(store.orders().empty());

    tqsdk::ctp::CtpOrderStore reloaded(path);
    reloaded.load();
    EXPECT_EQ(reloaded.trading_day(), "20261104");
    EXPECT_TRUE(reloaded.orders().empty());

    ::unlink(path.c_str());
}

TEST(CtpOrderStore, CorruptFileIsIgnored) {
    const std::string path = test_path("corrupt");
    {
        std::ofstream out(path);
        out << "{ this is not json";
    }
    tqsdk::ctp::CtpOrderStore store(path);
    store.load();
    EXPECT_TRUE(store.orders().empty());
    ::unlink(path.c_str());
}

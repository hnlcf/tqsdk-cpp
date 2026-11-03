#include "tqsdk/pch.h"

#include "tqsdk/instrument/instrument_catalog.h"

#include <unistd.h>
#include <fstream>

namespace {

std::string fixture_path() {
    return "/tmp/tqsdk_instrument_catalog_" + std::to_string(::getpid()) + ".json";
}

} // namespace

TEST(InstrumentCatalog, LoadsFuturesOptionsAndProducts) {
    const auto path = fixture_path();
    {
        std::ofstream out(path);
        out << R"({
          "SHFE.rb2701": {"class":"FUTURE","instrument_id":"SHFE.rb2701","exchange_id":"SHFE","ins_id":"rb2701","ins_name":"螺纹钢2701","product_id":"rb","product_short_name":"螺纹钢","volume_multiple":10,"price_tick":1,"price_decs":0,"expired":false},
          "SHFE.rb2701C3000": {"class":"FUTURE_OPTION","instrument_id":"SHFE.rb2701C3000","exchange_id":"SHFE","ins_id":"rb2701C3000","ins_name":"rb2701C3000","product_id":"rbC","product_short_name":"螺纹钢期权","option_class":"CALL","strike_price":3000,"underlying_symbol":"SHFE.rb2701","volume_multiple":10,"price_tick":1,"price_decs":0,"expire_datetime":1800000000,"expired":false},
          "SHFE.rb2701P3000": {"class":"FUTURE_OPTION","instrument_id":"SHFE.rb2701P3000","exchange_id":"SHFE","ins_id":"rb2701P3000","ins_name":"rb2701P3000","product_id":"rbP","product_short_name":"螺纹钢期权","option_class":"PUT","strike_price":3000,"underlying_symbol":"SHFE.rb2701","volume_multiple":10,"price_tick":1,"price_decs":0,"expire_datetime":1800000000,"expired":false},
          "CZCE.SR709": {"class":"FUTURE","instrument_id":"CZCE.SR709","exchange_id":"CZCE","ins_id":"SR709","ins_name":"白糖709","product_id":"SR","expired":false},
          "KQ.m@SHFE.rb": {"class":"FUTURE_CONT","instrument_id":"KQ.m@SHFE.rb","exchange_id":"KQ","ins_id":"m@SHFE.rb","ins_name":"螺纹钢主连","expired":false}
        })";
    }

    tqsdk::InstrumentCatalog::Config cfg;
    cfg.enabled = false;
    cfg.file    = path;
    tqsdk::InstrumentCatalog catalog(cfg);
    ASSERT_TRUE(catalog.load_local());

    auto futures = catalog.list([] {
        tqsdk::InstrumentCatalog::ListFilter f;
        f.classes = "FUTURE,FUTURE_CONT";
        return f;
    }());
    EXPECT_EQ(futures.size(), 3u);

    auto rb = catalog.list([] {
        tqsdk::InstrumentCatalog::ListFilter f;
        f.classes = "FUTURE";
        f.query   = "rb2701";
        return f;
    }());
    ASSERT_EQ(rb.size(), 1u);
    EXPECT_EQ(rb.front().instrument_id, "SHFE.rb2701");

    auto options = catalog.option_contracts("SHFE.rb2701", 10);
    ASSERT_EQ(options.size(), 2u);
    EXPECT_EQ(options[0].option_class, "CALL");
    EXPECT_EQ(options[1].option_class, "PUT");

    auto products = catalog.option_products();
    ASSERT_EQ(products.size(), 1u);
    EXPECT_EQ(products.front().exchange, "SHFE");
    EXPECT_EQ(products.front().product, "SHFE.rb");
    EXPECT_EQ(products.front().calls, 1);
    EXPECT_EQ(products.front().puts, 1);

    EXPECT_EQ(tqsdk::InstrumentCatalog::product_of("SHFE.rb2701"), "SHFE.rb");
    EXPECT_EQ(tqsdk::InstrumentCatalog::product_of("CSI.000300"), "CSI.000300");

    ::unlink(path.c_str());
}

TEST(InstrumentCatalog, MalformedJsonKeepsPreviousSnapshot) {
    const auto path = fixture_path();
    {
        std::ofstream out(path);
        out << R"({"SHFE.rb2701":{"class":"FUTURE","instrument_id":"SHFE.rb2701","expired":false}})";
    }
    tqsdk::InstrumentCatalog::Config cfg;
    cfg.file = path;
    tqsdk::InstrumentCatalog catalog(cfg);
    ASSERT_TRUE(catalog.load_local());
    ASSERT_NE(catalog.find("SHFE.rb2701"), std::nullopt);

    {
        std::ofstream out(path, std::ios::trunc);
        out << R"({"truncated": [})";
    }
    EXPECT_FALSE(catalog.load_local());
    EXPECT_NE(catalog.find("SHFE.rb2701"), std::nullopt) << "failed reload must keep old snapshot";

    {
        std::ofstream out(path, std::ios::trunc);
        out << "[]";
    }
    EXPECT_FALSE(catalog.load_local());
    EXPECT_NE(catalog.find("SHFE.rb2701"), std::nullopt);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(InstrumentCatalog, MixedScalarTypesAndBadEntries) {
    const auto path = fixture_path();
    {
        std::ofstream out(path);
        out << R"({
          "A": {"instrument_id":"A","class":"FUTURE","ins_name":"alpha",
                "volume_multiple":"10","price_tick":"0.5","price_decs":"2",
                "last_price":"3017.5","expired":"false"},
          "B": 42,
          "C": null,
          "D": {"instrument_id":123,"class":"FUTURE","ins_name":"delta",
                "expire_datetime":"1800000000","delivery_month":"12","expired":0},
          "E": {"class":"FUTURE","ins_name":"epsilon","expired":"true"}
        })";
    }
    tqsdk::InstrumentCatalog::Config cfg;
    cfg.file = path;
    tqsdk::InstrumentCatalog catalog(cfg);
    ASSERT_TRUE(catalog.load_local());

    auto a = catalog.find("A");
    ASSERT_TRUE(a.has_value());
    EXPECT_DOUBLE_EQ(a->volume_multiple, 10.0);
    EXPECT_DOUBLE_EQ(a->price_tick, 0.5);
    EXPECT_EQ(a->price_decs, 2);
    EXPECT_DOUBLE_EQ(a->last_price, 3017.5);
    EXPECT_FALSE(a->expired);

    auto d = catalog.find("D");
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->instrument_id, "D");
    EXPECT_DOUBLE_EQ(d->expire_datetime, 1800000000.0);
    EXPECT_EQ(d->delivery_month, 12);
    EXPECT_FALSE(d->expired);

    auto e = catalog.find("E");
    ASSERT_TRUE(e.has_value());
    EXPECT_TRUE(e->expired);

    auto active = catalog.list([] {
        tqsdk::InstrumentCatalog::ListFilter f;
        f.classes = "FUTURE";
        return f;
    }());
    EXPECT_EQ(active.size(), 2u) << "expired E must be filtered by default";

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(InstrumentCatalog, ConcurrentReadDuringReload) {
    const auto path = fixture_path();
    {
        std::ofstream out(path);
        out << R"({"SHFE.rb2701":{"class":"FUTURE","instrument_id":"SHFE.rb2701","expired":false}})";
    }
    tqsdk::InstrumentCatalog::Config cfg;
    cfg.file = path;
    tqsdk::InstrumentCatalog catalog(cfg);
    ASSERT_TRUE(catalog.load_local());

    std::atomic<bool> stop{false};
    std::atomic<int>  errors{0};
    auto              reader = [&] {
        int n = 0;
        while (!stop.load(std::memory_order_relaxed) && n++ < 2000) {
            auto item = catalog.find("SHFE.rb2701");
            if (!item) {
                ++errors;
            }
            auto rows = catalog.list([] {
                tqsdk::InstrumentCatalog::ListFilter f;
                f.classes = "FUTURE";
                return f;
            }());
            if (rows.size() != 1u) {
                ++errors;
            }
        }
    };
    std::thread t1(reader);
    std::thread t2(reader);
    for (int i = 0; i < 50; ++i) {
        if (!catalog.load_local()) {
            ++errors;
        }
    }
    stop = true;
    t1.join();
    t2.join();
    EXPECT_EQ(errors.load(), 0);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

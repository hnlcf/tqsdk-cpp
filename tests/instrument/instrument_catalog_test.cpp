#include "tqsdk/pch.h"

#include "tqsdk/instrument/instrument_catalog.h"

#include <fstream>
#include <unistd.h>

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

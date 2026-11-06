#include "tqsdk/pch.h"

#include "tqsdk/ipc/gateway_subscription_book.h"

using namespace tqsdk::ipc;

TEST(GatewaySubscriptionBook, QuotesAndKlineShareSymbolRefcount) {
    GatewaySubscriptionBook book;
    auto first = book.subscribe({"SHFE.rb2701"}, {{"SHFE.rb2701", 60'000'000'000LL, 200}});
    EXPECT_EQ(first.symbols, std::vector<std::string>{"SHFE.rb2701"});
    EXPECT_EQ(first.klines.size(), 1u);
    EXPECT_TRUE(book.has_symbol("SHFE.rb2701"));
    EXPECT_TRUE(book.has_kline("SHFE.rb2701", 60'000'000'000LL));

    // 重复 explicit quote/kline 不重复增加引用。
    auto duplicate = book.subscribe({"SHFE.rb2701"}, {{"SHFE.rb2701", 60'000'000'000LL, 200}});
    EXPECT_TRUE(duplicate.symbols.empty());
    EXPECT_TRUE(duplicate.klines.empty());

    // 只退显式 quote：K 线依赖仍保留 symbol。
    auto release_quote = book.unsubscribe({"SHFE.rb2701"}, {});
    EXPECT_TRUE(release_quote.symbols.empty());
    EXPECT_TRUE(book.has_symbol("SHFE.rb2701"));

    auto release_kline = book.unsubscribe({}, {{"SHFE.rb2701", 60'000'000'000LL, 200}});
    EXPECT_EQ(release_kline.symbols, std::vector<std::string>{"SHFE.rb2701"});
    EXPECT_FALSE(book.has_symbol("SHFE.rb2701"));
    EXPECT_FALSE(book.has_kline("SHFE.rb2701", 60'000'000'000LL));
}

TEST(GatewaySubscriptionBook, ReleaseAllReturnsEverything) {
    GatewaySubscriptionBook book;
    book.subscribe({"A", "B"}, {{"A", 1, 100}});
    auto all = book.release_all();
    EXPECT_EQ(all.symbols.size(), 2u);
    EXPECT_EQ(all.klines.size(), 1u);
    EXPECT_FALSE(book.has_symbol("A"));
    EXPECT_FALSE(book.has_kline("A", 1));
}

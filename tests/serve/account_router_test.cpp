#include "tqsdk/pch.h"

#include "tqsdk/serve/account_router.h"

#include <gtest/gtest.h>

namespace tqsdk {
namespace serve {
namespace {

TEST(AccountRouter, WithoutManagerKeepsSingleAccountCompatibility) {
    asio::io_context io;
    auto             primary = std::make_shared<TqApi>(io.get_executor(), nullptr, nullptr);
    AccountRouter    router(primary.get(), nullptr);

    auto empty = router.resolve("");
    EXPECT_EQ(empty.id, "default");
    EXPECT_EQ(empty.api, primary.get());

    auto named = router.resolve("custom");
    EXPECT_EQ(named.id, "custom");
    EXPECT_EQ(named.api, primary.get());
    EXPECT_EQ(router.default_id(), "default");
    ASSERT_EQ(router.account_ids().size(), 1u);
    EXPECT_EQ(router.account_ids()[0], "default");
}

TEST(AccountRouter, ResolvesDefaultAndNamedAccounts) {
    asio::io_context io;
    auto             primary = std::make_shared<TqApi>(io.get_executor(), nullptr, nullptr);
    auto             a       = std::make_shared<TqApi>(io.get_executor(), nullptr, nullptr);
    auto             b       = std::make_shared<TqApi>(io.get_executor(), nullptr, nullptr);

    MultiAccountManager manager;
    ASSERT_TRUE(manager.add_account("A", a));
    ASSERT_TRUE(manager.add_account("B", b));

    AccountRouter router(primary.get(), &manager);
    EXPECT_EQ(router.default_id(), "A");

    auto default_scope = router.resolve("");
    EXPECT_EQ(default_scope.id, "A");
    EXPECT_EQ(default_scope.api, a.get());

    auto b_scope = router.resolve("B");
    EXPECT_EQ(b_scope.id, "B");
    EXPECT_EQ(b_scope.api, b.get());

    EXPECT_FALSE(router.try_resolve("missing").has_value());
    EXPECT_THROW(router.resolve("missing"), AccountNotFound);
    EXPECT_EQ(router.account_ids(), (std::vector<std::string>{"A", "B"}));
}

} // namespace
} // namespace serve
} // namespace tqsdk

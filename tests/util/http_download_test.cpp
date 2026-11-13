#include "tqsdk/pch.h"

#include "tqsdk/util/http_download.h"

using namespace tqsdk;

TEST(HttpDownload, InvalidSchemeReturnsError) {
    const auto result = util::http_get("ftp://example.invalid/file");
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, util::HttpErrorCode::invalid_url);
    EXPECT_FALSE(result.error().detail.empty());
}

TEST(HttpDownload, InvalidHostReturnsError) {
    const auto result = util::http_get("http:///missing-host");
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, util::HttpErrorCode::invalid_url);
}

static_assert(util::http_error_name(util::HttpErrorCode::tls) == "tls");

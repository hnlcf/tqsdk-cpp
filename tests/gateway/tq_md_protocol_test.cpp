#include "tqsdk/pch.h"

#include "tqsdk/gateway/tq/tq_md_protocol.h"

using namespace tqsdk;
using json = nlohmann::json;

// 天勤 rtn_data.data[] 的典型片段（取自真实探测）：
//   patch0: {"quotes": {"SHFE.rb2610": {..., "amount": 4.5e6, 10 档盘口...}}}
//   patch1: {"account_id": "", "ins_list": "SHFE.rb2610,SHFE.rb", "mdhis_more_data": false}
//   patch2: {}
namespace {

json sample_rtn_data() {
    return json::parse(R"([
      {"quotes": {"SHFE.rb2610": {
          "instrument_id": "SHFE.rb2610",
          "datetime": "2024-10-30 21:00:00.000000",
          "last_price": 2990.0,
          "amount": 4500000.0,
          "volume": 12345,
          "open_interest": 67890,
          "close": 2990.0,
          "settlement": "-",
          "bid_price1": 2980.0, "bid_price2": 2970.0, "bid_price3": 2960.0,
          "bid_price4": 2950.0, "bid_price5": 2940.0, "bid_price6": 2930.0,
          "bid_price7": 2920.0, "bid_price8": 2910.0, "bid_price9": 2900.0,
          "bid_price10": 2890.0,
          "ask_price1": 3000.0, "ask_price6": 3010.0, "ask_price10": 3050.0
      }}},
      {"account_id": "", "ins_list": "SHFE.rb2610,SHFE.rb", "mdhis_more_data": false},
      {}
    ])");
}

} // namespace

// amount 必须翻译成 turnover
TEST(TqMdProtocol, AmountRenamedToTurnover) {
    json out = normalize_rtn_data(sample_rtn_data());
    ASSERT_TRUE(out.contains("quotes"));
    const auto& q = out["quotes"]["SHFE.rb2610"];
    EXPECT_FALSE(q.contains("amount")) << "amount should be renamed";
    ASSERT_TRUE(q.contains("turnover"));
    EXPECT_EQ(q["turnover"].get<double>(), 4500000.0);
}

// 10 档裁剪到 5 档：bid/ask_price6..10 必须被丢弃
TEST(TqMdProtocol, TenLevelsTrimmedToFive) {
    json        out = normalize_rtn_data(sample_rtn_data());
    const auto& q   = out["quotes"]["SHFE.rb2610"];
    for (int lvl : {6, 7, 8, 9, 10}) {
        EXPECT_FALSE(q.contains("bid_price" + std::to_string(lvl)));
        EXPECT_FALSE(q.contains("ask_price" + std::to_string(lvl)));
    }
    // 1..5 档保留
    EXPECT_TRUE(q.contains("bid_price5"));
    EXPECT_TRUE(q.contains("ask_price1"));
}

// "-" 字符串不做数值强转，原样透传（引擎侧会自然跳过）
TEST(TqMdProtocol, DashStringPassedThrough) {
    json        out = normalize_rtn_data(sample_rtn_data());
    const auto& q   = out["quotes"]["SHFE.rb2610"];
    ASSERT_TRUE(q.contains("settlement"));
    EXPECT_TRUE(q["settlement"].is_string());
    EXPECT_EQ(q["settlement"].get<std::string>(), "-");
}

// 数值字段原样保留
TEST(TqMdProtocol, NumericFieldsPreserved) {
    json        out = normalize_rtn_data(sample_rtn_data());
    const auto& q   = out["quotes"]["SHFE.rb2610"];
    EXPECT_EQ(q["last_price"].get<double>(), 2990.0);
    EXPECT_EQ(q["volume"].get<int>(), 12345);
    EXPECT_EQ(q["open_interest"].get<int>(), 67890);
}

// 无 quotes 的 data[] -> 空对象（调用方据此不推送）
TEST(TqMdProtocol, NoQuotesYieldsEmpty) {
    json data = json::parse(R"([{"account_id": ""}, {"ins_list": "SHFE.rb"}])");
    json out  = normalize_rtn_data(data);
    EXPECT_TRUE(out.empty());
}

// 多条 quotes 片段合并、后写覆盖先写
TEST(TqMdProtocol, MultiPatchQuotesMerged) {
    json data = json::parse(R"([
      {"quotes": {"SHFE.rb2610": {"last_price": 2990.0, "amount": 1.0}}},
      {"quotes": {"SHFE.rb2610": {"last_price": 2995.0, "amount": 2.0},
                  "DCE.i2501":  {"last_price": 800.0, "amount": 3.0}}}
    ])");
    json out  = normalize_rtn_data(data);
    ASSERT_TRUE(out.contains("quotes"));
    const auto& q1 = out["quotes"]["SHFE.rb2610"];
    EXPECT_EQ(q1["last_price"].get<double>(), 2995.0); // 后写覆盖
    EXPECT_EQ(q1["turnover"].get<double>(), 2.0);
    ASSERT_TRUE(out["quotes"].contains("DCE.i2501"));
    EXPECT_EQ(out["quotes"]["DCE.i2501"]["turnover"].get<double>(), 3.0);
}

// 非法入参（非数组）不崩溃，返回空
TEST(TqMdProtocol, NonArrayInputYieldsEmpty) {
    EXPECT_TRUE(normalize_rtn_data(json::object()).empty());
    EXPECT_TRUE(normalize_rtn_data(json()).empty());
}

// ---- K 线翻译 ----

// 天勤 K 线形态 {sym:{dur:{data:{id:bar}}}} -> 引擎数组形态 {sym:{dur:[bar...]}}
TEST(TqMdProtocol, KlineDataDictToArray) {
    json data = json::parse(R"([
      {"klines": {"SHFE.rb2610": {"180000000000": {"data": {
          "82630": {"datetime": 1792046400000000000, "open": 3005.0, "high": 3005.0,
                    "low": 3005.0, "close": 3005.0, "volume": 0, "open_oi": 4050, "close_oi": 4050},
          "82631": {"datetime": 17920464180000000000, "open": 3006.0, "high": 3007.0,
                    "low": 3005.0, "close": 3006.5, "volume": 10, "open_oi": 4050, "close_oi": 4055}
      }}}}}
    ])");
    json out  = normalize_rtn_data(data);
    ASSERT_TRUE(out.contains("klines"));
    const auto& arr = out["klines"]["SHFE.rb2610"]["180000000000"];
    ASSERT_TRUE(arr.is_array());
    ASSERT_EQ(arr.size(), 2u);
    // 按 id 数值升序
    EXPECT_EQ(arr[0]["datetime"].get<int64_t>(), 1792046400000000000LL);
    EXPECT_EQ(arr[1]["close"].get<double>(), 3006.5);
}

// bar_id 字典序陷阱： "100" 数值上大于 "99"，必须按数值排序而非字典序
TEST(TqMdProtocol, KlineBarIdNumericOrder) {
    json        data = json::parse(R"([
      {"klines": {"X": {"180000000000": {"data": {
          "100": {"datetime": 3, "close": 3.0},
          "99":  {"datetime": 2, "close": 2.0},
          "101": {"datetime": 4, "close": 4.0}
      }}}}}
    ])");
    json        out  = normalize_rtn_data(data);
    const auto& arr  = out["klines"]["X"]["180000000000"];
    ASSERT_EQ(arr.size(), 3u);
    EXPECT_EQ(arr[0]["datetime"].get<int>(), 2); // 99
    EXPECT_EQ(arr[1]["datetime"].get<int>(), 3); // 100
    EXPECT_EQ(arr[2]["datetime"].get<int>(), 4); // 101
}

// 纯元信息帧（无 "data"）被忽略，不产生 klines
TEST(TqMdProtocol, KlineMetaOnlyIgnored) {
    json data = json::parse(R"([
      {"klines": {"X": {"180000000000": {"last_id": 82649, "market_time_length": 1922}}}}
    ])");
    json out  = normalize_rtn_data(data);
    EXPECT_FALSE(out.contains("klines"));
    EXPECT_TRUE(out.empty());
}

// 同一帧内 quotes 与 klines 共存，各自独立输出
TEST(TqMdProtocol, QuotesAndKlinesCoexist) {
    json data = json::parse(R"([
      {"quotes": {"X": {"last_price": 100.0, "amount": 9.0}}},
      {"klines": {"X": {"180000000000": {"data": {"1": {"datetime": 100, "close": 99.0}}}}}}
    ])");
    json out  = normalize_rtn_data(data);
    ASSERT_TRUE(out.contains("quotes"));
    ASSERT_TRUE(out.contains("klines"));
    EXPECT_EQ(out["quotes"]["X"]["turnover"].get<double>(), 9.0);
    EXPECT_EQ(out["klines"]["X"]["180000000000"][0]["close"].get<double>(), 99.0);
}

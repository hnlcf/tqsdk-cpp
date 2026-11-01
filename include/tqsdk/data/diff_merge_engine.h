#pragma once

#include "tqsdk/pch.h"

#include "tqsdk/data/data_manager.h"

namespace tqsdk {

class DiffMergeEngine {
public:
    explicit DiffMergeEngine(DataManager& dm)
        : data_(dm) {}

    // 唯一对外入口:一次推送可能包含 quotes/trade/klines 中的任意组合
    void apply(const nlohmann::json& push);

private:
    // 顶层路由
    void merge_quotes(const nlohmann::json&);
    void merge_trade(const nlohmann::json&);
    void merge_klines(const nlohmann::json&);

    // 叶子
    void merge_one_quote(std::string_view sym, const nlohmann::json&);
    void merge_one_order(Order&, const nlohmann::json&);
    void merge_one_trade(Trade&, const nlohmann::json&);
    void merge_one_position(Position&, const nlohmann::json&);
    void merge_one_account(Account&, const nlohmann::json&);
    void merge_one_kline(std::string_view sym, int64_t dur, const nlohmann::json& bars);

    DataManager& data_;
};

} // namespace tqsdk

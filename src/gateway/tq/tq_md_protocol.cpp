#include "tqsdk/gateway/tq/tq_md_protocol.h"

namespace tqsdk {

namespace {

// 引擎只保留 5 档；协议多出来的档位在翻译时丢弃。
bool is_dropped_level(std::string_view key) {
    // 形如 bid_price6..bid_price10 / ask_volume6..ask_volume10
    for (std::string_view prefix : {"bid_price", "ask_price", "bid_volume", "ask_volume"}) {
        if (key.size() > prefix.size() && key.starts_with(prefix)) {
            std::string_view lvl = key.substr(prefix.size());
            // 档位为纯数字，且 > 5
            if (!lvl.empty()) {
                int  n         = 0;
                bool all_digit = true;
                for (char c : lvl) {
                    if (c < '0' || c > '9') {
                        all_digit = false;
                        break;
                    }
                    n = n * 10 + (c - '0');
                }
                if (all_digit && n > 5) {
                    return true;
                }
            }
        }
    }
    return false;
}

} // namespace

void merge_quote_patch(nlohmann::json& out_quotes, const nlohmann::json& quote_patch) {
    if (!quote_patch.is_object()) {
        return;
    }
    for (auto sym_it = quote_patch.begin(); sym_it != quote_patch.end(); ++sym_it) {
        const std::string& sym    = sym_it.key();
        const auto&        fields = sym_it.value();
        if (!fields.is_object()) {
            continue;
        }

        nlohmann::json& dst = out_quotes[sym]; // 可能是新建，也可能已有（多 patch 累积）
        if (!dst.is_object()) {
            dst = nlohmann::json::object();
        }

        for (auto fit = fields.begin(); fit != fields.end(); ++fit) {
            const std::string& key = fit.key();
            if (is_dropped_level(key)) {
                continue; // 裁掉 6..10 档
            }
            if (key == "amount") {
                dst["turnover"] = fit.value(); // 协议 amount -> 引擎 turnover
            } else {
                dst[key] = fit.value();
            }
        }
    }
}

void merge_kline_patch(nlohmann::json& out_klines, const nlohmann::json& kline_patch) {
    if (!kline_patch.is_object()) {
        return;
    }
    // 结构： { sym: { "<duration_ns>": { "data": { "<bar_id>": {bar} }, 或 meta } } }
    for (auto sym_it = kline_patch.begin(); sym_it != kline_patch.end(); ++sym_it) {
        const std::string& sym     = sym_it.key();
        const auto&        dur_map = sym_it.value();
        if (!dur_map.is_object()) {
            continue;
        }
        for (auto dur_it = dur_map.begin(); dur_it != dur_map.end(); ++dur_it) {
            const std::string& dur_str = dur_it.key();
            const auto&        payload = dur_it.value();
            if (!payload.is_object()) {
                continue;
            }
            auto data_it = payload.find("data");
            if (data_it == payload.end() || !data_it->is_object()) {
                continue; // 纯元信息帧（last_id / market_time_length ...），跳过
            }
            const auto& data = *data_it;
            if (data.empty()) {
                continue;
            }

            // 用 map 按 bar_id 升序累积（同 id 覆盖）。
            // 注意：bar_id 是十进制字符串，按数值排序避免 "100" < "99" 的字典序错误。
            std::map<int64_t, const nlohmann::json*> ordered;
            for (auto bar_it = data.begin(); bar_it != data.end(); ++bar_it) {
                int64_t id = 0;
                try {
                    id = std::stoll(bar_it.key());
                } catch (...) {
                    // 非数值 id 退化为 0（保底，不丢数据）
                }
                ordered[id] = &bar_it.value();
            }

            nlohmann::json bars = nlohmann::json::array();
            for (const auto& [id, barp] : ordered) {
                if (barp->is_object()) {
                    bars.push_back(*barp);
                }
            }
            if (!bars.empty()) {
                out_klines[sym][dur_str] = std::move(bars);
            }
        }
    }
}

nlohmann::json normalize_rtn_data(const nlohmann::json& patches) {
    nlohmann::json out_quotes = nlohmann::json::object();
    nlohmann::json out_klines = nlohmann::json::object();

    if (patches.is_array()) {
        for (const auto& patch : patches) {
            if (!patch.is_object()) {
                continue;
            }
            if (auto q = patch.find("quotes"); q != patch.end() && q->is_object()) {
                merge_quote_patch(out_quotes, *q);
            }
            if (auto k = patch.find("klines"); k != patch.end() && k->is_object()) {
                merge_kline_patch(out_klines, *k);
            }
            // 非 quotes/klines 的顶层字段（account_id / ins_list / charts / mdhis_more_data 等）
            // 与行情数据面无关，阶段4 忽略。
        }
    }

    nlohmann::json out = nlohmann::json::object();
    if (!out_quotes.empty()) {
        out["quotes"] = std::move(out_quotes);
    }
    if (!out_klines.empty()) {
        out["klines"] = std::move(out_klines);
    }
    return out;
}

} // namespace tqsdk

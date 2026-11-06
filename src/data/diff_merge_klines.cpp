#include "tqsdk/data/diff_merge_engine.h"
#include "tqsdk/data/field_table.h"
#include "tqsdk/domain/enum_util.h"
#include "tqsdk/types.h"

namespace tqsdk {

void DiffMergeEngine::merge_klines(const nlohmann::json& klines) {
    if (!klines.is_object()) {
        return;
    }
    for (auto sit = klines.begin(); sit != klines.end(); ++sit) {
        const auto& dur_map = sit.value();
        if (!dur_map.is_object()) {
            continue;
        }
        for (auto dit = dur_map.begin(); dit != dur_map.end(); ++dit) {
            int64_t dur = 0;
            try {
                dur = std::stoll(dit.key());
            } catch (...) {
                continue;
            }
            merge_one_kline(sit.key(), dur, dit.value());
        }
    }
}

void DiffMergeEngine::merge_one_kline(std::string_view      sym,
                                      int64_t               dur,
                                      const nlohmann::json& bars) {
    if (!bars.is_array() || bars.empty()) {
        return;
    }

    auto& s = data_.klines.get_or_create(sym, dur, 200);

    auto parse_bar = [](const nlohmann::json& b) {
        KlineBar r;
        if (!b.is_object()) {
            return r;
        }
        auto num = [&]<typename T>(const char* k, T& dst) {
            if (auto v = b.find(k); v != b.end() && v->is_number()) {
                dst = v->get<T>();
            }
        };
        num("datetime", r.datetime);
        num("open", r.open);
        num("high", r.high);
        num("low", r.low);
        num("close", r.close);
        num("volume", r.volume);
        num("open_oi", r.open_oi);
        num("close_oi", r.close_oi);
        return r;
    };

    if (bars.size() > 1) {
        if (s.cached_history) {
            // 本地缓存 + 远端只取了最新几根：按 datetime 合并，不能清掉更早历史。
            for (auto& raw : bars) {
                KlineBar incoming = parse_bar(raw);
                auto     pos      = std::lower_bound(
                    s.bars.begin(),
                    s.bars.end(),
                    incoming.datetime,
                    [](const KlineBar& item, TimeStamp dt) { return item.datetime < dt; });
                if (pos != s.bars.end() && pos->datetime == incoming.datetime) {
                    *pos = std::move(incoming);
                } else {
                    s.bars.insert(pos, std::move(incoming));
                }
            }
            while (static_cast<int>(s.bars.size()) > s.capacity) {
                s.bars.pop_front();
            }
            s.changed_fields |= KlineChangeDetail::Reset;
            return;
        }

        // 无本地历史：远端历史帧是权威全量，整体替换。
        s.bars.clear();
        for (auto& b : bars) {
            s.bars.push_back(parse_bar(b));
        }
        while (static_cast<int>(s.bars.size()) > s.capacity) {
            s.bars.pop_front();
        }
        s.changed_fields |= KlineChangeDetail::Reset;
        return;
    }

    // 单根:更新最后一根 or 追加
    KlineBar b = parse_bar(bars[0]);
    if (!s.bars.empty() && s.bars.back().datetime == b.datetime) {
        s.bars.back() = std::move(b);
        s.changed_fields |= KlineChangeDetail::LastUpdated;
    } else {
        s.bars.push_back(std::move(b));
        while (static_cast<int>(s.bars.size()) > s.capacity) {
            s.bars.pop_front();
        }
        s.changed_fields |= KlineChangeDetail::Appended;
    }
}

} // namespace tqsdk

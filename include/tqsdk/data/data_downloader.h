#pragma once

#include "tqsdk/pch.h"
#include "tqsdk/data/tick_series.h"
#include "tqsdk/domain/kline.h"

namespace tqsdk {

/// 把内存中的 tick / kline 序列导出为 CSV。
///
/// 设计目标是可重复、无隐式网络访问：调用方先通过 TqApi 获取/订阅数据，
/// 再由本类导出。CSV 使用 UTF-8、逗号分隔、首行表头。
class DataDownloader {
public:
    struct Options {
        char        delimiter   = ',';
        bool        header      = true;
        std::size_t limit       = 0; ///< 0 = 全部
    };

    DataDownloader() = default;
    explicit DataDownloader(Options options) : options_(options) {}

    bool download_ticks(const TickSeries& series,
                        const std::filesystem::path& path) const;
    bool download_klines(const KlineStream& series,
                         const std::filesystem::path& path) const;

    /// 纯字符串形式，便于测试和无文件系统环境使用。
    std::string ticks_csv(const TickSeries& series) const;
    std::string klines_csv(const KlineStream& series) const;

private:
    Options options_;
};

} // namespace tqsdk

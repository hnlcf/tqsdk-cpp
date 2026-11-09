#include "tqsdk/data/data_downloader.h"

#include <fstream>
#include <sstream>

namespace tqsdk {

namespace {

void csv_field(std::ostringstream& out, double value, char delimiter) {
    if (std::isfinite(value)) {
        out << value;
    }
    out << delimiter;
}

bool write_text(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return false;
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return out.good();
}

std::size_t begin_index(std::size_t size, std::size_t limit) {
    return limit == 0 || limit >= size ? 0 : size - limit;
}

} // namespace

std::string DataDownloader::ticks_csv(const TickSeries& series) const {
    std::ostringstream out;
    const char         d = options_.delimiter;
    if (options_.header) {
        out << "datetime" << d << "instrument_id" << d << "last_price" << d << "highest" << d
            << "lowest" << d << "open" << d << "close" << d << "pre_close" << d
            << "pre_settlement" << d << "average" << d << "turnover" << d << "open_interest" << d
            << "volume";
        for (int i = 1; i <= 5; ++i) out << d << "bid_price" << i;
        for (int i = 1; i <= 5; ++i) out << d << "ask_price" << i;
        for (int i = 1; i <= 5; ++i) out << d << "bid_volume" << i;
        for (int i = 1; i <= 5; ++i) out << d << "ask_volume" << i;
        out << '\n';
    }

    const auto start = begin_index(series.data.size(), options_.limit);
    std::size_t index = 0;
    for (const auto& t : series.data) {
        if (index++ < start) continue;
        out << t.datetime << d << t.instrument_id << d;
        csv_field(out, t.last_price, d);
        csv_field(out, t.highest, d);
        csv_field(out, t.lowest, d);
        csv_field(out, t.open, d);
        csv_field(out, t.close, d);
        csv_field(out, t.pre_close, d);
        csv_field(out, t.pre_settlement, d);
        csv_field(out, t.average, d);
        csv_field(out, t.turnover, d);
        csv_field(out, t.open_interest, d);
        out << t.volume << d;
        for (double v : t.bid_price) csv_field(out, v, d);
        for (double v : t.ask_price) csv_field(out, v, d);
        for (auto v : t.bid_volume) out << v << d;
        for (auto v : t.ask_volume) out << v << d;
        out << '\n';
    }
    return out.str();
}

std::string DataDownloader::klines_csv(const KlineStream& series) const {
    std::ostringstream out;
    const char         d = options_.delimiter;
    if (options_.header) {
        out << "datetime" << d << "instrument_id" << d << "duration_ns" << d << "open" << d
            << "high" << d << "low" << d << "close" << d << "volume" << d << "open_oi" << d
            << "close_oi\n";
    }

    const auto  total = series.bars.size();
    const auto  start = begin_index(total, options_.limit);
    std::size_t index = 0;
    for (const auto& bar : series.bars) {
        if (index++ < start) continue;
        out << bar.datetime << d << series.instrumnet_id << d << series.duration_ns << d;
        csv_field(out, bar.open, d);
        csv_field(out, bar.high, d);
        csv_field(out, bar.low, d);
        csv_field(out, bar.close, d);
        out << bar.volume << d << bar.open_oi << d << bar.close_oi << '\n';
    }
    return out.str();
}

bool DataDownloader::download_ticks(const TickSeries& series,
                                    const std::filesystem::path& path) const {
    return write_text(path, ticks_csv(series));
}

bool DataDownloader::download_klines(const KlineStream& series,
                                     const std::filesystem::path& path) const {
    return write_text(path, klines_csv(series));
}

} // namespace tqsdk

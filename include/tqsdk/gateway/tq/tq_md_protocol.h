#pragma once

#include "tqsdk/pch.h"

namespace tqsdk {

/// 天勤 MD 协议（DIFF/JSON 结构）与本地 DiffMergeEngine 之间的翻译层。
///
/// 天勤行情服务器（wss://.../t/md/front/mobile）推送的消息形如：
///   {"aid":"rtn_data","data":[ {patch0}, {patch1}, ... ]}
/// 其中每个 patch 是部分 merge 片段，常见形态：
///   - {"quotes": {"SHFE.rb2610": {"last_price": 2990.0, "amount": 4.5e6, ...}}}
///   - {"account_id": "...", "ins_list": "...", "mdhis_more_data": false}
///   - {} 空片段
///
/// 本地 DiffMergeEngine 与天勤协议有两处差异，必须在此翻译：
///   1. 字段名：协议用 "amount"（成交额），引擎用 "turnover"。
///   2. 引擎只认 5 档盘口（bid_price1..5 / ask_price1..5），协议最多 10 档；
///      多出的 6..10 档在翻译时丢弃（引擎本也会忽略，这里提前裁掉更清晰）。
///
/// 其余字段（last_price/volume/open_interest/... 及 "-" 字符串）直接透传：
///   - 数值字段原样保留；
///   - 字符串占位（如未收盘时 close/settlement 为 "-"）原样保留，
///     由引擎的 MERGE_DBL 依 is_number() 判定后自然跳过（保持 unknown）。

/// 把一条 rtn_data 的 data[] 片段数组，合并成"单条引擎可消费的 diff 对象"。
///
/// 语义（事务性）：data[] 中所有片段按出现顺序合并；同一符号的 quotes 字段
/// 后写覆盖先写，最终产出一个：
///   { "quotes": { sym: {合并后的字段...} , ... },
///     "klines": { sym: { "<duration_ns>": [bar,...] } , ... } }
/// 其中 quotes 与 klines 各自独立；任一为空则不输出对应键。
/// 若 data[] 中既无 quotes 也无 klines，则返回空对象 {}（调用方据此判断是否推送）。
///
/// 入参 `patches` 为 rtn_data 的 data 数组（nlohmann::json array）。
/// 返回值为一个可直接喂给 DiffMergeEngine::apply 的 JSON 对象。
nlohmann::json normalize_rtn_data(const nlohmann::json& patches);

/// 单条 patch 翻译：把 patch["quotes"] 各符号的 amount→turnover、裁剪 10→5 档。
/// 结果合并进 out_quotes（out_quotes 为 {sym: fields} 的累积对象）。
/// 供 normalize_rtn_data 内部使用，也单独可测。
void merge_quote_patch(nlohmann::json& out_quotes, const nlohmann::json& quote_patch);

/// 单条 patch 的 K 线翻译：把天勤协议形态
///   {"SHFE.rb2610": {"60000000000": {"data": {"<bar_id>": {bar}, ...}}}}
/// 翻译成引擎形态
///   {"SHFE.rb2610": {"60000000000": [bar, bar, ...]}}   (数组，按 bar_id 升序)
/// 结果合并进 out_klines（累积对象）。含 "data" 的 duration 覆盖写入（同 id 后写并集）；
/// 纯元信息（last_id / market_time_length / ... 无 "data"）的 duration 忽略。
void merge_kline_patch(nlohmann::json& out_klines, const nlohmann::json& kline_patch);

} // namespace tqsdk

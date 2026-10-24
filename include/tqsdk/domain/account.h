#pragma once

#include "tqsdk/pch.h"
#include "tqsdk/types.h"

#include "sentinel.h"

namespace tqsdk {

enum class AccountField : uint64_t {
    // TODO
};

struct Account {
    /// 用户ID
    AccountId account_id;

    /// 币种
    Currency currency = Currency::CNY;

    /// 上一交易日结算时的账户权益
    double pre_balance = kUnknownDouble;

    /// 本交易日内的入金金额
    double deposit = kUnknownDouble;

    /// 本交易日内的出金金额
    double withdraw = kUnknownDouble;

    /// 静态权益 = pre_balance + deposit - withdraw
    double static_balance = kUnknownDouble;

    /// 本交易日内的平仓盈亏
    double close_profit = kUnknownDouble;

    /// 本交易日内交纳的手续费
    double commission = kUnknownDouble;

    /// 本交易日内交纳的期权权利金
    double premium = kUnknownDouble;

    /// 当前持仓盈亏
    double position_profit = kUnknownDouble;

    /// 当前浮动盈亏
    double float_profit = kUnknownDouble;

    /// 账户权益 = static_balance + close_profit - commission - premium + position_profit
    double balance = kUnknownDouble;

    /// 持仓占用保证金
    double margin = kUnknownDouble;

    /// 挂单冻结保证金
    double frozen_margin = kUnknownDouble;

    /// 挂单冻结手续费
    double frozen_commission = kUnknownDouble;

    /// 挂单冻结权利金
    double frozen_premium = kUnknownDouble;

    /// 可用资金 = balance - margin - frozen_margin - frozen_commission - frozen_premium
    double available = kUnknownDouble;

    /// 风险度 = 1 - available / balance
    double risk_ratio = kUnknownDouble;

    /// 字段更新标志位图
    uint64_t changed_fields = 0;
};
} // namespace tqsdk

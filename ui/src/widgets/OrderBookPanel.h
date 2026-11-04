#pragma once

#include "net/Protocol.h"

#include <QWidget>

class QLabel;
class QMouseEvent;

namespace tqsdk::ui {

/// 五档盘口组件。
///
/// 数据来源为 Quote 的 bid/ask price/volume 1..5；组件只渲染，不请求行情。
/// 卖出档从上到下为卖5..卖1，中间为最新价，买入档为买1..买5。
class OrderBookPanel final : public QWidget {
    Q_OBJECT

public:
    explicit OrderBookPanel(QWidget* parent = nullptr);

    void setSymbol(const QString& symbol);
    void setQuote(const Quote* quote);
    void clear();

signals:
    void priceClicked(double price);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QLabel* symbol_       = nullptr;
    QLabel* last_         = nullptr;
    QLabel* askPrice_[5]  = {};
    QLabel* askVolume_[5] = {};
    QLabel* bidPrice_[5]  = {};
    QLabel* bidVolume_[5] = {};
};

} // namespace tqsdk::ui

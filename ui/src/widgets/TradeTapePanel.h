#pragma once

#include <QWidget>

class QEvent;
class QTableView;

namespace tqsdk::ui {

class TablePanel;
class TradeTapeModel;

/// 最新优先的有界分笔成交面板。
///
/// 表格区域多高就保留多少行；视口 resize 时直接调整模型容量。该面板与
/// 行情快照完全独立，可以单独 dock/浮动/关闭。
class TradeTapePanel final : public QWidget {
    Q_OBJECT

public:
    explicit TradeTapePanel(TradeTapeModel* model, QWidget* parent = nullptr);

    QTableView* view() const;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void updateCapacity();

    TablePanel*     panel_ = nullptr;
    TradeTapeModel* model_ = nullptr;
};

} // namespace tqsdk::ui

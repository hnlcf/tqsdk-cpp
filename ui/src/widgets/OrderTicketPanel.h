#pragma once

#include <QWidget>

class QCheckBox;
class QCompleter;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QStringListModel;

namespace tqsdk::ui {

/// 三键下单组件。
///
/// 组件只负责输入/按钮/状态展示；交易合约解析、持仓方向和最终 OrderRequest
/// 由 MainWindow/交易控制器处理。symbolSubmitted() 暴露给控制器做 fuzzy resolve。
class OrderTicketPanel final : public QWidget {
    Q_OBJECT

public:
    explicit OrderTicketPanel(QWidget* parent = nullptr);

    void setSymbol(const QString& symbol);
    void setCompletions(const QStringList& completions);
    void setTradingEnabled(bool enabled);
    void setCloseEnabled(bool enabled);
    void setStatus(const QString& text, bool error = false);
    void setPriceIfEmpty(double price);
    void setPrice(double price);

    QString symbolText() const;
    double  price() const;
    int     volume() const;
    bool    marketOrder() const;
    bool    counterpartyOrder() const;

signals:
    void symbolSubmitted(const QString& input);
    void buyOpenRequested();
    void sellOpenRequested();
    void closeRequested();

private:
    QLineEdit*        symbolEdit_    = nullptr;
    QCompleter*       completer_     = nullptr;
    QStringListModel* completions_   = nullptr;
    QDoubleSpinBox*   priceBox_      = nullptr;
    QSpinBox*         volumeBox_     = nullptr;
    QPushButton*      buyButton_     = nullptr;
    QPushButton*      sellButton_    = nullptr;
    QPushButton*      closeButton_   = nullptr;
    QCheckBox*        marketCheck_   = nullptr;
    QCheckBox*        opponentCheck_ = nullptr;
    QLabel*           status_        = nullptr;
};

} // namespace tqsdk::ui

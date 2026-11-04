#pragma once

#include "net/Protocol.h"

#include <QWidget>

class QLabel;

namespace tqsdk::ui {

/// 右侧行情快照显示组件。
///
/// 只负责 label 展示和格式化，不持有 node/model；MainWindow 在 quote
/// 更新后把当前合约和快照交给它。这样行情展示逻辑不会继续堆在 MainWindow。
class QuoteSnapshotPanel final : public QWidget {
    Q_OBJECT

public:
    explicit QuoteSnapshotPanel(QWidget* parent = nullptr);

    void setSymbol(const QString& symbol);
    /// quote 为空时清空数值；instrumentName 可来自 catalog。
    void setQuote(const Quote* quote, const QString& instrumentName);

private:
    QLabel* symbol_       = nullptr;
    QLabel* name_         = nullptr;
    QLabel* last_         = nullptr;
    QLabel* change_       = nullptr;
    QLabel* changePct_    = nullptr;
    QLabel* bidAsk_       = nullptr;
    QLabel* volume_       = nullptr;
    QLabel* openInterest_ = nullptr;
    QLabel* oiChange_     = nullptr;
    QLabel* highLow_      = nullptr;
};

} // namespace tqsdk::ui

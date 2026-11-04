#pragma once

#include <QWidget>

class QAbstractItemModel;
class QTableView;

namespace tqsdk::ui {

/// 可复用的表格列表组件。
///
/// 自选、合约、持仓、委托、成交、分笔、期权列表共用同一套选择行为、
/// 表头右键列配置和 QSettings 持久化，避免每个页面重复实现。
class TablePanel final : public QWidget {
    Q_OBJECT

public:
    explicit TablePanel(QAbstractItemModel* model, QWidget* parent = nullptr);

    QTableView* view() const { return view_; }

    /// 右键表头显示/隐藏列，并以 settingsKey 持久化。
    void installColumnMenu(const QString& settingsKey);

private:
    QTableView* view_ = nullptr;
};

} // namespace tqsdk::ui

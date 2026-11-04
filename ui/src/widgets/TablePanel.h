#pragma once

#include <QString>
#include <QWidget>

class QAbstractItemModel;
class QTableView;

namespace tqsdk::ui {

/// 可复用的表格列表组件。
///
/// 自选、合约、持仓、委托、成交、分笔、期权列表共用同一套选择行为、
/// 表头右键列配置、列宽/列序拖拽和 QSettings 持久化。
///
/// 持久化键布局：
///   ui/columns/<settingsKey>/hidden/<logicalColumn>
///   ui/columns/<settingsKey>/width/<logicalColumn>
///   ui/columns/<settingsKey>/order
/// 列索引使用 model 的 logical column，因此拖动换序不会改变数据语义。
class TablePanel final : public QWidget {
    Q_OBJECT

public:
    explicit TablePanel(QAbstractItemModel* model, QWidget* parent = nullptr);
    ~TablePanel() override;

    QTableView* view() const { return view_; }
    void        setRowHeight(int height);

    /// 右键表头显示/隐藏列，并以 settingsKey 持久化列显示、宽度和顺序。
    void installColumnMenu(const QString& settingsKey);

private:
    void restoreColumnState();
    void saveColumnState();

    QTableView* view_ = nullptr;
    QString     settingsKey_;
    bool        restoring_ = false;
};

} // namespace tqsdk::ui

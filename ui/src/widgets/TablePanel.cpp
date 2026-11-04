#include "widgets/TablePanel.h"

#include <QAbstractItemModel>
#include <QAction>
#include <QHeaderView>
#include <QMenu>
#include <QSettings>
#include <QSignalBlocker>
#include <QTableView>
#include <QVBoxLayout>
#include <QVariantList>

#include <algorithm>

namespace tqsdk::ui {
namespace {

QString orderKey(const QString& key) {
    return QStringLiteral("ui/columns/%1/order").arg(key);
}
QString widthKey(const QString& key, int column) {
    return QStringLiteral("ui/columns/%1/width/%2").arg(key).arg(column);
}
QString hiddenKey(const QString& key, int column) {
    return QStringLiteral("ui/columns/%1/hidden/%2").arg(key).arg(column);
}
QString legacyHiddenKey(const QString& key, int column) {
    return QStringLiteral("ui/columns/%1/%2").arg(key).arg(column);
}

} // namespace

TablePanel::TablePanel(QAbstractItemModel* model, QWidget* parent)
    : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    view_ = new QTableView(this);
    view_->setModel(model);
    view_->setSelectionBehavior(QAbstractItemView::SelectRows);
    view_->setSelectionMode(QAbstractItemView::SingleSelection);
    view_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view_->verticalHeader()->setVisible(false);
    view_->verticalHeader()->setDefaultSectionSize(24);
    view_->setWordWrap(false);
    view_->horizontalHeader()->setSectionsMovable(true);
    view_->horizontalHeader()->setSectionsClickable(true);
    view_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    view_->horizontalHeader()->setStretchLastSection(false);
    view_->setAlternatingRowColors(true);
    layout->addWidget(view_);
}

TablePanel::~TablePanel() {
    saveColumnState();
}

void TablePanel::installColumnMenu(const QString& settingsKey) {
    if (view_ == nullptr || view_->model() == nullptr || view_->horizontalHeader() == nullptr) {
        return;
    }
    settingsKey_ = settingsKey;
    restoreColumnState();

    auto* header = view_->horizontalHeader();
    header->setContextMenuPolicy(Qt::CustomContextMenu);

    connect(header,
            &QWidget::customContextMenuRequested,
            this,
            [view = view_, settingsKey](const QPoint& pos) {
                QMenu    menu(view);
                QAction* reset = menu.addAction(QObject::tr("显示全部列"));
                menu.addSeparator();
                for (int i = 0; i < view->model()->columnCount(); ++i) {
                    const QString title = view->model()->headerData(i, Qt::Horizontal).toString();
                    QAction* action = menu.addAction(title.isEmpty() ? QString::number(i) : title);
                    action->setCheckable(true);
                    action->setChecked(!view->isColumnHidden(i));
                    QObject::connect(
                        action, &QAction::toggled, view, [view, i, settingsKey](bool checked) {
                            view->setColumnHidden(i, !checked);
                            QSettings s;
                            s.setValue(hiddenKey(settingsKey, i), checked);
                        });
                }
                if (menu.exec(view->horizontalHeader()->viewport()->mapToGlobal(pos)) == reset) {
                    QSettings s;
                    for (int i = 0; i < view->model()->columnCount(); ++i) {
                        view->setColumnHidden(i, false);
                        s.setValue(hiddenKey(settingsKey, i), true);
                    }
                }
            });

    connect(header, &QHeaderView::sectionMoved, this, [this](int, int, int) { saveColumnState(); });
    connect(
        header, &QHeaderView::sectionResized, this, [this](int, int, int) { saveColumnState(); });
}

// 恢复顺序/宽度/隐藏状态。QSignalBlocker + restoring_ 防止恢复过程本身
// 触发 sectionMoved/sectionResized 再次写入不完整的中间状态。
void TablePanel::restoreColumnState() {
    if (view_ == nullptr || view_->model() == nullptr || settingsKey_.isEmpty()) {
        return;
    }
    auto*     header = view_->horizontalHeader();
    QSettings settings;
    const int count = view_->model()->columnCount();
    if (count <= 0) {
        return;
    }

    restoring_ = true;
    const QSignalBlocker blocker(header);

    const QVariantList storedOrder = settings.value(orderKey(settingsKey_)).toList();
    if (storedOrder.size() == count) {
        for (int visual = 0; visual < count; ++visual) {
            const int logical = storedOrder.at(visual).toInt();
            if (logical < 0 || logical >= count) {
                continue;
            }
            const int currentVisual = header->visualIndex(logical);
            if (currentVisual >= 0 && currentVisual != visual) {
                header->moveSection(currentVisual, visual);
            }
        }
    }

    for (int column = 0; column < count; ++column) {
        const QVariant width = settings.value(widthKey(settingsKey_, column));
        if (width.isValid()) {
            header->resizeSection(column, std::max(20, width.toInt()));
        }
        const QString hidden = hiddenKey(settingsKey_, column);
        const QString legacy = legacyHiddenKey(settingsKey_, column);
        if (settings.contains(hidden)) {
            view_->setColumnHidden(column, !settings.value(hidden, true).toBool());
        } else if (settings.contains(legacy)) {
            view_->setColumnHidden(column, !settings.value(legacy, true).toBool());
        }
    }

    restoring_ = false;
}

// 列状态以 logical index 保存，因此列顺序变化后宽度/隐藏属性仍跟随原列。
void TablePanel::saveColumnState() {
    if (view_ == nullptr || view_->model() == nullptr || settingsKey_.isEmpty() || restoring_) {
        return;
    }
    auto*     header = view_->horizontalHeader();
    QSettings settings;
    const int count = view_->model()->columnCount();

    QVariantList order;
    order.reserve(count);
    for (int visual = 0; visual < count; ++visual) {
        order.push_back(header->logicalIndex(visual));
    }
    settings.setValue(orderKey(settingsKey_), order);

    for (int column = 0; column < count; ++column) {
        settings.setValue(widthKey(settingsKey_, column), header->sectionSize(column));
        settings.setValue(hiddenKey(settingsKey_, column), !view_->isColumnHidden(column));
    }
}

} // namespace tqsdk::ui

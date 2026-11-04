#include "widgets/TablePanel.h"

#include <QAbstractItemModel>
#include <QAction>
#include <QHeaderView>
#include <QMenu>
#include <QSettings>
#include <QTableView>
#include <QVBoxLayout>

namespace tqsdk::ui {

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
    view_->horizontalHeader()->setStretchLastSection(true);
    view_->setAlternatingRowColors(true);
    layout->addWidget(view_);
}

void TablePanel::installColumnMenu(const QString& settingsKey) {
    if (view_ == nullptr || view_->model() == nullptr || view_->horizontalHeader() == nullptr) {
        return;
    }
    auto* header = view_->horizontalHeader();
    header->setContextMenuPolicy(Qt::CustomContextMenu);

    QSettings settings;
    for (int i = 0; i < view_->model()->columnCount(); ++i) {
        const QString key = QStringLiteral("ui/columns/%1/%2").arg(settingsKey).arg(i);
        if (settings.contains(key)) {
            view_->setColumnHidden(i, !settings.value(key, true).toBool());
        }
    }

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
                            s.setValue(QStringLiteral("ui/columns/%1/%2").arg(settingsKey).arg(i),
                                       checked);
                        });
                }
                if (menu.exec(view->horizontalHeader()->viewport()->mapToGlobal(pos)) == reset) {
                    QSettings s;
                    for (int i = 0; i < view->model()->columnCount(); ++i) {
                        view->setColumnHidden(i, false);
                        s.setValue(QStringLiteral("ui/columns/%1/%2").arg(settingsKey).arg(i),
                                   true);
                    }
                }
            });
}

} // namespace tqsdk::ui

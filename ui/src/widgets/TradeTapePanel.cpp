#include "widgets/TradeTapePanel.h"

#include "model/TradingModels.h"
#include "widgets/TablePanel.h"

#include <QEvent>
#include <QHeaderView>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace tqsdk::ui {

TradeTapePanel::TradeTapePanel(TradeTapeModel* model, QWidget* parent)
    : QWidget(parent),
      model_(model) {
    panel_ = new TablePanel(model, this);
    panel_->installColumnMenu(QStringLiteral("tape"));
    auto* table = panel_->view();
    table->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    table->setSelectionMode(QAbstractItemView::NoSelection);
    table->verticalHeader()->setDefaultSectionSize(22);
    table->viewport()->installEventFilter(this);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(panel_);
    QTimer::singleShot(0, this, [this] { updateCapacity(); });
}

QTableView* TradeTapePanel::view() const {
    return panel_ != nullptr ? panel_->view() : nullptr;
}

bool TradeTapePanel::eventFilter(QObject* watched, QEvent* event) {
    if (panel_ != nullptr && watched == panel_->view()->viewport() &&
        event->type() == QEvent::Resize) {
        updateCapacity();
    }
    return QWidget::eventFilter(watched, event);
}

void TradeTapePanel::updateCapacity() {
    if (panel_ == nullptr || model_ == nullptr) {
        return;
    }
    const int rowHeight = std::max(1, panel_->view()->verticalHeader()->defaultSectionSize());
    model_->setMaxRows(std::max(1, panel_->view()->viewport()->height() / rowHeight - 1));
}

} // namespace tqsdk::ui

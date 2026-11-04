#include "app/IndicatorController.h"

#include "chart/KLineChartWidget.h"
#include "indicator/IndicatorManager.h"
#include "indicator/IndicatorSettingsDialog.h"
#include "widgets/Toast.h"

#include <QAction>
#include <QJsonDocument>
#include <QMenu>
#include <QMenuBar>
#include <QSettings>
#include <QStatusBar>
#include <QTimer>

namespace tqsdk::ui {

IndicatorController::IndicatorController(KLineChartWidget* chart,
                                         QMenuBar*         menuBar,
                                         QStatusBar*       statusBar,
                                         Toast*            toast,
                                         QObject*          parent)
    : QObject(parent),
      chart_(chart),
      menuBar_(menuBar),
      statusBar_(statusBar),
      toast_(toast),
      manager_(new IndicatorManager(this)) {
    if (chart_ != nullptr) {
        connect(
            manager_,
            &IndicatorManager::seriesChanged,
            this,
            [this](const QVector<IndicatorSeries>& series) { chart_->setIndicatorSeries(series); });
    }
    connect(manager_, &IndicatorManager::errorOccurred, this, [this](const QString& error) {
        if (statusBar_ != nullptr) {
            statusBar_->showMessage(error, 6000);
        }
        if (toast_ != nullptr) {
            toast_->showMessage(error, true);
        }
    });
    connect(manager_, &IndicatorManager::catalogChanged, this, [this] {
        // 刷新动作可能正由菜单项的 triggered 信号触发；延后重建，避免在
        // QMenu::clear() 时删除当前信号发送者。
        QTimer::singleShot(0, this, &IndicatorController::rebuildMenu);
    });
    rebuildMenu();
}

void IndicatorController::setBars(const QString&           symbol,
                                  qint64                   durationNs,
                                  const QVector<KlineBar>& bars) {
    manager_->setBars(symbol, durationNs, bars);
    if (chart_ != nullptr && bars.isEmpty()) {
        chart_->setIndicatorSeries({});
    }
}

void IndicatorController::saveEnabled() {
    QSettings().setValue(QStringLiteral("ui/indicators/enabled"), manager_->enabled());
}

void IndicatorController::rebuildMenu() {
    if (menuBar_ == nullptr) {
        return;
    }
    auto*       indicatorMenu = menuBar_->addMenu(tr("指标"));
    QSettings   settings;
    QStringList enabled = settings.value(QStringLiteral("ui/indicators/enabled")).toStringList();

    QHash<QString, QAction*> actions;
    for (const auto& spec : manager_->specs()) {
        const QString    paramsKey = QStringLiteral("ui/indicators/%1/params").arg(spec.id);
        const QByteArray saved     = settings.value(paramsKey).toByteArray();
        if (!saved.isEmpty()) {
            const auto document = QJsonDocument::fromJson(saved);
            if (document.isObject()) {
                manager_->setParams(spec.id, document.object());
            }
        }

        auto* specMenu = indicatorMenu->addMenu(spec.name);
        auto* enable   = specMenu->addAction(tr("启用"));
        enable->setCheckable(true);
        enable->setChecked(enabled.contains(spec.id));
        actions.insert(spec.id, enable);
        specMenu->addSeparator();
        auto* edit = specMenu->addAction(tr("参数..."));

        connect(enable, &QAction::toggled, this, [this, actions] {
            QStringList ids;
            for (auto it = actions.begin(); it != actions.end(); ++it) {
                if (it.value()->isChecked()) {
                    ids.push_back(it.key());
                }
            }
            manager_->setEnabled(ids);
            saveEnabled();
        });
        connect(edit, &QAction::triggered, this, [this, spec] {
            IndicatorSettingsDialog dialog(spec,
                                           manager_->params(spec.id),
                                           menuBar_ != nullptr ? menuBar_->window() : nullptr);
            if (dialog.exec() != QDialog::Accepted) {
                return;
            }
            const QJsonObject values = dialog.values();
            manager_->setParams(spec.id, values);
            QSettings().setValue(QStringLiteral("ui/indicators/%1/params").arg(spec.id),
                                 QJsonDocument(values).toJson(QJsonDocument::Compact));
            if (statusBar_ != nullptr) {
                statusBar_->showMessage(tr("%1 参数已更新").arg(spec.name), 2500);
            }
        });
    }

    indicatorMenu->addSeparator();
    auto* refresh = indicatorMenu->addAction(tr("刷新指标列表"));
    connect(refresh, &QAction::triggered, this, [this] { manager_->reloadCatalog(); });
    auto* hint = indicatorMenu->addAction(tr("指标计算由独立 Python sidecar 执行"));
    hint->setEnabled(false);
    manager_->setEnabled(enabled);
}

} // namespace tqsdk::ui

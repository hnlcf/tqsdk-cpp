#pragma once

#include "net/Protocol.h"

#include <QObject>
#include <QVector>

class QMenuBar;
class QStatusBar;

namespace tqsdk::ui {

class IndicatorManager;
class KLineChartWidget;
class Toast;

/// 指标 UI 装配层：负责菜单、参数对话框、QSettings 和 Manager 生命周期。
/// MainWindow 只转发 K 线，不直接处理指标协议或参数持久化。
class IndicatorController final : public QObject {
    Q_OBJECT

public:
    IndicatorController(KLineChartWidget* chart,
                        QMenuBar*         menuBar,
                        QStatusBar*       statusBar,
                        Toast*            toast,
                        QObject*          parent = nullptr);

    void setBars(const QString& symbol, qint64 durationNs, const QVector<KlineBar>& bars);

private:
    void buildMenu();
    void saveEnabled();

    KLineChartWidget* chart_     = nullptr;
    QMenuBar*         menuBar_   = nullptr;
    QStatusBar*       statusBar_ = nullptr;
    Toast*            toast_     = nullptr;
    IndicatorManager* manager_   = nullptr;
};

} // namespace tqsdk::ui

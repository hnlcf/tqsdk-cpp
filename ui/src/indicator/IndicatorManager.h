#pragma once

#include "indicator/IndicatorTypes.h"
#include "net/Protocol.h"

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QVector>

class QProcess;
class QTimer;

namespace tqsdk::ui {

/// Python indicator sidecar 的 Qt 生命周期/协议管理器。
///
/// - 指标计算永远在独立进程，不在 GUI 线程执行用户脚本；
/// - 主进程只持有最近 K 线和 sidecar 返回的数值序列；
/// - 进程崩溃/超时会禁用当前计算并允许自动恢复，不阻塞窗口。
class IndicatorManager final : public QObject {
    Q_OBJECT

public:
    explicit IndicatorManager(QObject* parent = nullptr);
    ~IndicatorManager() override;

    QVector<IndicatorSpec> specs() const { return specs_; }
    QJsonObject            params(const QString& indicatorId) const;
    QStringList            enabled() const { return enabled_; }

    /// 测试/打包环境可显式指定脚本路径；正常路径由环境变量或源码目录发现。
    void setSidecarPathForTest(const QString& path);

    void setEnabled(const QStringList& indicatorIds);
    void setParams(const QString& indicatorId, const QJsonObject& params);
    void setBars(const QString& symbol, qint64 durationNs, const QVector<KlineBar>& bars);
    void clear();

signals:
    void seriesChanged(const QVector<tqsdk::ui::IndicatorSeries>& series);
    void errorOccurred(const QString& error);

private:
    void    startProcess();
    void    stopProcess();
    void    scheduleRestart();
    void    sendFrame(const QJsonObject& message);
    void    sendInit(const QString& indicatorId);
    void    sendBars(bool forceReset);
    void    flushInput();
    void    handleMessage(const QJsonObject& message);
    void    emitSeries();
    QString resolveSidecarPath() const;
    QString pythonExecutable() const;
    QString specName(const QString& indicatorId) const;
    QString specPanel(const QString& indicatorId) const;
    QColor  seriesColor(const QString& indicatorId, int index) const;

    QVector<IndicatorSpec>      specs_;
    QHash<QString, QJsonObject> params_;
    QStringList                 enabled_;

    QProcess*         process_       = nullptr;
    QTimer*           restartTimer_  = nullptr;
    QTimer*           responseTimer_ = nullptr;
    QByteArray        inputBuffer_;
    QString           sidecarPath_;
    QString           symbol_;
    qint64            durationNs_ = 0;
    QVector<KlineBar> bars_;
    QVector<KlineBar> sentBars_;
    QString           sentSymbol_;
    qint64            sentDurationNs_ = 0;

    QHash<QString, QVector<IndicatorSeries>> seriesById_;
    QSet<QString>                            pendingResponses_;
    bool                                     stopping_ = false;
    bool                                     ready_    = false;
};

} // namespace tqsdk::ui

#pragma once

#include "net/Protocol.h"

#include <QPoint>
#include <QRectF>
#include <QVector>
#include <QWidget>

class QPainter;
class QMouseEvent;
class QPaintEvent;

namespace tqsdk::ui {

/// 轻量 QPainter K 线控件。
///
/// Phase 1 先做蜡烛、成交量、当前价、坐标和十字光标；指标/画线后续叠加。
class KLineChartWidget : public QWidget {
public:
    explicit KLineChartWidget(QWidget* parent = nullptr);

    void setBars(const QVector<KlineBar>& bars);
    void setTitle(const QString& symbol, const QString& interval = QStringLiteral("1m"));
    void setTheme(bool dark);

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    void drawEmpty(QPainter& painter);
    void drawGrid(QPainter& painter, const QRectF& plot, double minPrice, double maxPrice) const;
    void drawCandles(QPainter& painter, const QRectF& plot, double minPrice, double maxPrice) const;
    void drawVolume(QPainter& painter, const QRectF& plot) const;
    void
    drawCrosshair(QPainter& painter, const QRectF& plot, double minPrice, double maxPrice) const;

    QVector<KlineBar> bars_;
    QString           symbol_    = QStringLiteral("—");
    QString           interval_  = QStringLiteral("1m");
    bool              dark_      = true;
    QPoint            crosshair_ = QPoint(-1, -1);
};

} // namespace tqsdk::ui

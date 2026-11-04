#pragma once

#include "net/Protocol.h"

#include <QPoint>
#include <QRectF>
#include <QVector>
#include <QWidget>

class QPainter;
class QMouseEvent;
class QPaintEvent;
class QWheelEvent;

namespace tqsdk::ui {

/// 轻量 QPainter K 线控件。
///
/// 支持滚轮缩放、按住左键左右拖动；头部显示最新价/涨跌/涨跌幅。
class KLineChartWidget : public QWidget {
public:
    explicit KLineChartWidget(QWidget* parent = nullptr);

    void setBars(const QVector<KlineBar>& bars);
    void setQuote(const Quote& quote);
    void setTitle(const QString& symbol, const QString& interval = QStringLiteral("1d"));
    void setTheme(bool dark);

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    QRectF plotArea() const;
    int    firstVisibleBar() const;
    int    visibleBarCount() const;
    void   clampView();
    void   drawHeader(QPainter& painter);
    void   drawEmpty(QPainter& painter);
    void   drawGrid(QPainter& painter, const QRectF& plot, double minPrice, double maxPrice) const;
    void   drawCandles(QPainter&     painter,
                       const QRectF& plot,
                       double        minPrice,
                       double        maxPrice,
                       int           first,
                       int           count) const;
    void   drawVolume(QPainter& painter, const QRectF& plot, int first, int count) const;
    void
    drawCrosshair(QPainter& painter, const QRectF& plot, double minPrice, double maxPrice) const;

    QVector<KlineBar> bars_;
    Quote             quote_;
    QString           symbol_    = QStringLiteral("—");
    QString           interval_  = QStringLiteral("1d");
    bool              dark_      = true;
    QPoint            crosshair_ = QPoint(-1, -1);

    int  visibleCount_ = 120;
    int  visibleStart_ = 0;
    bool followLatest_ = true;
    bool dragging_     = false;
    int  dragLastX_    = 0;
    int  dragStart_    = 0;
};

} // namespace tqsdk::ui

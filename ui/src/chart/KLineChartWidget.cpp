#include "chart/KLineChartWidget.h"

#include <QDateTime>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace tqsdk::ui {
namespace {

bool finite(double value) {
    return std::isfinite(value);
}

QColor upColor(bool dark) {
    return dark ? QColor("#e43c4a") : QColor("#c62828");
}

QColor downColor(bool dark) {
    return dark ? QColor("#2fbf71") : QColor("#1b8f5a");
}

QString priceText(double value) {
    return finite(value) ? QString::number(value, 'f', 2) : QStringLiteral("—");
}

QString timeText(qint64 nanoseconds) {
    if (nanoseconds <= 0) {
        return {};
    }
    return QDateTime::fromMSecsSinceEpoch(nanoseconds / 1'000'000)
        .toString(QStringLiteral("MM-dd HH:mm"));
}

} // namespace

KLineChartWidget::KLineChartWidget(QWidget* parent)
    : QWidget(parent) {
    setMinimumSize(420, 280);
    setMouseTracking(true);
    setAutoFillBackground(false);
    setToolTip(tr("滚轮缩放，按住左键左右拖动"));
    setCursor(Qt::OpenHandCursor);
}

void KLineChartWidget::setBars(const QVector<KlineBar>& bars) {
    const int oldSize = bars_.size();
    bars_             = bars;
    if (oldSize == 0) {
        if (followLatest_) {
            visibleStart_ = std::max(0, static_cast<int>(bars_.size()) - visibleBarCount());
        }
    } else if (followLatest_) {
        visibleStart_ = std::max(0, static_cast<int>(bars_.size()) - visibleBarCount());
    }
    clampView();
    update();
}

void KLineChartWidget::setViewport(int visibleCount, int visibleStart, bool followLatest) {
    visibleCount_ = std::clamp(visibleCount, 1, 2000);
    followLatest_ = followLatest;
    if (followLatest_) {
        visibleStart_ = std::max(0, static_cast<int>(bars_.size()) - visibleBarCount());
    } else {
        visibleStart_ = std::max(0, visibleStart);
    }
    clampView();
    update();
}

void KLineChartWidget::setQuote(const Quote& quote) {
    quote_ = quote;
    update();
}

void KLineChartWidget::setTitle(const QString& symbol, const QString& interval) {
    symbol_   = symbol;
    interval_ = interval;
    update();
}

void KLineChartWidget::setTheme(bool dark) {
    dark_ = dark;
    update();
}

QSize KLineChartWidget::sizeHint() const {
    return {900, 520};
}

QRectF KLineChartWidget::plotArea() const {
    return QRectF(14.0,
                  56.0,
                  std::max(20.0, static_cast<double>(width()) - 92.0),
                  std::max(20.0, static_cast<double>(height()) - 148.0));
}

int KLineChartWidget::visibleBarCount() const {
    if (bars_.isEmpty()) {
        return 0;
    }
    return std::clamp(visibleCount_, 1, std::min<int>(bars_.size(), 2000));
}

int KLineChartWidget::firstVisibleBar() const {
    const int count = visibleBarCount();
    if (count <= 0) {
        return 0;
    }
    return std::clamp(visibleStart_, 0, std::max(0, static_cast<int>(bars_.size()) - count));
}

void KLineChartWidget::clampView() {
    if (bars_.isEmpty()) {
        visibleCount_ = std::clamp(visibleCount_, 1, 2000);
        visibleStart_ = std::max(0, visibleStart_);
        return;
    }
    visibleCount_   = std::clamp(visibleCount_, 1, 2000);
    const int count = visibleBarCount();
    visibleStart_ =
        std::clamp(visibleStart_, 0, std::max(0, static_cast<int>(bars_.size()) - count));
    followLatest_ = visibleStart_ + count >= bars_.size();
}

void KLineChartWidget::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QColor background = dark_ ? QColor("#0d1117") : QColor("#f7f9fc");
    painter.fillRect(rect(), background);
    drawHeader(painter);

    if (bars_.isEmpty()) {
        drawEmpty(painter);
        return;
    }

    const int first = firstVisibleBar();
    const int count = visibleBarCount();
    if (count <= 0) {
        drawEmpty(painter);
        return;
    }

    double minPrice = bars_.at(first).low;
    double maxPrice = bars_.at(first).high;
    for (int i = first; i < first + count; ++i) {
        const auto& bar = bars_.at(i);
        if (finite(bar.low)) {
            minPrice = std::min(minPrice, bar.low);
        }
        if (finite(bar.high)) {
            maxPrice = std::max(maxPrice, bar.high);
        }
    }
    if (!finite(minPrice) || !finite(maxPrice)) {
        drawEmpty(painter);
        return;
    }
    if (maxPrice <= minPrice) {
        maxPrice = minPrice + 1.0;
        minPrice -= 1.0;
    }
    const double padding = (maxPrice - minPrice) * 0.05;
    minPrice -= padding;
    maxPrice += padding;

    const QRectF plot = plotArea();
    const QRectF volumePlot(plot.left(), plot.bottom() + 8.0, plot.width(), 64.0);
    drawGrid(painter, plot, minPrice, maxPrice);
    drawCandles(painter, plot, minPrice, maxPrice, first, count);
    drawVolume(painter, volumePlot, first, count);
    drawCrosshair(painter, plot, minPrice, maxPrice);
}

void KLineChartWidget::drawHeader(QPainter& painter) {
    QFont titleFont = font();
    titleFont.setPointSizeF(titleFont.pointSizeF() + 2.0);
    titleFont.setBold(true);
    painter.setFont(titleFont);
    painter.setPen(dark_ ? QColor("#e6edf3") : QColor("#202a36"));
    painter.drawText(QRect(14, 7, width() - 320, 28), Qt::AlignLeft | Qt::AlignVCenter, symbol_);

    painter.setFont(font());
    painter.setPen(dark_ ? QColor("#8b93a3") : QColor("#657080"));
    painter.drawText(QRect(14, 31, 150, 20), Qt::AlignLeft | Qt::AlignVCenter, interval_);

    double last = quote_.lastPrice;
    if (!finite(last) && !bars_.isEmpty()) {
        last = bars_.last().close;
    }
    double reference = quote_.preSettlement;
    if (!finite(reference) && bars_.size() >= 2) {
        reference = bars_.at(bars_.size() - 2).close;
    }
    const double change     = finite(last) && finite(reference)
                                  ? last - reference
                                  : std::numeric_limits<double>::quiet_NaN();
    const double percent    = finite(change) && reference != 0.0
                                  ? change / reference * 100.0
                                  : std::numeric_limits<double>::quiet_NaN();
    const QColor valueColor = !finite(change) ? (dark_ ? QColor("#8b93a3") : QColor("#657080"))
                                              : (change >= 0.0 ? upColor(dark_) : downColor(dark_));

    QFont valueFont = font();
    valueFont.setPointSizeF(valueFont.pointSizeF() + 7.0);
    valueFont.setBold(true);
    painter.setFont(valueFont);
    painter.setPen(valueColor);
    painter.drawText(
        QRect(width() - 300, 3, 286, 34), Qt::AlignRight | Qt::AlignVCenter, priceText(last));

    painter.setFont(font());
    const QString delta = finite(change) ? QStringLiteral("%1%2  %3%4%")
                                               .arg(change >= 0.0 ? "+" : "")
                                               .arg(change, 0, 'f', 2)
                                               .arg(finite(percent) && percent >= 0.0 ? "+" : "")
                                               .arg(percent, 0, 'f', 2)
                                         : QStringLiteral("涨跌 —  —%");
    painter.drawText(QRect(width() - 360, 35, 346, 18), Qt::AlignRight | Qt::AlignVCenter, delta);
}

void KLineChartWidget::drawEmpty(QPainter& painter) {
    painter.setPen(dark_ ? QColor("#66717f") : QColor("#8b93a3"));
    painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("等待 K 线数据"));
}

void KLineChartWidget::drawGrid(QPainter&     painter,
                                const QRectF& plot,
                                double        minPrice,
                                double        maxPrice) const {
    const QColor grid = dark_ ? QColor("#202a36") : QColor("#dde3eb");
    const QColor text = dark_ ? QColor("#7f8b99") : QColor("#6b7584");
    painter.setPen(QPen(grid, 1.0));
    for (int i = 0; i <= 4; ++i) {
        const double y = plot.top() + plot.height() * i / 4.0;
        painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        const double value = maxPrice - (maxPrice - minPrice) * i / 4.0;
        painter.setPen(text);
        painter.drawText(QRectF(plot.right() + 8, y - 8, 70, 16),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         priceText(value));
        painter.setPen(QPen(grid, 1.0));
    }

    for (int i = 0; i <= 3; ++i) {
        const double x = plot.left() + plot.width() * i / 3.0;
        painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
    }
}

void KLineChartWidget::drawCandles(QPainter&     painter,
                                   const QRectF& plot,
                                   double        minPrice,
                                   double        maxPrice,
                                   int           first,
                                   int           count) const {
    if (count <= 0 || maxPrice <= minPrice) {
        return;
    }
    const double step      = plot.width() / count;
    const double bodyWidth = std::max(1.0, step * 0.64);
    const double range     = maxPrice - minPrice;

    auto yFor = [&](double price) {
        return plot.bottom() - (price - minPrice) / range * plot.height();
    };
    const QColor text = dark_ ? QColor("#7f8b99") : QColor("#6b7584");

    for (int i = 0; i < count; ++i) {
        const auto& bar = bars_.at(first + i);
        if (!finite(bar.open) || !finite(bar.close)) {
            continue;
        }
        const double x      = plot.left() + step * (i + 0.5);
        const double yOpen  = yFor(bar.open);
        const double yClose = yFor(bar.close);
        const QColor color  = bar.close >= bar.open ? upColor(dark_) : downColor(dark_);

        painter.setPen(QPen(color, 1.0));
        painter.drawLine(QPointF(x, yFor(bar.high)), QPointF(x, yFor(bar.low)));

        const double top    = std::min(yOpen, yClose);
        const double bottom = std::max(yOpen, yClose);
        const QRectF body(x - bodyWidth / 2.0, top, bodyWidth, std::max(1.0, bottom - top));
        if (bar.close >= bar.open) {
            // A-share/futures convention: rising candles are hollow red.
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(color, 1.0));
            painter.drawRect(body);
        } else {
            // Falling candles remain solid green.
            painter.setPen(Qt::NoPen);
            painter.setBrush(color);
            painter.drawRect(body);
        }

        if (i == 0 || i == count / 2 || i == count - 1) {
            painter.setPen(text);
            const int labelWidth = 100;
            painter.drawText(QRectF(x - labelWidth / 2.0, plot.bottom() + 4, labelWidth, 16),
                             Qt::AlignHCenter | Qt::AlignTop,
                             timeText(bar.datetime));
        }
    }
}

void KLineChartWidget::drawVolume(QPainter&     painter,
                                  const QRectF& plot,
                                  int           first,
                                  int           count) const {
    if (count <= 0) {
        return;
    }
    quint64 maxVolume = 1;
    for (int i = first; i < first + count; ++i) {
        maxVolume = std::max(maxVolume, bars_.at(i).volume);
    }
    const double step     = plot.width() / count;
    const double barWidth = std::max(1.0, step * 0.64);
    for (int i = 0; i < count; ++i) {
        const auto&  bar = bars_.at(first + i);
        const double h =
            plot.height() * static_cast<double>(bar.volume) / static_cast<double>(maxVolume);
        const double x = plot.left() + step * (i + 0.5);
        painter.fillRect(QRectF(x - barWidth / 2.0, plot.bottom() - h, barWidth, std::max(1.0, h)),
                         bar.close >= bar.open ? upColor(dark_) : downColor(dark_));
    }
}

void KLineChartWidget::drawCrosshair(QPainter&     painter,
                                     const QRectF& plot,
                                     double        minPrice,
                                     double        maxPrice) const {
    if (!plot.contains(crosshair_)) {
        return;
    }
    const QColor line = dark_ ? QColor("#596574") : QColor("#9aa4b2");
    painter.setPen(QPen(line, 1.0, Qt::DashLine));
    painter.drawLine(QPointF(crosshair_.x(), plot.top()), QPointF(crosshair_.x(), plot.bottom()));
    painter.drawLine(QPointF(plot.left(), crosshair_.y()), QPointF(plot.right(), crosshair_.y()));

    const double ratio = (plot.bottom() - crosshair_.y()) / plot.height();
    const double value = minPrice + ratio * (maxPrice - minPrice);
    painter.setPen(Qt::NoPen);
    painter.setBrush(dark_ ? QColor("#263344") : QColor("#ffffff"));
    const QRectF label(plot.right() + 4, crosshair_.y() - 9, 72, 18);
    painter.drawRect(label);
    painter.setPen(dark_ ? QColor("#e6edf3") : QColor("#202a36"));
    painter.drawText(label, Qt::AlignCenter, priceText(value));

    const int first = firstVisibleBar();
    const int count = visibleBarCount();
    if (count > 0) {
        const double step  = plot.width() / count;
        int          index = first + static_cast<int>((crosshair_.x() - plot.left()) / step);
        index              = std::clamp(index, first, first + count - 1);
        if (index >= 0 && index < bars_.size()) {
            const QString when = timeText(bars_.at(index).datetime);
            const QRectF  timeLabel(crosshair_.x() - 58.0, height() - 22.0, 116.0, 18.0);
            painter.fillRect(timeLabel, dark_ ? QColor("#263344") : QColor("#ffffff"));
            painter.setPen(dark_ ? QColor("#e6edf3") : QColor("#202a36"));
            painter.drawText(timeLabel, Qt::AlignCenter, when);
        }
    }
}

void KLineChartWidget::mouseMoveEvent(QMouseEvent* event) {
    if (dragging_) {
        const QRectF plot = plotArea();
        const int    step =
            std::max(1, static_cast<int>(plot.width()) / std::max(1, visibleBarCount()));
        const int dx  = event->position().toPoint().x() - dragLastX_;
        visibleStart_ = dragStart_ - static_cast<int>(std::lround(static_cast<double>(dx) / step));
        clampView();
        followLatest_ = false;
        update();
        emit viewportChanged();
        event->accept();
        return;
    }
    crosshair_ = event->position().toPoint();
    update();
    QWidget::mouseMoveEvent(event);
}

void KLineChartWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && plotArea().contains(event->position())) {
        dragging_     = true;
        dragLastX_    = event->position().toPoint().x();
        dragStart_    = firstVisibleBar();
        followLatest_ = false;
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void KLineChartWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && dragging_) {
        dragging_ = false;
        clampView();
        setCursor(Qt::OpenHandCursor);
        emit viewportChanged();
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void KLineChartWidget::wheelEvent(QWheelEvent* event) {
    if (bars_.isEmpty() || event->angleDelta().y() == 0) {
        event->ignore();
        return;
    }
    const int    oldCount = visibleBarCount();
    const int    oldFirst = firstVisibleBar();
    const double factor   = std::pow(1.18, -event->angleDelta().y() / 120.0);
    const int    maxCount = std::min<int>(bars_.size(), 2000);
    const int    minCount = std::min(10, maxCount);
    const int    newCount =
        std::clamp(static_cast<int>(std::lround(oldCount * factor)), minCount, maxCount);
    if (newCount == oldCount) {
        event->accept();
        return;
    }

    const QRectF plot  = plotArea();
    const double ratio = std::clamp((event->position().x() - plot.left()) / plot.width(), 0.0, 1.0);
    const int    anchor = oldFirst + static_cast<int>(std::lround(ratio * oldCount));
    visibleCount_       = newCount;
    visibleStart_       = anchor - static_cast<int>(std::lround(ratio * newCount));
    followLatest_       = false;
    clampView();
    update();
    emit viewportChanged();
    event->accept();
}

void KLineChartWidget::leaveEvent(QEvent* event) {
    crosshair_ = QPoint(-1, -1);
    if (!dragging_) {
        setCursor(Qt::OpenHandCursor);
    }
    update();
    QWidget::leaveEvent(event);
}

} // namespace tqsdk::ui

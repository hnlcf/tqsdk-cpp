#include "chart/KLineChartWidget.h"

#include <QDateTime>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace tqsdk::ui {
namespace {

bool finite(double value) {
    return std::isfinite(value);
}

QColor upColor(bool dark) {
    return dark ? QColor("#e04a4a") : QColor("#c94b4b");
}

QColor downColor(bool dark) {
    return dark ? QColor("#23b39a") : QColor("#249c86");
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

constexpr double kHeaderHeight = 56.0;
constexpr double kAxisGap      = 28.0;
constexpr double kVolumeHeight = 86.0;
constexpr double kBottomMargin = 8.0;

} // namespace

KLineChartWidget::KLineChartWidget(QWidget* parent)
    : QWidget(parent) {
    setMinimumSize(420, 280);
    setMouseTracking(true);
    titleFont_ = font();
    titleFont_.setPointSizeF(titleFont_.pointSizeF() + 2.0);
    titleFont_.setBold(true);
    valueFont_ = font();
    valueFont_.setPointSizeF(valueFont_.pointSizeF() + 7.0);
    valueFont_.setBold(true);
    crosshairTimer_ = new QTimer(this);
    crosshairTimer_->setSingleShot(true);
    crosshairTimer_->setInterval(16);
    connect(crosshairTimer_, &QTimer::timeout, this, [this] {
        if (crosshair_ == pendingCrosshair_) {
            return;
        }
        if (crosshair_.x() >= 0) {
            update(crosshairDamageRect(crosshair_));
        }
        crosshair_ = pendingCrosshair_;
        if (crosshair_.x() >= 0) {
            update(crosshairDamageRect(crosshair_));
        }
    });
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
    invalidateCache();
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
    invalidateCache();
}

void KLineChartWidget::setQuote(const Quote& quote) {
    // Only last price / pre-settlement affect chart painting. Volume-only quote
    // frames must not force the whole chart to repaint.
    if (quote.lastPrice == quote_.lastPrice && quote.preSettlement == quote_.preSettlement) {
        return;
    }
    quote_ = quote;
    update(headerDamageRect());
}

void KLineChartWidget::setTitle(const QString& symbol, const QString& interval) {
    symbol_   = symbol;
    interval_ = interval;
    update(headerDamageRect());
}

void KLineChartWidget::setTheme(bool dark) {
    dark_ = dark;
    invalidateCache();
}

QSize KLineChartWidget::sizeHint() const {
    return {900, 520};
}

QRectF KLineChartWidget::plotArea() const {
    return QRectF(14.0,
                  kHeaderHeight,
                  std::max(20.0, static_cast<double>(width()) - 92.0),
                  std::max(20.0,
                           static_cast<double>(height()) - kHeaderHeight - kAxisGap -
                               kVolumeHeight - kBottomMargin));
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

QRect KLineChartWidget::headerDamageRect() const {
    return QRect(0, 0, width(), 56);
}

QRect KLineChartWidget::crosshairDamageRect(const QPoint& point) const {
    if (point.x() < 0 || point.y() < 0) {
        return {};
    }
    const QRectF plot = plotArea();
    QRect        damage(
        static_cast<int>(plot.left()), point.y() - 10, static_cast<int>(plot.width()) + 84, 20);
    damage = damage.united(QRect(
        point.x() - 10, static_cast<int>(plot.top()), 20, static_cast<int>(plot.height()) + 84));
    damage = damage.united(QRect(point.x() - 60, height() - 24, 120, 24));
    return damage.adjusted(-2, -2, 2, 2);
}

void KLineChartWidget::invalidateCache() {
    cacheDirty_ = true;
    update();
}

bool KLineChartWidget::visiblePriceRange(double& minPrice, double& maxPrice) const {
    if (bars_.isEmpty()) {
        return false;
    }
    const int first = firstVisibleBar();
    const int count = visibleBarCount();
    if (count <= 0) {
        return false;
    }
    minPrice = bars_.at(first).low;
    maxPrice = bars_.at(first).high;
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
        return false;
    }
    if (maxPrice <= minPrice) {
        maxPrice = minPrice + 1.0;
        minPrice -= 1.0;
    }
    const double padding = (maxPrice - minPrice) * 0.05;
    minPrice -= padding;
    maxPrice += padding;
    return true;
}

void KLineChartWidget::renderCache() {
    if (size().isEmpty()) {
        return;
    }
    const qreal dpr       = devicePixelRatioF();
    const QSize pixelSize = size() * dpr;
    if (chartCache_.size() != pixelSize || chartCache_.devicePixelRatio() != dpr) {
        chartCache_ = QPixmap(pixelSize);
        chartCache_.setDevicePixelRatio(dpr);
    }
    chartCache_.fill(dark_ ? QColor("#050505") : QColor("#f3f0eb"));

    QPainter painter(&chartCache_);
    painter.setRenderHint(QPainter::Antialiasing, true);

    double    minPrice = 0.0;
    double    maxPrice = 0.0;
    const int first    = firstVisibleBar();
    const int count    = visibleBarCount();
    if (!visiblePriceRange(minPrice, maxPrice) || count <= 0) {
        drawEmpty(painter);
    } else {
        const QRectF plot = plotArea();
        const QRectF volumePlot(plot.left(), plot.bottom() + kAxisGap, plot.width(), kVolumeHeight);
        drawGrid(painter, plot, minPrice, maxPrice);
        drawCandles(painter, plot, minPrice, maxPrice, first, count);
        drawVolume(painter, volumePlot, first, count);
    }

    cacheDirty_ = false;
}

void KLineChartWidget::paintEvent(QPaintEvent* event) {
    if (cacheDirty_ || chartCache_.size() != size() * devicePixelRatioF()) {
        renderCache();
    }

    QPainter painter(this);
    painter.setClipRegion(event->region());
    if (!chartCache_.isNull()) {
        painter.drawPixmap(0, 0, chartCache_);
    }
    painter.setRenderHint(QPainter::Antialiasing, true);
    // Header and crosshair are cheap overlays; only candles/grid/volume are
    // cached. Thus a quote tick repaints text without re-rasterising the chart.
    const QRect damaged = event->rect();
    if (damaged.intersects(headerDamageRect())) {
        drawHeader(painter);
    }
    if (bars_.isEmpty() || crosshair_.x() < 0 ||
        !damaged.intersects(crosshairDamageRect(crosshair_))) {
        return;
    }
    double minPrice = 0.0;
    double maxPrice = 0.0;
    if (!visiblePriceRange(minPrice, maxPrice)) {
        return;
    }
    drawCrosshair(painter, plotArea(), minPrice, maxPrice);
}

void KLineChartWidget::drawHeader(QPainter& painter) {
    painter.setFont(titleFont_);
    painter.setPen(dark_ ? QColor("#f0f0f0") : QColor("#2a2a2a"));
    painter.drawText(QRect(14, 7, 260, 28), Qt::AlignLeft | Qt::AlignVCenter, symbol_);

    painter.setFont(font());
    painter.setPen(dark_ ? QColor("#8f8f8f") : QColor("#6f675f"));
    painter.drawText(QRect(14, 31, 220, 20), Qt::AlignLeft | Qt::AlignVCenter, interval_);

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
    const QColor valueColor = !finite(change) ? (dark_ ? QColor("#8f8f8f") : QColor("#6f675f"))
                                              : (change >= 0.0 ? upColor(dark_) : downColor(dark_));

    const QFontMetrics titleMetrics(titleFont_);
    const QFontMetrics normalMetrics(font());
    const int          leftWidth = std::max(titleMetrics.horizontalAdvance(symbol_),
                                            normalMetrics.horizontalAdvance(interval_));
    const int          priceX    = std::max(170, 14 + leftWidth + 24);
    const QString      lastText  = priceText(last);

    painter.setFont(valueFont_);
    painter.setPen(valueColor);
    painter.drawText(QRect(priceX, 3, 150, 34), Qt::AlignLeft | Qt::AlignVCenter, lastText);

    painter.setFont(font());
    const QString delta = finite(change) ? QStringLiteral("%1%2  %3%4%")
                                               .arg(change >= 0.0 ? "+" : "")
                                               .arg(change, 0, 'f', 2)
                                               .arg(finite(percent) && percent >= 0.0 ? "+" : "")
                                               .arg(percent, 0, 'f', 2)
                                         : QStringLiteral("涨跌 —  —%");
    const int     deltaX =
        priceX + std::max(80, QFontMetrics(valueFont_).horizontalAdvance(lastText)) + 18;
    painter.setPen(valueColor);
    painter.drawText(QRect(deltaX, 7, 260, 26), Qt::AlignLeft | Qt::AlignVCenter, delta);
}

void KLineChartWidget::drawEmpty(QPainter& painter) {
    painter.setPen(dark_ ? QColor("#66717f") : QColor("#8f8f8f"));
    painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("等待 K 线数据"));
}

void KLineChartWidget::drawGrid(QPainter&     painter,
                                const QRectF& plot,
                                double        minPrice,
                                double        maxPrice) const {
    const QColor grid = dark_ ? QColor("#2a2a2a") : QColor("#ded7cc");
    const QColor text = dark_ ? QColor("#9a9a9a") : QColor("#6f675f");
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
    const QColor text = dark_ ? QColor("#9a9a9a") : QColor("#6f675f");

    for (int i = 0; i < count; ++i) {
        const auto& bar = bars_.at(first + i);
        if (!finite(bar.open) || !finite(bar.close)) {
            continue;
        }
        const double x      = plot.left() + step * (i + 0.5);
        const double yOpen  = yFor(bar.open);
        const double yClose = yFor(bar.close);
        const QColor color  = bar.close >= bar.open ? upColor(dark_) : downColor(dark_);

        const double top    = std::min(yOpen, yClose);
        const double bottom = std::max(yOpen, yClose);
        const QRectF body(x - bodyWidth / 2.0, top, bodyWidth, std::max(1.0, bottom - top));

        // Draw the wick above and below the body separately. A single
        // high->low line would run through the hollow interior.
        painter.setPen(QPen(color, 1.0));
        painter.drawLine(QPointF(x, yFor(bar.high)), QPointF(x, top));
        painter.drawLine(QPointF(x, bottom), QPointF(x, yFor(bar.low)));

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
        const QRectF volume(x - barWidth / 2.0, plot.bottom() - h, barWidth, std::max(1.0, h));
        if (bar.close >= bar.open) {
            // Match the hollow rising candle: outline-only red volume bar.
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(upColor(dark_), 1.0));
            painter.drawRect(volume);
        } else {
            painter.setPen(Qt::NoPen);
            painter.setBrush(downColor(dark_));
            painter.drawRect(volume);
        }
    }
}

void KLineChartWidget::drawCrosshair(QPainter&     painter,
                                     const QRectF& plot,
                                     double        minPrice,
                                     double        maxPrice) const {
    if (!plot.contains(crosshair_)) {
        return;
    }
    const QColor line = dark_ ? QColor("#6a6a6a") : QColor("#8a8176");
    painter.setPen(QPen(line, 1.0, Qt::DashLine));
    painter.drawLine(QPointF(crosshair_.x(), plot.top()), QPointF(crosshair_.x(), plot.bottom()));
    painter.drawLine(QPointF(plot.left(), crosshair_.y()), QPointF(plot.right(), crosshair_.y()));

    const double ratio = (plot.bottom() - crosshair_.y()) / plot.height();
    const double value = minPrice + ratio * (maxPrice - minPrice);
    painter.setPen(Qt::NoPen);
    painter.setBrush(dark_ ? QColor("#2a2a2a") : QColor("#ffffff"));
    const QRectF label(plot.right() + 4, crosshair_.y() - 9, 72, 18);
    painter.drawRect(label);
    painter.setPen(dark_ ? QColor("#f0f0f0") : QColor("#2a2a2a"));
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
            painter.fillRect(timeLabel, dark_ ? QColor("#2a2a2a") : QColor("#ffffff"));
            painter.setPen(dark_ ? QColor("#f0f0f0") : QColor("#2a2a2a"));
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
        invalidateCache();
        emit viewportChanged();
        event->accept();
        return;
    }
    pendingCrosshair_ = event->position().toPoint();
    if (crosshairTimer_ != nullptr && !crosshairTimer_->isActive()) {
        crosshairTimer_->start();
    }
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
    invalidateCache();
    emit viewportChanged();
    event->accept();
}

void KLineChartWidget::leaveEvent(QEvent* event) {
    if (crosshairTimer_ != nullptr) {
        crosshairTimer_->stop();
    }
    if (crosshair_.x() >= 0) {
        update(crosshairDamageRect(crosshair_));
    }
    crosshair_        = QPoint(-1, -1);
    pendingCrosshair_ = QPoint(-1, -1);
    if (!dragging_) {
        setCursor(Qt::OpenHandCursor);
    }
    QWidget::leaveEvent(event);
}

} // namespace tqsdk::ui

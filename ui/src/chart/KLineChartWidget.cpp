#include "chart/KLineChartWidget.h"

#include <QDateTime>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>

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
}

void KLineChartWidget::setBars(const QVector<KlineBar>& bars) {
    bars_ = bars;
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

void KLineChartWidget::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QColor background = dark_ ? QColor("#0d1117") : QColor("#f7f9fc");
    const QColor text       = dark_ ? QColor("#9aa4b2") : QColor("#5b6573");
    painter.fillRect(rect(), background);

    QFont titleFont = font();
    titleFont.setPointSizeF(titleFont.pointSizeF() + 2.0);
    titleFont.setBold(true);
    painter.setFont(titleFont);
    painter.setPen(dark_ ? QColor("#e6edf3") : QColor("#202a36"));
    painter.drawText(QRect(14, 8, width() - 28, 26), Qt::AlignLeft | Qt::AlignVCenter, symbol_);

    painter.setFont(font());
    painter.setPen(text);
    painter.drawText(QRect(14, 30, width() - 28, 20), Qt::AlignLeft | Qt::AlignVCenter, interval_);

    if (bars_.isEmpty()) {
        drawEmpty(painter);
        return;
    }

    double minPrice = bars_.front().low;
    double maxPrice = bars_.front().high;
    for (const auto& bar : bars_) {
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

    const QRectF plot(14.0, 56.0, width() - 92.0, height() - 56.0 - 92.0);
    const QRectF volumePlot(plot.left(), plot.bottom() + 8.0, plot.width(), 64.0);
    drawGrid(painter, plot, minPrice, maxPrice);
    drawCandles(painter, plot, minPrice, maxPrice);
    drawVolume(painter, volumePlot);
    drawCrosshair(painter, plot, minPrice, maxPrice);
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
                                   double        maxPrice) const {
    if (bars_.isEmpty() || maxPrice <= minPrice) {
        return;
    }
    const double step      = plot.width() / bars_.size();
    const double bodyWidth = std::max(1.0, step * 0.64);
    const double range     = maxPrice - minPrice;

    auto yFor = [&](double price) {
        return plot.bottom() - (price - minPrice) / range * plot.height();
    };

    const QColor text = dark_ ? QColor("#7f8b99") : QColor("#6b7584");
    painter.setPen(text);
    for (int i = 0; i < bars_.size(); ++i) {
        const auto& bar = bars_.at(i);
        if (!finite(bar.open) || !finite(bar.close)) {
            continue;
        }
        const double x      = plot.left() + step * (i + 0.5);
        const double yOpen  = yFor(bar.open);
        const double yClose = yFor(bar.close);
        const bool   up     = bar.close >= bar.open;
        const QColor color  = up ? upColor(dark_) : downColor(dark_);

        painter.setPen(QPen(color, 1.0));
        painter.drawLine(QPointF(x, yFor(bar.high)), QPointF(x, yFor(bar.low)));

        const double top    = std::min(yOpen, yClose);
        const double bottom = std::max(yOpen, yClose);
        const QRectF body(x - bodyWidth / 2.0, top, bodyWidth, std::max(1.0, bottom - top));
        painter.fillRect(body, color);

        if (i == 0 || i == bars_.size() / 2 || i == bars_.size() - 1) {
            painter.setPen(text);
            const QString label      = timeText(bar.datetime);
            const int     labelWidth = 100;
            painter.drawText(QRectF(x - labelWidth / 2.0, plot.bottom() + 4, labelWidth, 16),
                             Qt::AlignHCenter | Qt::AlignTop,
                             label);
        }
    }
}

void KLineChartWidget::drawVolume(QPainter& painter, const QRectF& plot) const {
    if (bars_.isEmpty()) {
        return;
    }
    quint64 maxVolume = 1;
    for (const auto& bar : bars_) {
        maxVolume = std::max(maxVolume, bar.volume);
    }
    const double step       = plot.width() / bars_.size();
    const double barWidth   = std::max(1.0, step * 0.64);
    const QColor volumeUp   = upColor(dark_);
    const QColor volumeDown = downColor(dark_);

    for (int i = 0; i < bars_.size(); ++i) {
        const auto&  bar = bars_.at(i);
        const double h =
            plot.height() * static_cast<double>(bar.volume) / static_cast<double>(maxVolume);
        const double x = plot.left() + step * (i + 0.5);
        const QRectF rect(x - barWidth / 2.0, plot.bottom() - h, barWidth, std::max(1.0, h));
        painter.fillRect(rect, bar.close >= bar.open ? volumeUp : volumeDown);
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
}

void KLineChartWidget::mouseMoveEvent(QMouseEvent* event) {
    crosshair_ = event->position().toPoint();
    update();
    QWidget::mouseMoveEvent(event);
}

void KLineChartWidget::leaveEvent(QEvent* event) {
    crosshair_ = QPoint(-1, -1);
    update();
    QWidget::leaveEvent(event);
}

} // namespace tqsdk::ui

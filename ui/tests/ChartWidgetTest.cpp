#include "chart/KLineChartWidget.h"

#include <QApplication>
#include <QSignalSpy>
#include <QWheelEvent>
#include <QtTest>

#include <cstdint>

using namespace tqsdk::ui;

class ChartWidgetTest : public QObject {
    Q_OBJECT

private slots:
    void emptyChartIsSafe();
    void viewportCanBeRestored();
    void leftDragMovesViewport();
    void wheelZoomChangesVisibleBars();
    void crosshairPaintDoesNotCrash();

private:
    static QVector<KlineBar> makeBars(int count = 300) {
        QVector<KlineBar> bars;
        bars.reserve(count);
        for (int i = 0; i < count; ++i) {
            const double price = 1000.0 + i * 0.5;
            bars.push_back({int64_t(i) * 60'000'000'000LL, price, price + 2.0, price - 2.0,
                            price + 1.0, quint64(100 + i), 10, 11});
        }
        return bars;
    }
};

void ChartWidgetTest::emptyChartIsSafe() {
    KLineChartWidget chart;
    chart.resize(800, 500);
    chart.show();
    QTest::qWait(10);

    QVERIFY(!chart.grab().isNull());
}

void ChartWidgetTest::viewportCanBeRestored() {
    KLineChartWidget chart;
    chart.resize(900, 600);
    chart.setBars(makeBars());
    chart.setViewport(50, 100, false);

    QCOMPARE(chart.visibleCount(), 50);
    QCOMPARE(chart.visibleStartIndex(), 100);
    QVERIFY(!chart.followLatest());

    chart.setViewport(80, 0, true);
    QCOMPARE(chart.visibleCount(), 80);
    QVERIFY(chart.followLatest());
}

void ChartWidgetTest::leftDragMovesViewport() {
    KLineChartWidget chart;
    chart.resize(900, 600);
    chart.show();
    chart.setBars(makeBars());
    chart.setViewport(50, 100, false);
    QTest::qWait(10);

    QSignalSpy changed(&chart, &KLineChartWidget::viewportChanged);
    const int before = chart.visibleStartIndex();
    QTest::mousePress(&chart, Qt::LeftButton, Qt::NoModifier, QPoint(500, 250));
    QTest::mouseMove(&chart, QPoint(380, 250));
    QTest::mouseRelease(&chart, Qt::LeftButton, Qt::NoModifier, QPoint(380, 250));

    QVERIFY(chart.visibleStartIndex() > before);
    QVERIFY(changed.count() >= 1);
}

void ChartWidgetTest::wheelZoomChangesVisibleBars() {
    KLineChartWidget chart;
    chart.resize(900, 600);
    chart.show();
    chart.setBars(makeBars());
    chart.setViewport(80, 100, false);
    QTest::qWait(10);

    const int before = chart.visibleCount();
    QWheelEvent wheel(QPointF(450, 250), QPointF(chart.mapToGlobal(QPoint(450, 250))),
                      QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
                      Qt::NoScrollPhase, false);
    QApplication::sendEvent(&chart, &wheel);
    QVERIFY(chart.visibleCount() < before);
}

void ChartWidgetTest::crosshairPaintDoesNotCrash() {
    KLineChartWidget chart;
    chart.resize(900, 600);
    chart.show();
    chart.setBars(makeBars());
    chart.setViewport(80, 100, false);
    QTest::qWait(10);

    QTest::mouseMove(&chart, QPoint(450, 260));
    const QPixmap rendered = chart.grab();
    QVERIFY(!rendered.isNull());
    QVERIFY(rendered.width() > 0);
    QVERIFY(rendered.height() > 0);
}

QTEST_MAIN(ChartWidgetTest)
#include "ChartWidgetTest.moc"

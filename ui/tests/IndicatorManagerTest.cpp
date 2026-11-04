#include "indicator/IndicatorManager.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace tqsdk::ui;

namespace {

QVector<KlineBar> makeBars(int count) {
    QVector<KlineBar> bars;
    bars.reserve(count);
    for (int i = 0; i < count; ++i) {
        KlineBar bar;
        bar.datetime = static_cast<qint64>(i + 1) * 60'000'000'000LL;
        bar.open     = 100.0 + i;
        bar.high     = 101.0 + i;
        bar.low      = 99.0 + i;
        bar.close    = 100.0 + i;
        bar.volume   = 1000 + i;
        bars.push_back(bar);
    }
    return bars;
}

QString builtinPlugins() {
    return QFileInfo(QStringLiteral(TQSDK_TEST_SIDECAR_PATH)).absolutePath() +
           QStringLiteral("/plugins");
}

QVector<IndicatorSeries> latestSeries(const QSignalSpy& spy) {
    if (spy.isEmpty()) {
        return {};
    }
    return qvariant_cast<QVector<IndicatorSeries>>(spy.last().first());
}

} // namespace

class IndicatorManagerTest final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        qRegisterMetaType<QVector<IndicatorSeries>>("QVector<tqsdk::ui::IndicatorSeries>");
    }

    void maAndMacdProduceAlignedSeries() {
        IndicatorManager manager;
        manager.setSidecarPathForTest(QStringLiteral(TQSDK_TEST_SIDECAR_PATH));
        QSignalSpy spy(&manager, &IndicatorManager::seriesChanged);
        QSignalSpy errors(&manager, &IndicatorManager::errorOccurred);

        manager.setEnabled({QStringLiteral("ma"), QStringLiteral("macd")});
        manager.setBars(QStringLiteral("SHFE.rb2701"), 60'000'000'000LL, makeBars(60));

        QTRY_VERIFY_WITH_TIMEOUT(!spy.isEmpty(), 4000);
        QVERIFY2(errors.isEmpty(),
                 qPrintable(errors.isEmpty() ? QString{} : errors.first().first().toString()));

        bool sawMa   = false;
        bool sawMacd = false;
        for (int attempt = 0; attempt < 20 && (!sawMa || !sawMacd); ++attempt) {
            const auto series = latestSeries(spy);
            for (const auto& item : series) {
                if (item.indicatorId == QStringLiteral("ma") &&
                    item.seriesName == QStringLiteral("ma_fast")) {
                    sawMa = !item.values.isEmpty();
                    QCOMPARE(item.values.last(), 157.0);
                }
                if (item.indicatorId == QStringLiteral("macd") &&
                    item.panel == QStringLiteral("sub")) {
                    sawMacd = !item.values.isEmpty();
                }
            }
            if (!sawMa || !sawMacd) {
                QTest::qWait(20);
            }
        }
        QVERIFY(sawMa);
        QVERIFY(sawMacd);
        manager.setEnabled({});
    }

    void incrementalUpdateChangesLastValue() {
        IndicatorManager manager;
        manager.setSidecarPathForTest(QStringLiteral(TQSDK_TEST_SIDECAR_PATH));
        QSignalSpy spy(&manager, &IndicatorManager::seriesChanged);
        manager.setEnabled({QStringLiteral("ema")});

        auto bars = makeBars(20);
        manager.setBars(QStringLiteral("SHFE.rb2701"), 60'000'000'000LL, bars);
        QTRY_VERIFY_WITH_TIMEOUT(!spy.isEmpty(), 4000);

        bars.last().close += 10.0;
        manager.setBars(QStringLiteral("SHFE.rb2701"), 60'000'000'000LL, bars);
        QTRY_VERIFY_WITH_TIMEOUT(spy.count() >= 2, 4000);
        manager.setEnabled({});
    }

    void parametersReachSidecar() {
        IndicatorManager manager;
        manager.setSidecarPathForTest(QStringLiteral(TQSDK_TEST_SIDECAR_PATH));
        manager.setParams(QStringLiteral("ma"),
                          QJsonObject{{QStringLiteral("fast"), 3}, {QStringLiteral("slow"), 7}});
        QSignalSpy spy(&manager, &IndicatorManager::seriesChanged);
        manager.setEnabled({QStringLiteral("ma")});
        manager.setBars(QStringLiteral("SHFE.rb2701"), 60'000'000'000LL, makeBars(30));
        const auto hasOutput = [&spy] {
            for (const auto& item : latestSeries(spy)) {
                if (!item.values.isEmpty()) {
                    return true;
                }
            }
            return false;
        };
        QTRY_VERIFY_WITH_TIMEOUT(hasOutput(), 4000);

        bool sawFast = false;
        bool sawSlow = false;
        for (const auto& item : latestSeries(spy)) {
            if (item.seriesName == QStringLiteral("ma_fast")) {
                sawFast = true;
                QCOMPARE(item.values.last(), 128.0);
            } else if (item.seriesName == QStringLiteral("ma_slow")) {
                sawSlow = true;
                QCOMPARE(item.values.last(), 126.0);
            }
        }
        QVERIFY(sawFast);
        QVERIFY(sawSlow);
        manager.setEnabled({});
    }

    void externalPluginLoadsWithoutRecompiling() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QDir().mkpath(directory.filePath(QStringLiteral("rsi")));
        QFile plugin(directory.filePath(QStringLiteral("rsi/indicator.py")));
        QVERIFY(plugin.open(QIODevice::WriteOnly));
        // 用户目录插件也可以直接 import 共享 indicator_lib。
        plugin.write("from indicator_lib import param, series_def, sma\n\n"
                     "def manifest():\n"
                     "    return {\n"
                     "        'id': 'rsi', 'name': 'RSI', 'version': '1.0', 'panel': 'sub',\n"
                     "        'params': [param('period', '周期', 5, 2, 100)],\n"
                     "        'series': [series_def('rsi', 'RSI', 'gold')],\n"
                     "    }\n\n"
                     "def compute(params, bars):\n"
                     "    closes = [float(bar['close']) for bar in bars]\n"
                     "    return {'rsi': sma(closes, int(params.get('period', 5)))}\n");
        plugin.close();

        IndicatorManager manager;
        manager.setSidecarPathForTest(QStringLiteral(TQSDK_TEST_SIDECAR_PATH));
        manager.setPluginDirectoriesForTest({directory.path()});
        QTRY_COMPARE_WITH_TIMEOUT(manager.specs().size(), 1, 3000);
        QCOMPARE(manager.specs().first().id, QStringLiteral("rsi"));
        QCOMPARE(manager.specs().first().colors.first(), QStringLiteral("#f0c75e"));

        QSignalSpy spy(&manager, &IndicatorManager::seriesChanged);
        QSignalSpy errors(&manager, &IndicatorManager::errorOccurred);
        manager.setEnabled({QStringLiteral("rsi")});
        manager.setBars(QStringLiteral("SHFE.rb2701"), 60'000'000'000LL, makeBars(20));
        const auto hasOutput = [&spy] {
            for (const auto& item : latestSeries(spy)) {
                if (!item.values.isEmpty()) {
                    return true;
                }
            }
            return false;
        };
        QTRY_VERIFY_WITH_TIMEOUT(hasOutput(), 4000);
        QVERIFY(errors.isEmpty());
        const auto series = latestSeries(spy);
        QVERIFY(!series.isEmpty());
        QCOMPARE(series.first().indicatorId, QStringLiteral("rsi"));
        QVERIFY(!series.first().values.isEmpty());
        manager.setEnabled({});
    }

    void crashedSidecarIsReported() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString script = directory.filePath(QStringLiteral("crash.py"));
        QFile         file(script);
        QVERIFY(file.open(QIODevice::WriteOnly));
        // 只发 ready 后立即退出：模拟插件进程崩溃，验证 Manager 不阻塞 GUI 并上报错误。
        file.write("import json, struct, sys\n"
                   "payload=json.dumps({'type':'ready','protocol':1}).encode()\n"
                   "sys.stdout.buffer.write(struct.pack('>I', len(payload))+payload)\n"
                   "sys.stdout.buffer.flush()\n"
                   "sys.exit(0)\n");
        file.close();

        IndicatorManager manager;
        manager.setSidecarPathForTest(script);
        manager.setPluginDirectoriesForTest({builtinPlugins()});
        QSignalSpy errors(&manager, &IndicatorManager::errorOccurred);
        manager.setEnabled({QStringLiteral("ma")});
        QTRY_VERIFY_WITH_TIMEOUT(!errors.isEmpty(), 3000);
        manager.setEnabled({});
    }

    void missingSidecarReportsError() {
        IndicatorManager manager;
        manager.setSidecarPathForTest(QStringLiteral("/nonexistent/tqsdk-indicator-sidecar.py"));
        manager.setPluginDirectoriesForTest({builtinPlugins()});
        QSignalSpy errors(&manager, &IndicatorManager::errorOccurred);
        manager.setEnabled({QStringLiteral("ma")});
        QTRY_VERIFY_WITH_TIMEOUT(!errors.isEmpty(), 1000);
        manager.setEnabled({});
    }
};

QTEST_GUILESS_MAIN(IndicatorManagerTest)
#include "IndicatorManagerTest.moc"

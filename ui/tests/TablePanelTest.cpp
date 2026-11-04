#include "widgets/TablePanel.h"

#include <QHeaderView>
#include <QSettings>
#include <QStandardItemModel>
#include <QTableView>
#include <QTemporaryDir>
#include <QtTest>

using namespace tqsdk::ui;

class TablePanelTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void columnStateSurvivesRecreation();
    void hiddenColumnCanBeRestored();
    void cleanup();

private:
    QTemporaryDir settingsDir_;
};

void TablePanelTest::initTestCase() {
    QVERIFY(settingsDir_.isValid());
    QCoreApplication::setOrganizationName(QStringLiteral("TQSDK-UI-Test"));
    QCoreApplication::setApplicationName(QStringLiteral("TablePanelTest"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir_.path());
}

void TablePanelTest::cleanup() {
    QSettings settings;
    settings.clear();
    settings.sync();
}

void TablePanelTest::columnStateSurvivesRecreation() {
    {
        QStandardItemModel model(3, 4);
        for (int column = 0; column < 4; ++column) {
            model.setHeaderData(column, Qt::Horizontal, QStringLiteral("C%1").arg(column));
        }
        TablePanel panel(&model);
        panel.installColumnMenu(QStringLiteral("test-table"));
        panel.resize(720, 320);
        panel.show();
        QTest::qWait(10);

        auto* header = panel.view()->horizontalHeader();
        QVERIFY(header->sectionsMovable());
        header->moveSection(header->visualIndex(3), 0);
        header->resizeSection(1, 177);
        panel.view()->setColumnHidden(2, true);
        QSettings().sync();
    }

    {
        QStandardItemModel model(3, 4);
        for (int column = 0; column < 4; ++column) {
            model.setHeaderData(column, Qt::Horizontal, QStringLiteral("C%1").arg(column));
        }
        TablePanel panel(&model);
        panel.installColumnMenu(QStringLiteral("test-table"));
        auto* header = panel.view()->horizontalHeader();

        QCOMPARE(header->visualIndex(3), 0);
        QCOMPARE(header->sectionSize(1), 177);
        QVERIFY(panel.view()->isColumnHidden(2));
    }
}

void TablePanelTest::hiddenColumnCanBeRestored() {
    QStandardItemModel model(1, 3);
    for (int column = 0; column < 3; ++column) {
        model.setHeaderData(column, Qt::Horizontal, QStringLiteral("H%1").arg(column));
    }
    {
        TablePanel panel(&model);
        panel.installColumnMenu(QStringLiteral("hidden-table"));
        panel.view()->setColumnHidden(1, true);
        QSettings().sync();
    }

    // Recreate with the same settings key after the first panel is destroyed;
    // its destructor flushes the hidden-column state.
    TablePanel restored(&model);
    restored.installColumnMenu(QStringLiteral("hidden-table"));
    QVERIFY(restored.view()->isColumnHidden(1));
}

QTEST_MAIN(TablePanelTest)
#include "TablePanelTest.moc"

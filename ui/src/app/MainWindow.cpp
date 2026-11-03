#include "app/MainWindow.h"

#include <QAbstractItemView>
#include <QDockWidget>
#include <QHeaderView>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSize>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QToolBar>
#include <QVBoxLayout>

namespace {

QLabel* placeholder(const QString& text, QWidget* parent = nullptr) {
    auto* label = new QLabel(text, parent);
    label->setAlignment(Qt::AlignCenter);
    label->setWordWrap(true);
    label->setStyleSheet("color: #8b93a3; padding: 24px;");
    return label;
}

QWidget* panel(const QString& title, QWidget* content, QWidget* parent = nullptr) {
    auto* box = new QWidget(parent);
    auto* layout = new QVBoxLayout(box);
    layout->setContentsMargins(8, 8, 8, 8);
    auto* heading = new QLabel(title, box);
    heading->setStyleSheet("font-weight: 600; padding: 4px 2px;");
    layout->addWidget(heading);
    layout->addWidget(content, 1);
    return box;
}

QTableWidget* emptyTable(const QStringList& headers, QWidget* parent = nullptr) {
    auto* table = new QTableWidget(0, headers.size(), parent);
    table->setHorizontalHeaderLabels(headers);
    table->horizontalHeader()->setStretchLastSection(true);
    table->verticalHeader()->setVisible(false);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    return table;
}

} // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(tr("TQSDK Terminal (Qt)"));
    resize(1440, 900);
    buildUi();
}

void MainWindow::buildUi() {
    buildToolbar();
    buildWorkspace();
    buildBottomDock();

    statusBar()->showMessage(tr("Qt UI Phase 0 · 等待连接 node"));
}

void MainWindow::buildToolbar() {
    auto* toolbar = addToolBar(tr("主工具栏"));
    toolbar->setMovable(false);
    toolbar->setIconSize(QSize(18, 18));

    auto* brand = new QLabel(tr("TQSDK"), toolbar);
    brand->setStyleSheet("font-size: 15px; font-weight: 700; padding: 0 10px;");
    toolbar->addWidget(brand);

    auto* search = new QLineEdit(toolbar);
    search->setPlaceholderText(tr("合约代码 / 名称"));
    search->setMinimumWidth(260);
    toolbar->addWidget(search);

    toolbar->addSeparator();
    auto* interval = new QLabel(tr("周期"), toolbar);
    interval->setStyleSheet("padding: 0 6px;");
    toolbar->addWidget(interval);
    for (const auto* text : {"1m", "5m", "15m", "1h", "1d"}) {
        auto* button = new QPushButton(text, toolbar);
        button->setFlat(true);
        toolbar->addWidget(button);
    }

    auto* spacer = new QWidget(toolbar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar->addWidget(spacer);

    connectionLabel_ = new QLabel(tr("● 未连接"), toolbar);
    connectionLabel_->setStyleSheet("color: #e7b35a; padding: 0 10px;");
    toolbar->addWidget(connectionLabel_);

    auto* theme = new QPushButton(tr("浅色"), toolbar);
    theme->setFlat(true);
    toolbar->addWidget(theme);
}

void MainWindow::buildWorkspace() {
    auto* splitter = new QSplitter(Qt::Horizontal, this);

    auto* watchlist = new QListWidget(splitter);
    for (const auto* name : {"SHFE.rb2701", "DCE.m2701", "CZCE.SA701", "CFFEX.IF2612"}) {
        watchlist->addItem(name);
    }
    splitter->addWidget(panel(tr("自选 / 最近"), watchlist, splitter));

    centerTabs_ = new QTabWidget(splitter);
    centerTabs_->addTab(placeholder(tr("K 线图\nPhase 1: QPainter 自绘 K 线")), tr("交易终端"));
    centerTabs_->addTab(placeholder(tr("期货列表\nPhase 1: QTableView + 合约目录")), tr("期货列表"));
    centerTabs_->addTab(placeholder(tr("期权链 / T 型报价\nPhase 2")), tr("期权链"));
    splitter->addWidget(centerTabs_);

    auto* ticket = new QWidget(splitter);
    auto* ticketLayout = new QFormLayout(ticket);
    ticketLayout->setContentsMargins(14, 14, 14, 14);
    ticketLayout->addRow(tr("合约"), new QLabel(tr("—"), ticket));
    ticketLayout->addRow(tr("方向"), new QLabel(tr("买入"), ticket));
    ticketLayout->addRow(tr("开平"), new QLabel(tr("开仓"), ticket));
    ticketLayout->addRow(tr("价格"), new QLabel(tr("—"), ticket));
    ticketLayout->addRow(tr("数量"), new QLabel(tr("1"), ticket));
    auto* submit = new QPushButton(tr("等待交易账户"), ticket);
    submit->setEnabled(false);
    ticketLayout->addRow(submit);
    splitter->addWidget(panel(tr("交易下单"), ticket, splitter));

    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 5);
    splitter->setStretchFactor(2, 1);
    setCentralWidget(splitter);
}

void MainWindow::buildBottomDock() {
    auto* dock = new QDockWidget(tr("交易与账户"), this);
    dock->setAllowedAreas(Qt::BottomDockWidgetArea);
    auto* tabs = new QTabWidget(dock);
    tabs->addTab(emptyTable({tr("合约"), tr("方向"), tr("手数"), tr("开仓价"), tr("浮动盈亏")}),
                 tr("持仓"));
    tabs->addTab(emptyTable({tr("订单号"), tr("合约"), tr("方向"), tr("价格"), tr("状态")}),
                 tr("委托"));
    tabs->addTab(emptyTable({tr("成交号"), tr("合约"), tr("方向"), tr("价格"), tr("数量")}),
                 tr("成交"));
    dock->setWidget(tabs);
    addDockWidget(Qt::BottomDockWidgetArea, dock);
    dock->setMaximumHeight(230);
}

#pragma once

#include <QString>

namespace tqsdk::ui {

inline QString themeStyle(bool dark) {
    const QString bg       = dark ? "#0d1117" : "#f5f7fb";
    const QString panel    = dark ? "#111821" : "#ffffff";
    const QString text     = dark ? "#d9e2ec" : "#202a36";
    const QString muted    = dark ? "#8b93a3" : "#657080";
    const QString line     = dark ? "#263140" : "#dce3ec";
    const QString selected = dark ? "#1c2a3a" : "#e6f0ff";
    const QString accent   = dark ? "#2f81f7" : "#0b66c3";
    return QStringLiteral(
               "QMainWindow, QWidget { background: %1; color: %3; }"
               "QToolBar { background: %2; border: none; spacing: 4px; }"
               "QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox { background: %2; "
               "border: 1px solid %4; border-radius: 4px; padding: 5px 8px; color: %3; }"
               "QPushButton { background: transparent; border: 1px solid transparent; "
               "border-radius: 4px; padding: 5px 8px; color: %5; }"
               "QPushButton:hover { background: %6; color: %3; }"
               "QPushButton:disabled { color: %4; }"
               "QTableView { background: %2; alternate-background-color: %7; "
               "border: 1px solid %4; gridline-color: %4; selection-background-color: %6; "
               "selection-color: %3; }"
               "QHeaderView::section { background: %2; color: %5; border: none; "
               "border-bottom: 1px solid %4; padding: 6px; }"
               "QTabWidget::pane { border: 1px solid %4; }"
               "QTabBar::tab { padding: 7px 14px; color: %5; }"
               "QTabBar::tab:selected { color: %3; border-bottom: 2px solid %8; }"
               "QDockWidget::title { background: %2; padding: 6px; }"
               "QStatusBar { color: %5; }")
        .arg(bg, panel, text, line, muted, selected, bg, accent);
}

} // namespace tqsdk::ui

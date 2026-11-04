#pragma once

#include <QColor>
#include <QPalette>

namespace tqsdk::ui {

/// 全局主题使用 QPalette 而不是 qApp->setStyleSheet()。
///
/// A non-empty application stylesheet installs QStyleSheetStyle for *every*
/// widget; in perf profiles its renderRule()/QHash path was a major steady
/// state cost even when the rule did not match. QPalette gives us the same
/// dark/light base colors while leaving high-frequency widgets on Fusion's
/// fast C++ path.
inline QPalette applicationPalette(bool dark) {
    const QColor background = dark ? QColor("#0d1117") : QColor("#f5f7fb");
    const QColor panel      = dark ? QColor("#111821") : QColor("#ffffff");
    const QColor text       = dark ? QColor("#d9e2ec") : QColor("#202a36");
    const QColor muted      = dark ? QColor("#8b93a3") : QColor("#657080");
    const QColor line       = dark ? QColor("#263140") : QColor("#dce3ec");
    const QColor selected   = dark ? QColor("#1c2a3a") : QColor("#e6f0ff");
    const QColor accent     = dark ? QColor("#2f81f7") : QColor("#0b66c3");

    QPalette palette;
    palette.setColor(QPalette::Window, background);
    palette.setColor(QPalette::WindowText, text);
    palette.setColor(QPalette::Base, panel);
    palette.setColor(QPalette::AlternateBase, background);
    palette.setColor(QPalette::Text, text);
    palette.setColor(QPalette::Button, panel);
    palette.setColor(QPalette::ButtonText, text);
    palette.setColor(QPalette::BrightText, QColor("#ffffff"));
    palette.setColor(QPalette::Highlight, selected);
    palette.setColor(QPalette::HighlightedText, text);
    palette.setColor(QPalette::ToolTipBase, panel);
    palette.setColor(QPalette::ToolTipText, text);
    palette.setColor(QPalette::PlaceholderText, muted);
    palette.setColor(QPalette::Link, accent);
    palette.setColor(QPalette::Light, line);
    palette.setColor(QPalette::Midlight, line);
    palette.setColor(QPalette::Mid, line);
    palette.setColor(QPalette::Dark, background);
    palette.setColor(QPalette::Shadow, QColor("#000000"));
    palette.setColor(QPalette::Disabled, QPalette::WindowText, muted);
    palette.setColor(QPalette::Disabled, QPalette::Text, muted);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, muted);
    return palette;
}

} // namespace tqsdk::ui

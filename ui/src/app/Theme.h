#pragma once

#include <QColor>
#include <QPalette>

namespace tqsdk::ui {

/// 全局主题使用 QPalette 而不是 qApp->setStyleSheet()。
///
/// A non-empty application stylesheet installs QStyleSheetStyle for *every*
/// widget; in perf profiles its renderRule()/QHash path was a major steady
/// state cost even when the rule did not match. QPalette gives us the same
/// Wenhua-like dark/light base colors while leaving high-frequency widgets on
/// Fusion's fast C++ path.
inline QPalette applicationPalette(bool dark) {
    const QColor background = dark ? QColor("#050505") : QColor("#f3f0eb");
    const QColor panel      = dark ? QColor("#0b0b0b") : QColor("#ffffff");
    const QColor elevated   = dark ? QColor("#141414") : QColor("#faf8f4");
    const QColor text        = dark ? QColor("#e6e6e6") : QColor("#242424");
    const QColor muted       = dark ? QColor("#8f8f8f") : QColor("#6f675f");
    const QColor line        = dark ? QColor("#2b2b2b") : QColor("#ded7cc");
    const QColor selected    = dark ? QColor("#3b2020") : QColor("#f3dede");
    const QColor accent      = dark ? QColor("#d94a4a") : QColor("#b43a3a");

    QPalette palette;
    palette.setColor(QPalette::Window, background);
    palette.setColor(QPalette::WindowText, text);
    palette.setColor(QPalette::Base, panel);
    palette.setColor(QPalette::AlternateBase, elevated);
    palette.setColor(QPalette::Text, text);
    palette.setColor(QPalette::Button, panel);
    palette.setColor(QPalette::ButtonText, text);
    palette.setColor(QPalette::BrightText, QColor("#ffffff"));
    palette.setColor(QPalette::Highlight, selected);
    palette.setColor(QPalette::HighlightedText, text);
    palette.setColor(QPalette::ToolTipBase, elevated);
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

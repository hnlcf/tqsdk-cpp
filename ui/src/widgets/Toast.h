#pragma once

#include <QLabel>

class QGraphicsOpacityEffect;
class QPropertyAnimation;
class QTimer;

namespace tqsdk::ui {

/// 轻量、不阻塞交互的窗口顶部提示。
///
/// 错误回报使用红色提示，约 3.5 秒后自动淡出；不抢占焦点、不拦截鼠标。
class Toast final : public QLabel {
    Q_OBJECT

public:
    explicit Toast(QWidget* parent);
    void showMessage(const QString& text, bool error = true);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void reposition();

    QGraphicsOpacityEffect* effect_   = nullptr;
    QPropertyAnimation*     fade_     = nullptr;
    QTimer*                 hideTimer_ = nullptr;
};

} // namespace tqsdk::ui

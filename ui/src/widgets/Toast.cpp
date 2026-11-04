#include "widgets/Toast.h"

#include <QEvent>
#include <QFontMetrics>
#include <QGraphicsOpacityEffect>
#include <QPropertyAnimation>
#include <QTimer>

#include <algorithm>

namespace tqsdk::ui {

Toast::Toast(QWidget* parent)
    : QLabel(parent) {
    setObjectName(QStringLiteral("toastMessage"));
    setAlignment(Qt::AlignCenter);
    setWordWrap(true);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setVisible(false);

    effect_ = new QGraphicsOpacityEffect(this);
    setGraphicsEffect(effect_);
    fade_ = new QPropertyAnimation(effect_, "opacity", this);
    fade_->setDuration(420);
    fade_->setStartValue(1.0);
    fade_->setEndValue(0.0);
    connect(fade_, &QPropertyAnimation::finished, this, [this] { hide(); });

    hideTimer_ = new QTimer(this);
    hideTimer_->setSingleShot(true);
    connect(hideTimer_, &QTimer::timeout, this, [this] { fade_->start(); });

    if (parent != nullptr) {
        parent->installEventFilter(this);
    }
}

void Toast::showMessage(const QString& text, bool error) {
    if (text.trimmed().isEmpty()) {
        return;
    }
    fade_->stop();
    hideTimer_->stop();
    effect_->setOpacity(1.0);

    setText(text);
    setStyleSheet(error ? QStringLiteral("QLabel { background:#4a1f1f; color:#ffffff; "
                                         "border:1px solid #d94a4a; border-radius:4px; "
                                         "padding:9px 16px; font-weight:600; }")
                        : QStringLiteral("QLabel { background:#252a24; color:#ffffff; "
                                         "border:1px solid #5d8a54; border-radius:4px; "
                                         "padding:9px 16px; }"));

    const int maxWidth = std::max(300, parentWidget() != nullptr ? parentWidget()->width() - 80 : 600);
    const QFontMetrics metrics(font());
    const QRect bounds = metrics.boundingRect(QRect(0, 0, maxWidth, 2000),
                                              Qt::TextWordWrap,
                                              text);
    setFixedWidth(std::clamp(bounds.width() + 36, 320, maxWidth));
    adjustSize();
    reposition();
    show();
    raise();
    hideTimer_->start(3500);
}

void Toast::reposition() {
    if (parentWidget() == nullptr) {
        return;
    }
    const int x = std::max(8, (parentWidget()->width() - width()) / 2);
    move(x, 10);
}

bool Toast::eventFilter(QObject* watched, QEvent* event) {
    if (watched == parentWidget() && event->type() == QEvent::Resize && isVisible()) {
        reposition();
    }
    return QLabel::eventFilter(watched, event);
}

} // namespace tqsdk::ui

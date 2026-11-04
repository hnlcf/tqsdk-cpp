#pragma once

#include <QToolBar>

class QCompleter;
class QLabel;
class QLineEdit;
class QMenu;
class QPushButton;
class QStringListModel;

namespace tqsdk::ui {

/// 主工具栏：搜索、加入自选、组件菜单、周期、连接状态和主题。
///
/// 不持有 NodeClient/模型；用户动作通过信号交给 MainWindow controller。
class MarketToolBar final : public QToolBar {
    Q_OBJECT

public:
    explicit MarketToolBar(QWidget* parent = nullptr);

    void setCompletions(const QStringList& completions);
    void setComponentsMenu(QMenu* menu);
    void setConnectionState(const QString& text, const QString& color);
    void setThemeLabel(bool dark);
    void clearSearch();
    QString searchText() const;

signals:
    void searchTextChanged(const QString& text);
    void symbolSubmitted(const QString& input);
    void addRequested(const QString& input);
    void intervalRequested(qint64 durationNs, const QString& label);
    void themeToggleRequested();

private:
    QLineEdit*        search_       = nullptr;
    QCompleter*       completer_    = nullptr;
    QStringListModel* completions_  = nullptr;
    QPushButton*      addButton_    = nullptr;
    QPushButton*      components_   = nullptr;
    QLabel*           connection_   = nullptr;
    QPushButton*      themeButton_  = nullptr;
};

} // namespace tqsdk::ui

#pragma once

#include <QMainWindow>

class QLabel;
class QTabWidget;

/// Phase 0 Qt shell.
///
/// The window intentionally only owns layout/placeholders. Network, models and
/// charts are introduced behind NodeRestClient / NodeStreamClient in Phase 1 so
/// the UI never needs to link tqsdk_core.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override = default;

private:
    void buildUi();
    void buildToolbar();
    void buildWorkspace();
    void buildBottomDock();

    QLabel*     connectionLabel_ = nullptr;
    QTabWidget* centerTabs_      = nullptr;
};

#pragma once

#include "indicator/IndicatorTypes.h"

#include <QDialog>
#include <QHash>

class QDoubleSpinBox;

namespace tqsdk::ui {

/// 通用数值指标参数编辑框。后续插件若需要枚举/字符串参数，可扩展参数类型。
class IndicatorSettingsDialog final : public QDialog {
    Q_OBJECT

public:
    IndicatorSettingsDialog(const IndicatorSpec& spec,
                            const QJsonObject&   current,
                            QWidget*             parent = nullptr);

    QJsonObject values() const;

private:
    IndicatorSpec                   spec_;
    QHash<QString, QDoubleSpinBox*> spins_;
};

} // namespace tqsdk::ui

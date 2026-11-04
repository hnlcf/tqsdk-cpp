#include "indicator/IndicatorSettingsDialog.h"

#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QJsonValue>

namespace tqsdk::ui {

IndicatorSettingsDialog::IndicatorSettingsDialog(const IndicatorSpec& spec,
                                                 const QJsonObject&   current,
                                                 QWidget*             parent)
    : QDialog(parent),
      spec_(spec) {
    setWindowTitle(tr("%1 参数").arg(spec.name));
    auto* form = new QFormLayout(this);

    for (const auto& param : spec.params) {
        auto* spin = new QDoubleSpinBox(this);
        spin->setRange(param.minimum, param.maximum);
        spin->setDecimals(param.integer ? 0 : param.decimals);
        spin->setSingleStep(param.integer ? 1.0 : 0.1);
        const QJsonValue saved = current.value(param.key);
        spin->setValue(saved.isDouble() ? saved.toDouble() : param.defaultValue);
        spins_.insert(param.key, spin);
        form->addRow(param.label, spin);
    }

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    form->addRow(buttons);
}

QJsonObject IndicatorSettingsDialog::values() const {
    QJsonObject result;
    for (auto it = spins_.begin(); it != spins_.end(); ++it) {
        result.insert(it.key(), it.value()->value());
    }
    return result;
}

} // namespace tqsdk::ui

#pragma once

#include <QColor>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace tqsdk::ui {

/// 一个指标参数的可见定义。P3 先支持数值参数，后续可扩展枚举/字符串。
struct IndicatorParamSpec {
    QString key;
    QString label;
    double  defaultValue = 0.0;
    double  minimum      = 0.0;
    double  maximum      = 1.0e9;
    int     decimals     = 0;
    bool    integer      = true;
};

struct IndicatorSpec {
    QString                     id;
    QString                     name;
    QString                     panel; // main | sub
    QVector<IndicatorParamSpec> params;
    QStringList                 colors;
    QVector<QString>            seriesNames;

    QJsonObject defaults() const {
        QJsonObject object;
        for (const auto& param : params) {
            object.insert(param.key, param.defaultValue);
        }
        return object;
    }
};

/// sidecar 输出的一条数值序列，datetime 与 K 线时间戳严格对齐。
struct IndicatorSeries {
    QString         indicatorId;
    QString         indicatorName;
    QString         panel;
    QString         seriesName;
    QColor          color;
    QVector<qint64> datetimes;
    QVector<double> values;
    QString         error;
};

} // namespace tqsdk::ui

Q_DECLARE_METATYPE(tqsdk::ui::IndicatorSeries)
Q_DECLARE_METATYPE(QVector<tqsdk::ui::IndicatorSeries>)

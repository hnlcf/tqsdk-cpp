#include "indicator/IndicatorManager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSet>
#include <QStandardPaths>
#include <QTimer>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <limits>

namespace tqsdk::ui {
namespace {

constexpr int kMaxSidecarBytes   = 8 * 1024 * 1024;
constexpr int kResponseTimeoutMs = 3000;

QJsonArray barsToJson(const QVector<KlineBar>& bars, int start) {
    QJsonArray array;
    for (int i = std::max(0, start); i < bars.size(); ++i) {
        const auto& bar = bars.at(i);
        QJsonObject object;
        object.insert(QStringLiteral("datetime"), static_cast<double>(bar.datetime));
        object.insert(QStringLiteral("open"), bar.open);
        object.insert(QStringLiteral("high"), bar.high);
        object.insert(QStringLiteral("low"), bar.low);
        object.insert(QStringLiteral("close"), bar.close);
        object.insert(QStringLiteral("volume"), static_cast<double>(bar.volume));
        object.insert(QStringLiteral("open_oi"), static_cast<double>(bar.openOi));
        object.insert(QStringLiteral("close_oi"), static_cast<double>(bar.closeOi));
        array.append(object);
    }
    return array;
}

/// 从 manifest 构建可显示/可参数化的 spec。manifest 是动态插件与 UI 的唯一接口，
/// 新指标不需要修改 C++ 代码。
IndicatorSpec parseManifest(const QFileInfo& file, bool& ok) {
    ok = false;
    IndicatorSpec spec;
    QFile         handle(file.absoluteFilePath());
    if (!handle.open(QIODevice::ReadOnly)) {
        return spec;
    }
    const QJsonDocument document = QJsonDocument::fromJson(handle.readAll());
    if (!document.isObject()) {
        return spec;
    }
    const QJsonObject object = document.object();
    spec.id                  = object.value(QStringLiteral("id")).toString();
    spec.name                = object.value(QStringLiteral("name")).toString(spec.id);
    spec.panel = object.value(QStringLiteral("panel")).toString(QStringLiteral("sub"));
    if (spec.id.isEmpty() ||
        spec.panel != QStringLiteral("main") && spec.panel != QStringLiteral("sub")) {
        return spec;
    }
    if (spec.id.contains(QLatin1Char('/')) || spec.id.contains(QLatin1Char('\\'))) {
        return spec;
    }

    static const QStringList fallbackColors = {QStringLiteral("#f0c75e"),
                                               QStringLiteral("#53b7ff"),
                                               QStringLiteral("#f28e8e"),
                                               QStringLiteral("#c792ea"),
                                               QStringLiteral("#23b39a")};
    const QJsonArray         params         = object.value(QStringLiteral("params")).toArray();
    for (const auto& item : params) {
        const QJsonObject param = item.toObject();
        const QString     key   = param.value(QStringLiteral("key")).toString();
        if (key.isEmpty()) {
            continue;
        }
        IndicatorParamSpec definition;
        definition.key          = key;
        definition.label        = param.value(QStringLiteral("label")).toString(key);
        definition.defaultValue = param.value(QStringLiteral("default")).toDouble(0.0);
        definition.minimum      = param.value(QStringLiteral("minimum"))
                                      .toDouble(param.value(QStringLiteral("min")).toDouble(0.0));
        definition.maximum      = param.value(QStringLiteral("maximum"))
                                      .toDouble(param.value(QStringLiteral("max")).toDouble(1.0e9));
        definition.integer =
            param.value(QStringLiteral("type")).toString() != QStringLiteral("float");
        definition.decimals =
            definition.integer ? 0 : param.value(QStringLiteral("decimals")).toInt(2);
        spec.params.push_back(definition);
    }

    const QJsonArray series = object.value(QStringLiteral("series")).toArray();
    int              index  = 0;
    for (const auto& item : series) {
        const QJsonObject seriesObject = item.toObject();
        QString           name         = seriesObject.value(QStringLiteral("name")).toString();
        if (name.isEmpty() && item.isString()) {
            name = item.toString();
        }
        if (name.isEmpty()) {
            continue;
        }
        spec.seriesNames.push_back(name);
        spec.colors.push_back(seriesObject.value(QStringLiteral("color"))
                                  .toString(fallbackColors.at(index % fallbackColors.size())));
        ++index;
    }
    if (spec.seriesNames.isEmpty()) {
        return spec;
    }
    ok = true;
    return spec;
}

} // namespace

IndicatorManager::IndicatorManager(QObject* parent)
    : QObject(parent) {
    qRegisterMetaType<IndicatorSeries>("tqsdk::ui::IndicatorSeries");
    qRegisterMetaType<QVector<IndicatorSeries>>("QVector<tqsdk::ui::IndicatorSeries>");

    responseTimer_ = new QTimer(this);
    responseTimer_->setSingleShot(true);
    connect(responseTimer_, &QTimer::timeout, this, [this] {
        if (!pendingResponses_.isEmpty()) {
            pendingResponses_.clear();
            emit errorOccurred(tr("指标 sidecar 响应超时"));
        }
    });

    restartTimer_ = new QTimer(this);
    restartTimer_->setSingleShot(true);
    restartTimer_->setInterval(1000);
    connect(restartTimer_, &QTimer::timeout, this, [this] {
        if (!enabled_.isEmpty() && !process_) {
            startProcess();
        }
    });
    reloadCatalog();
}

IndicatorManager::~IndicatorManager() {
    stopping_ = true;
    stopProcess();
}

QJsonObject IndicatorManager::params(const QString& indicatorId) const {
    return params_.value(indicatorId);
}

void IndicatorManager::setSidecarPathForTest(const QString& path) {
    sidecarPath_ = path;
    reloadCatalog();
}

void IndicatorManager::setPluginDirectoriesForTest(const QStringList& directories) {
    pluginDirsOverride_ = directories;
    reloadCatalog();
}

QStringList IndicatorManager::pluginDirectories() const {
    if (!pluginDirsOverride_.isEmpty()) {
        return pluginDirsOverride_;
    }

    QStringList   directories;
    const QString config = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    if (!config.isEmpty()) {
        directories.push_back(QDir(config).filePath(QStringLiteral("tqsdk/indicators")));
    }
    const QString appConfig = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (!appConfig.isEmpty()) {
        directories.push_back(QDir(appConfig).filePath(QStringLiteral("indicators")));
    }
    const QString env = qEnvironmentVariable("TQSDK_INDICATOR_PLUGIN_DIRS");
    if (!env.isEmpty()) {
        directories += env.split(QDir::listSeparator(), Qt::SkipEmptyParts);
    }
    const QString script = resolveSidecarPath();
    if (!script.isEmpty()) {
        directories.push_back(
            QDir(QFileInfo(script).absolutePath()).filePath(QStringLiteral("plugins")));
    }
    QStringList   result;
    QSet<QString> seen;
    for (const QString& directory : directories) {
        const QString clean = QDir::cleanPath(directory);
        if (!clean.isEmpty() && !seen.contains(clean)) {
            seen.insert(clean);
            result.push_back(clean);
        }
    }
    return result;
}

QVector<IndicatorSpec> IndicatorManager::loadSpecs(const QStringList& directories) const {
    QVector<IndicatorSpec> specs;
    QSet<QString>          seenIds;
    for (const QString& directory : directories) {
        QDir root(directory);
        if (!root.exists()) {
            continue;
        }
        QStringList     manifests;
        const QFileInfo direct(root.filePath(QStringLiteral("manifest.json")));
        if (direct.isFile()) {
            manifests.push_back(direct.absoluteFilePath());
        }
        for (const QFileInfo& subdir :
             root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
            const QFileInfo manifest(subdir.absoluteFilePath() + QStringLiteral("/manifest.json"));
            if (manifest.isFile()) {
                manifests.push_back(manifest.absoluteFilePath());
            }
        }
        for (const QString& manifest : manifests) {
            bool          ok   = false;
            IndicatorSpec spec = parseManifest(QFileInfo(manifest), ok);
            if (!ok || seenIds.contains(spec.id)) {
                continue;
            }
            seenIds.insert(spec.id);
            specs.push_back(spec);
        }
    }
    std::sort(specs.begin(), specs.end(), [](const IndicatorSpec& a, const IndicatorSpec& b) {
        return a.name.localeAwareCompare(b.name) < 0;
    });
    return specs;
}

void IndicatorManager::reloadCatalog() {
    const QStringList                 oldEnabled = enabled_;
    const QHash<QString, QJsonObject> oldParams  = params_;
    specs_                                       = loadSpecs(pluginDirectories());
    params_.clear();
    for (const auto& spec : specs_) {
        params_.insert(spec.id, oldParams.value(spec.id, spec.defaults()));
    }

    QStringList normalized;
    for (const QString& id : oldEnabled) {
        for (const auto& spec : specs_) {
            if (spec.id == id) {
                normalized.push_back(id);
                break;
            }
        }
    }
    enabled_ = normalized;
    seriesById_.clear();
    if (process_ != nullptr) {
        stopProcess();
        if (!enabled_.isEmpty()) {
            startProcess();
        }
    } else if (!enabled_.isEmpty()) {
        startProcess();
    }
    if (enabled_.isEmpty()) {
        emit seriesChanged({});
    }
    emit catalogChanged();
}

QString IndicatorManager::pythonExecutable() const {
    const QString configured = qEnvironmentVariable("TQSDK_PYTHON");
    return configured.isEmpty() ? QStringLiteral("python3") : configured;
}

QString IndicatorManager::resolveSidecarPath() const {
    // 测试/打包可以显式覆盖；即使路径不存在也必须返回它，让启动阶段明确报错，
    // 而不是悄悄回退到源码树里的另一个脚本。
    if (!sidecarPath_.isEmpty()) {
        return sidecarPath_;
    }
    const QString env = qEnvironmentVariable("TQSDK_INDICATOR_SIDECAR");
    if (!env.isEmpty() && QFileInfo::exists(env)) {
        return env;
    }
    const QString     script     = QStringLiteral("ui/indicator_sidecar/indicator_sidecar.py");
    const QStringList candidates = {
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("../../") + script),
        QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("../share/tqsdk/indicator_sidecar/indicator_sidecar.py")),
        QDir(QCoreApplication::applicationDirPath()).filePath(script),
        QDir::current().filePath(script),
        QDir::current().filePath(QStringLiteral("../") + script),
    };
    for (const auto& candidate : candidates) {
        if (QFileInfo::exists(candidate)) {
            return QDir::cleanPath(candidate);
        }
    }
    return {};
}

void IndicatorManager::startProcess() {
    if (enabled_.isEmpty()) {
        return;
    }
    if (process_ != nullptr) {
        return;
    }
    const QString script = resolveSidecarPath();
    if (script.isEmpty() || !QFileInfo::exists(script)) {
        emit errorOccurred(tr("找不到指标 sidecar: %1").arg(script));
        return;
    }

    process_ = new QProcess(this);
    process_->setProcessChannelMode(QProcess::SeparateChannels);
    // Sidecar 只应看到 Python 运行所需环境；CTP 账号/终端凭据不传给插件进程。
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    for (const QString& key : environment.keys()) {
        if (key.startsWith(QStringLiteral("CTP_")) || key.startsWith(QStringLiteral("TQSDK_CTP")) ||
            key.contains(QStringLiteral("PASSWORD"), Qt::CaseInsensitive) ||
            key.contains(QStringLiteral("AUTH_CODE"), Qt::CaseInsensitive)) {
            environment.remove(key);
        }
    }
    // UI 与 sidecar 必须扫描完全相同的目录，否则菜单会出现无法执行的指标。
    environment.insert(QStringLiteral("TQSDK_INDICATOR_PLUGIN_DIRS"),
                       pluginDirectories().join(QDir::listSeparator()));
    process_->setProcessEnvironment(environment);
    process_->setWorkingDirectory(QFileInfo(script).absolutePath());
    ready_    = false;
    stopping_ = false;
    inputBuffer_.clear();

    connect(process_, &QProcess::readyReadStandardOutput, this, &IndicatorManager::flushInput);
    connect(process_, &QProcess::readyReadStandardError, this, [this] {
        const QByteArray error =
            process_ != nullptr ? process_->readAllStandardError() : QByteArray{};
        if (!error.trimmed().isEmpty()) {
            qWarning("indicator sidecar: %s", error.trimmed().constData());
        }
    });
    connect(process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            emit errorOccurred(tr("指标 sidecar 启动失败: %1").arg(pythonExecutable()));
            process_->deleteLater();
            process_ = nullptr;
        }
    });
    connect(process_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        Q_UNUSED(code);
        Q_UNUSED(status);
        if (stopping_ || process_ == nullptr) {
            return;
        }
        const bool hadEnabled = !enabled_.isEmpty();
        process_->deleteLater();
        process_ = nullptr;
        ready_   = false;
        sentBars_.clear();
        pendingResponses_.clear();
        if (hadEnabled) {
            emit errorOccurred(tr("指标 sidecar 已退出，1 秒后尝试恢复"));
            restartTimer_->start();
        }
    });

    process_->start(pythonExecutable(), {script});
}

void IndicatorManager::stopProcess() {
    if (responseTimer_ != nullptr) {
        responseTimer_->stop();
    }
    if (restartTimer_ != nullptr) {
        restartTimer_->stop();
    }
    pendingResponses_.clear();
    ready_ = false;
    if (process_ != nullptr) {
        stopping_ = true;
        process_->terminate();
        if (!process_->waitForFinished(500)) {
            process_->kill();
            process_->waitForFinished(500);
        }
        process_->deleteLater();
        process_ = nullptr;
    }
}

void IndicatorManager::scheduleRestart() {
    if (!enabled_.isEmpty() && !restartTimer_->isActive()) {
        restartTimer_->start();
    }
}

void IndicatorManager::sendFrame(const QJsonObject& message) {
    if (process_ == nullptr || process_->state() != QProcess::Running) {
        return;
    }
    const QByteArray payload = QJsonDocument(message).toJson(QJsonDocument::Compact);
    if (payload.size() > kMaxSidecarBytes) {
        emit errorOccurred(tr("指标请求超过大小上限"));
        return;
    }
    const quint32 length = qToBigEndian<quint32>(static_cast<quint32>(payload.size()));
    QByteArray    frame;
    frame.append(reinterpret_cast<const char*>(&length), sizeof(length));
    frame.append(payload);
    process_->write(frame);
}

void IndicatorManager::sendInit(const QString& indicatorId) {
    QJsonObject message{
        {QStringLiteral("type"), QStringLiteral("init")},
        {QStringLiteral("indicator_id"), indicatorId},
        {QStringLiteral("params"), params_.value(indicatorId)},
    };
    sendFrame(message);
}

void IndicatorManager::sendBars(bool forceReset) {
    if (!ready_ || enabled_.isEmpty()) {
        return;
    }

    bool reset = forceReset || sentBars_.isEmpty() || bars_.isEmpty() || sentSymbol_ != symbol_ ||
                 sentDurationNs_ != durationNs_ || bars_.size() < sentBars_.size();
    if (!reset && !bars_.isEmpty() && !sentBars_.isEmpty() &&
        bars_.first().datetime != sentBars_.first().datetime) {
        reset = true;
    }

    int start = 0;
    if (!reset) {
        if (bars_.size() == sentBars_.size()) {
            start = bars_.size() - 1;
        } else {
            start = sentBars_.size();
        }
    }
    const QJsonArray payload = barsToJson(bars_, start);
    pendingResponses_        = QSet<QString>(enabled_.begin(), enabled_.end());
    for (const auto& id : enabled_) {
        sendFrame(QJsonObject{{QStringLiteral("type"), QStringLiteral("bars")},
                              {QStringLiteral("indicator_id"), id},
                              {QStringLiteral("reset"), reset},
                              {QStringLiteral("bars"), payload}});
    }
    sentBars_       = bars_;
    sentSymbol_     = symbol_;
    sentDurationNs_ = durationNs_;
    if (!pendingResponses_.isEmpty()) {
        responseTimer_->start(kResponseTimeoutMs);
    }
}

void IndicatorManager::flushInput() {
    if (process_ == nullptr) {
        return;
    }
    inputBuffer_ += process_->readAllStandardOutput();
    if (inputBuffer_.size() > kMaxSidecarBytes) {
        inputBuffer_.clear();
        emit errorOccurred(tr("指标 sidecar 输出超过大小上限，已重启"));
        stopProcess();
        scheduleRestart();
        return;
    }

    while (inputBuffer_.size() >= static_cast<int>(sizeof(quint32))) {
        const quint32 length =
            qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(inputBuffer_.constData()));
        if (length == 0 || length > kMaxSidecarBytes) {
            inputBuffer_.clear();
            emit errorOccurred(tr("指标 sidecar 返回非法帧长度"));
            stopProcess();
            scheduleRestart();
            return;
        }
        if (inputBuffer_.size() < static_cast<int>(sizeof(quint32) + length)) {
            return;
        }
        const QByteArray payload = inputBuffer_.mid(sizeof(quint32), length);
        inputBuffer_.remove(0, sizeof(quint32) + length);
        const QJsonDocument document = QJsonDocument::fromJson(payload);
        if (!document.isObject()) {
            emit errorOccurred(tr("指标 sidecar 返回非法 JSON"));
            continue;
        }
        handleMessage(document.object());
    }
}

void IndicatorManager::handleMessage(const QJsonObject& message) {
    const QString type = message.value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("ready")) {
        ready_ = true;
        for (const auto& id : enabled_) {
            sendInit(id);
        }
        sentBars_.clear();
        sendBars(true);
        return;
    }
    if (type == QStringLiteral("initialized")) {
        return;
    }
    if (type == QStringLiteral("error")) {
        const QString id     = message.value(QStringLiteral("indicator_id")).toString();
        const QString detail = message.value(QStringLiteral("message")).toString();
        pendingResponses_.remove(id);
        if (pendingResponses_.isEmpty()) {
            responseTimer_->stop();
        }
        emit errorOccurred(tr("指标 %1: %2").arg(id, detail));
        return;
    }
    if (type != QStringLiteral("series")) {
        return;
    }

    const QString indicatorId = message.value(QStringLiteral("indicator_id")).toString();
    if (!enabled_.contains(indicatorId)) {
        return;
    }
    const QJsonArray         array = message.value(QStringLiteral("series")).toArray();
    QVector<IndicatorSeries> output;
    int                      index = 0;
    for (const auto& item : array) {
        if (!item.isObject()) {
            continue;
        }
        const QJsonObject object = item.toObject();
        IndicatorSeries   series;
        series.indicatorId   = indicatorId;
        series.indicatorName = specName(indicatorId);
        series.panel      = object.value(QStringLiteral("panel")).toString(specPanel(indicatorId));
        series.seriesName = object.value(QStringLiteral("name")).toString();
        series.color      = seriesColor(indicatorId, index++);
        for (const auto& pointValue : object.value(QStringLiteral("points")).toArray()) {
            const QJsonObject point = pointValue.toObject();
            const double      value = point.value(QStringLiteral("value"))
                                          .toDouble(std::numeric_limits<double>::quiet_NaN());
            const qint64      datetime =
                static_cast<qint64>(point.value(QStringLiteral("datetime")).toDouble());
            if (std::isfinite(value)) {
                series.datetimes.push_back(datetime);
                series.values.push_back(value);
            }
        }
        output.push_back(series);
    }
    seriesById_.insert(indicatorId, output);
    pendingResponses_.remove(indicatorId);
    if (pendingResponses_.isEmpty()) {
        responseTimer_->stop();
    }
    emitSeries();
}

void IndicatorManager::emitSeries() {
    QVector<IndicatorSeries> result;
    for (const auto& id : enabled_) {
        result += seriesById_.value(id);
    }
    emit seriesChanged(result);
}

QString IndicatorManager::specName(const QString& indicatorId) const {
    for (const auto& spec : specs_) {
        if (spec.id == indicatorId) {
            return spec.name;
        }
    }
    return indicatorId;
}

QString IndicatorManager::specPanel(const QString& indicatorId) const {
    for (const auto& spec : specs_) {
        if (spec.id == indicatorId) {
            return spec.panel;
        }
    }
    return QStringLiteral("main");
}

QColor IndicatorManager::seriesColor(const QString& indicatorId, int index) const {
    for (const auto& spec : specs_) {
        if (spec.id == indicatorId && !spec.colors.isEmpty()) {
            return QColor(spec.colors.at(index % spec.colors.size()));
        }
    }
    static const QStringList fallback = {QStringLiteral("#f0c75e"),
                                         QStringLiteral("#53b7ff"),
                                         QStringLiteral("#f28e8e"),
                                         QStringLiteral("#c792ea")};
    return QColor(fallback.at(index % fallback.size()));
}

void IndicatorManager::setEnabled(const QStringList& indicatorIds) {
    QStringList normalized;
    for (const auto& spec : specs_) {
        if (indicatorIds.contains(spec.id)) {
            normalized.push_back(spec.id);
        }
    }
    if (normalized == enabled_) {
        return;
    }
    enabled_ = normalized;
    seriesById_.clear();
    sentBars_.clear();
    emit seriesChanged({});
    stopProcess();
    if (enabled_.isEmpty()) {
        return;
    }
    startProcess();
}

void IndicatorManager::setParams(const QString& indicatorId, const QJsonObject& params) {
    if (!params_.contains(indicatorId)) {
        return;
    }
    QJsonObject merged = params_.value(indicatorId);
    for (auto it = params.begin(); it != params.end(); ++it) {
        merged.insert(it.key(), it.value());
    }
    params_[indicatorId] = merged;
    if (enabled_.contains(indicatorId)) {
        sentBars_.clear();
        stopProcess();
        startProcess();
    }
}

void IndicatorManager::setBars(const QString&           symbol,
                               qint64                   durationNs,
                               const QVector<KlineBar>& bars) {
    symbol_     = symbol;
    durationNs_ = durationNs;
    bars_       = bars;
    if (enabled_.isEmpty()) {
        return;
    }
    if (process_ == nullptr) {
        startProcess();
        return;
    }
    if (ready_) {
        sendBars(false);
    }
}

void IndicatorManager::clear() {
    bars_.clear();
    sentBars_.clear();
    seriesById_.clear();
    emit seriesChanged({});
    stopProcess();
}

} // namespace tqsdk::ui

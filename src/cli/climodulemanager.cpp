#include "climodulemanager.h"

#include "climoduleprotocol.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>

#include <cmath>
#include <cstring>
#include <utility>

namespace {
constexpr qint64 MaximumQueuedBytes = 2 * 1024 * 1024;

QString workerPath(const QString &name) {
    const QString executable = name == QStringLiteral("ax25")
        ? QStringLiteral("ObriiCliPacketModule")
        : name == QStringLiteral("monitor") ? QStringLiteral("ObriiCliSpectrumModule")
        : (name == QStringLiteral("sstv") || name == QStringLiteral("apt") ||
           name == QStringLiteral("wefax"))
            ? QStringLiteral("ObriiCliImageModule") : QStringLiteral("ObriiCliModule");
    return QCoreApplication::applicationDirPath() + QLatin1Char('/') + executable +
#ifdef _WIN32
           QLatin1String(".exe");
#else
           QString();
#endif
}

QString txWorkerPath() {
    return QCoreApplication::applicationDirPath() + QLatin1String("/ObriiCliTxModule") +
#ifdef _WIN32
           QLatin1String(".exe");
#else
           QString();
#endif
}

QString m17WorkerPath() {
    return QCoreApplication::applicationDirPath() + QLatin1String("/ObriiCliM17Module") +
#ifdef _WIN32
           QLatin1String(".exe");
#else
           QString();
#endif
}

QJsonObject errorReply(const QString &error) {
    return {{QStringLiteral("ok"), false}, {QStringLiteral("error"), error}};
}

bool onlyFields(const QJsonObject &object, const QStringList &fields) {
    for (auto it = object.begin(); it != object.end(); ++it)
        if (!fields.contains(it.key())) return false;
    return true;
}

bool number(const QJsonValue &value, double minimum, double maximum) {
    return value.isDouble() && std::isfinite(value.toDouble()) &&
        value.toDouble() >= minimum && value.toDouble() <= maximum;
}

bool integer(const QJsonValue &value, int minimum, int maximum) {
    return number(value, minimum, maximum) && std::floor(value.toDouble()) == value.toDouble();
}

bool backendIsValid(const QString &id) {
    static const QRegularExpression pattern(QStringLiteral("\\A[A-Za-z0-9_-]{1,32}\\z"));
    return id.isEmpty() || pattern.match(id).hasMatch();
}

int privacyMode(const QJsonValue &value) {
    if (!value.isString()) return -1;
    const QString text = value.toString();
    for (int mode : {DMR_PRIVACY_NONE, DMR_PRIVACY_ARC4, DMR_PRIVACY_AES256,
                     DMR_PRIVACY_HYTERA_BASIC, DMR_PRIVACY_AUTO})
        if (text == dmrPrivacyModeId(mode)) return mode;
    return -1;
}

bool validKey(const QJsonValue &value, int mode) {
    if (!value.isString()) return false;
    const QString key = value.toString();
    const int length = key.size();
    if (mode == DMR_PRIVACY_ARC4 ? length != 10 :
        mode == DMR_PRIVACY_AES256 ? length != 64 :
        mode == DMR_PRIVACY_HYTERA_BASIC ? (length != 10 && length != 32 && length != 64) : true)
        return false;
    for (const QChar c : key)
        if (!((c >= QLatin1Char('0') && c <= QLatin1Char('9')) ||
              (c >= QLatin1Char('a') && c <= QLatin1Char('f')) ||
              (c >= QLatin1Char('A') && c <= QLatin1Char('F')))) return false;
    return true;
}
}

bool CliDigitalProfile::supports(const QString &module) {
    return module == QStringLiteral("dmr") || module == QStringLiteral("rtty") ||
        module == QStringLiteral("ft8") || module == QStringLiteral("ft4") || module == QStringLiteral("m17");
}

bool CliDigitalProfile::parse(const QJsonObject &json, const QString &module,
                              Profile &profile, QString &error) {
    // Errors deliberately never contain input values, paths, or JSON parser excerpts.
    const auto fail = [&error](const char *message) { error = QLatin1String(message); return false; };
    if (!supports(module) || !onlyFields(json, {QStringLiteral("version"), QStringLiteral("module"),
            QStringLiteral("settings"), QStringLiteral("dmrPrivacyKeys"),
            QStringLiteral("dmrVoiceBackendId"), QStringLiteral("dmrVoiceOutputEnabled"),
            QStringLiteral("dmrVoicePayloadEnabled"), QStringLiteral("rtty")}) ||
        !integer(json.value(QStringLiteral("version")), 1, 1) ||
        !json.value(QStringLiteral("module")).isString() ||
        json.value(QStringLiteral("module")).toString() != module ||
        QJsonDocument(json).toJson(QJsonDocument::Compact).size() > MaximumBytes)
        return fail("Invalid digital profile schema, version, module or size");

    Profile candidate;
    candidate.json = json;
    candidate.radio.modulationType = module == QStringLiteral("dmr") ? MOD_DMR :
        module == QStringLiteral("rtty") ? MOD_RTTY : module == QStringLiteral("ft4") ? MOD_FT4 :
        module == QStringLiteral("m17") ? MOD_M17 : MOD_FT8;
    if (module != QStringLiteral("dmr")) {
        QStringList fields{QStringLiteral("version"), QStringLiteral("module")};
        if (module == QStringLiteral("rtty")) fields << QStringLiteral("rtty");
        if (!onlyFields(json, fields))
            return fail("DMR settings require the DMR module");
        if (json.contains(QStringLiteral("rtty"))) {
            if (!json.value(QStringLiteral("rtty")).isObject()) return fail("Invalid RTTY configuration");
            const QJsonObject rtty = json.value(QStringLiteral("rtty")).toObject();
            if (!onlyFields(rtty, {QStringLiteral("baud"), QStringLiteral("markHz"),
                                  QStringLiteral("spaceHz"), QStringLiteral("polarity")}))
                return fail("Unknown RTTY setting");
            for (const auto &field : {std::make_pair(QStringLiteral("baud"), 45.45),
                                      std::make_pair(QStringLiteral("markHz"), 2125.0),
                                      std::make_pair(QStringLiteral("spaceHz"), 2295.0)})
                if (rtty.contains(field.first) && !number(rtty.value(field.first), field.second, field.second))
                    return fail("Shared RTTY decoder supports only 45.45 baud and 2125/2295 Hz tones");
            if (rtty.contains(QStringLiteral("polarity")) &&
                (!rtty.value(QStringLiteral("polarity")).isString() ||
                 rtty.value(QStringLiteral("polarity")).toString() != QStringLiteral("auto")))
                return fail("Shared RTTY decoder supports automatic polarity only");
        }
    } else {
        if (json.contains(QStringLiteral("rtty"))) return fail("RTTY settings require the RTTY module");
        if (json.contains(QStringLiteral("dmrVoiceBackendId"))) {
            const QJsonValue id = json.value(QStringLiteral("dmrVoiceBackendId"));
            if (!id.isString() || !backendIsValid(id.toString())) return fail("Invalid DMR backend ID");
            candidate.voiceBackend = id.toString();
        }
        for (const auto &field : {std::make_pair(QStringLiteral("dmrVoiceOutputEnabled"), &candidate.voiceOutput),
                                  std::make_pair(QStringLiteral("dmrVoicePayloadEnabled"), &candidate.voicePayload)}) {
            if (!json.contains(field.first)) continue;
            if (!json.value(field.first).isBool()) return fail("Invalid DMR voice setting type");
            *field.second = json.value(field.first).toBool();
        }
        if (candidate.voiceOutput && !candidate.voicePayload)
            return fail("DMR voice output requires voice payload decoding");
        if (json.contains(QStringLiteral("settings")) && !json.value(QStringLiteral("settings")).isObject())
            return fail("Digital profile settings must be an object");
        QJsonObject settings = json.value(QStringLiteral("settings")).toObject();
        RadioSettings &radio = candidate.radio;
        QStringList fields{QStringLiteral("dmrPrivacyMode"), QStringLiteral("dmrPrivacyKeyHex")};
        const auto boolean = [&](const char *name, bool &target) {
            const QString field = QLatin1String(name);
            fields << field;
            if (!settings.contains(field)) return true;
            if (!settings.value(field).isBool()) return false;
            target = settings.value(field).toBool();
            return true;
        };
        const auto boundedInt = [&](const char *name, int &target, int minimum, int maximum) {
            const QString field = QLatin1String(name);
            fields << field;
            if (!settings.contains(field)) return true;
            if (!integer(settings.value(field), minimum, maximum)) return false;
            target = settings.value(field).toInt();
            return true;
        };
        if (!boolean("dmrPrivacyAutoKeySelection", radio.dmrPrivacyAutoKeySelection) ||
            !boolean("dmrLabEnabled", radio.dmrLabEnabled) ||
            !boolean("dmrManualTimingEnabled", radio.dmrManualTimingEnabled) ||
            !boolean("dmrAdaptiveSlicer", radio.dmrAdaptiveSlicer) ||
            !boundedInt("dmrPrivacyKeyId", radio.dmrPrivacyKeyId, 0, 255) ||
            !boundedInt("dmrLabColorCode", radio.dmrLabColorCode, -1, 15) ||
            !boundedInt("dmrLabTimeslot", radio.dmrLabTimeslot, 0, 2) ||
            !boundedInt("dmrLabSourceId", radio.dmrLabSourceId, 0, 16777215) ||
            !boundedInt("dmrLabTargetId", radio.dmrLabTargetId, 0, 16777215) ||
            !boundedInt("dmrManualTimingOffset", radio.dmrManualTimingOffset, -80, 80) ||
            !boundedInt("dmrAmbeLayout", radio.dmrAmbeLayout, 0, 4))
            return fail("Invalid DMR setting type or range");
        fields << QStringLiteral("dmrSlicerRatio");
        if (settings.contains(QStringLiteral("dmrSlicerRatio"))) {
            const QJsonValue ratio = settings.value(QStringLiteral("dmrSlicerRatio"));
            if (!number(ratio, 0.45, 0.80)) return fail("Invalid DMR slicer ratio");
            radio.dmrSlicerRatio = ratio.toDouble();
        }
        if (!onlyFields(settings, fields)) return fail("Unknown or unsupported DMR setting");
        if (settings.contains(QStringLiteral("dmrPrivacyMode"))) {
            radio.dmrPrivacyMode = privacyMode(settings.value(QStringLiteral("dmrPrivacyMode")));
            if (radio.dmrPrivacyMode < 0) return fail("Unsupported DMR privacy mode");
        }
        if (json.contains(QStringLiteral("dmrPrivacyKeys"))) {
            if (!json.value(QStringLiteral("dmrPrivacyKeys")).isArray()) return fail("DMR keyring must be an array");
            const QJsonArray keys = json.value(QStringLiteral("dmrPrivacyKeys")).toArray();
            if (keys.size() > MaximumKeys) return fail("DMR keyring exceeds entry limit");
            for (const QJsonValue &value : keys) {
                if (!value.isObject()) return fail("Invalid DMR keyring entry");
                const QJsonObject key = value.toObject();
                const int mode = privacyMode(key.value(QStringLiteral("mode")));
                if (!onlyFields(key, {QStringLiteral("mode"), QStringLiteral("keyId"), QStringLiteral("keyHex")}) ||
                    !integer(key.value(QStringLiteral("keyId")), 0, 255) ||
                    !validKey(key.value(QStringLiteral("keyHex")), mode))
                    return fail("Invalid DMR keyring mode, ID or hexadecimal key width");
                const int id = key.value(QStringLiteral("keyId")).toInt();
                if (findDmrPrivacyKeyIndexByModeAndId(candidate.keys, mode, id) >= 0)
                    return fail("Duplicate DMR keyring mode and ID");
                candidate.keys.append({false, mode, id, key.value(QStringLiteral("keyHex")).toString().toUpper(), {}});
            }
        }
        const bool automatic = radio.dmrPrivacyAutoKeySelection || radio.dmrPrivacyMode == DMR_PRIVACY_AUTO;
        if (settings.contains(QStringLiteral("dmrPrivacyKeyHex")) &&
            !settings.value(QStringLiteral("dmrPrivacyKeyHex")).isString())
            return fail("DMR key must be a hexadecimal string");
        const QString fixedKey = settings.value(QStringLiteral("dmrPrivacyKeyHex")).toString();
        if (automatic) {
            if (!fixedKey.isEmpty() || radio.dmrPrivacyKeyId != 0 || candidate.keys.isEmpty())
                return fail("Automatic DMR privacy requires a keyring without a fixed key or ID");
            for (const DmrPrivacyKeyEntry &key : candidate.keys)
                if (key.mode != DMR_PRIVACY_ARC4 && key.mode != DMR_PRIVACY_AES256)
                    return fail("Automatic DMR privacy supports ARC4 and AES256 PI selection only");
        } else if (radio.dmrPrivacyMode == DMR_PRIVACY_NONE) {
            if (!fixedKey.isEmpty() || radio.dmrPrivacyKeyId != 0 || !candidate.keys.isEmpty())
                return fail("DMR keys require an explicit privacy mode");
        } else {
            const int index = findDmrPrivacyKeyIndexByModeAndId(candidate.keys, radio.dmrPrivacyMode,
                                                               radio.dmrPrivacyKeyId);
            radio.dmrPrivacyKeyHex = fixedKey;
            if (fixedKey.isEmpty() && index >= 0) radio.dmrPrivacyKeyHex = candidate.keys.at(index).keyHex;
            if (!validKey(radio.dmrPrivacyKeyHex, radio.dmrPrivacyMode))
                return fail("Invalid fixed DMR hexadecimal key width or missing selected key");
            radio.dmrPrivacyKeyHex = radio.dmrPrivacyKeyHex.toUpper();
            if (index >= 0 && radio.dmrPrivacyKeyHex != candidate.keys.at(index).keyHex)
                return fail("Conflicting fixed DMR key and selected keyring entry");
        }
    }
    profile = std::move(candidate);
    error.clear();
    return true;
}

bool CliDigitalProfile::readFile(const QString &path, const QString &module,
                                 Profile &profile, QString &error) {
    if (path.isEmpty() || path.size() > 4096 || path.contains(QChar::Null) ||
        !path.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive)) {
        error = QStringLiteral("Digital profile requires a JSON file path of at most 4096 characters");
        return false;
    }
    QFile file(path);
    if (!QFileInfo(path).isFile() || !file.open(QIODevice::ReadOnly) ||
        !QFileInfo(file).isFile() || file.isSequential() || file.size() <= 0 || file.size() > MaximumBytes) {
        error = QStringLiteral("Digital profile must be a readable regular file of 1..65536 bytes");
        return false;
    }
    const QByteArray bytes = file.read(MaximumBytes + 1);
    if (file.error() != QFile::NoError || bytes.size() > MaximumBytes || !file.atEnd()) {
        error = QStringLiteral("Digital profile read failed or exceeded the size limit");
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        error = QStringLiteral("Digital profile must contain a valid JSON object");
        return false;
    }
    return parse(document.object(), module, profile, error);
}

CliModuleManager::CliModuleManager(QObject *parent) : QObject(parent) {
    ft8.name = QStringLiteral("ft8");
    ft4.name = QStringLiteral("ft4");
    m17.name = QStringLiteral("m17");
    rtty.name = QStringLiteral("rtty");
    dmr.name = QStringLiteral("dmr");
    ax25.name = QStringLiteral("ax25");
    sstv.name = QStringLiteral("sstv");
    apt.name = QStringLiteral("apt");
    wefax.name = QStringLiteral("wefax");
    monitor.name = QStringLiteral("monitor");
}

CliModuleManager::~CliModuleManager() {
    stop(ft8);
    stop(ft4);
    stop(m17);
    stop(rtty);
    stop(dmr);
    stop(ax25);
    stop(sstv);
    stop(apt);
    stop(wefax);
    stop(monitor);
}

CliModuleManager::Slot *CliModuleManager::slot(const QString &name) {
    if (name == QStringLiteral("ft8")) return &ft8;
    if (name == QStringLiteral("ft4")) return &ft4;
    if (name == QStringLiteral("m17")) return &m17;
    if (name == QStringLiteral("rtty")) return &rtty;
    if (name == QStringLiteral("dmr")) return &dmr;
    if (name == QStringLiteral("ax25")) return &ax25;
    if (name == QStringLiteral("sstv")) return &sstv;
    if (name == QStringLiteral("apt")) return &apt;
    if (name == QStringLiteral("wefax")) return &wefax;
    if (name == QStringLiteral("monitor")) return &monitor;
    return nullptr;
}

const CliModuleManager::Slot *CliModuleManager::slot(const QString &name) const {
    if (name == QStringLiteral("ft8")) return &ft8;
    if (name == QStringLiteral("ft4")) return &ft4;
    if (name == QStringLiteral("m17")) return &m17;
    if (name == QStringLiteral("rtty")) return &rtty;
    if (name == QStringLiteral("dmr")) return &dmr;
    if (name == QStringLiteral("ax25")) return &ax25;
    if (name == QStringLiteral("sstv")) return &sstv;
    if (name == QStringLiteral("apt")) return &apt;
    if (name == QStringLiteral("wefax")) return &wefax;
    if (name == QStringLiteral("monitor")) return &monitor;
    return nullptr;
}

bool CliModuleManager::running(const Slot &module) const {
    return module.process && module.process->state() == QProcess::Running;
}

QJsonObject CliModuleManager::status(const Slot &module) const {
    return {{QStringLiteral("name"), module.name},
            {QStringLiteral("available"), QFileInfo::exists(workerPath(module.name))},
            {QStringLiteral("running"), running(module)},
            {QStringLiteral("frames"), static_cast<double>(module.frames)},
            {QStringLiteral("droppedFrames"), static_cast<double>(module.droppedFrames)},
            {QStringLiteral("textEvents"), static_cast<double>(module.textEvents)},
            {QStringLiteral("voiceFrames"), static_cast<double>(module.voiceFrames)},
            {QStringLiteral("queuedBytes"), running(module) ? module.process->bytesToWrite() : 0},
            {QStringLiteral("lastStatus"), module.lastStatus},
            {QStringLiteral("lastError"), module.lastError},
            {QStringLiteral("profileSelected"), module.profileSelected}};
}

QJsonObject CliModuleManager::start(Slot &module, const QJsonObject &request) {
    if (running(module)) return errorReply(QStringLiteral("Module is already running"));
    if (request.contains(QStringLiteral("backend")) && !request.value(QStringLiteral("backend")).isString())
        return errorReply(QStringLiteral("Module start argument must be a string"));
    if (request.contains(QStringLiteral("path")) && !request.value(QStringLiteral("path")).isString())
        return errorReply(QStringLiteral("Digital profile path must be a string"));
    const QString backendId = request.value(QStringLiteral("backend")).toString();
    const QString path = request.value(QStringLiteral("path")).toString();
    const bool digital = CliDigitalProfile::supports(module.name);
    bool profileSelected = module.profileSelected;
    QJsonObject selectedProfile = module.digitalProfile;
    if (request.contains(QStringLiteral("path")) && (!digital || path.isEmpty()))
        return errorReply(QStringLiteral("A nonempty profile path applies only to DMR, RTTY or FT8 start"));
    if (digital) {
        CliDigitalProfile::Profile profile;
        QString error;
        if (!path.isEmpty()) {
            if (!CliDigitalProfile::readFile(path, module.name, profile, error)) return errorReply(error);
            selectedProfile = profile.json;
            profileSelected = true;
        }
        if (selectedProfile.isEmpty()) selectedProfile = {
            {QStringLiteral("version"), 1}, {QStringLiteral("module"), module.name}};
        if (!backendId.isEmpty() && module.name == QStringLiteral("dmr"))
            selectedProfile.insert(QStringLiteral("dmrVoiceBackendId"), backendId);
        if (!CliDigitalProfile::parse(selectedProfile, module.name, profile, error)) return errorReply(error);
    }
    const QString executable = workerPath(module.name);
    if (!QFileInfo::exists(executable))
        return errorReply(QStringLiteral("Module executable is not installed beside ObriiCli"));
    if (module.name != QStringLiteral("dmr") && module.name != QStringLiteral("ax25") &&
        module.name != QStringLiteral("monitor") &&
        !backendId.isEmpty())
        return errorReply(QStringLiteral("Only DMR, AX.25 and monitor accept a start argument"));
    if (module.name == QStringLiteral("dmr") && !backendIsValid(backendId))
        return errorReply(QStringLiteral("Invalid DMR backend ID"));
    int kissPort = 8001;
    if (module.name == QStringLiteral("ax25") && !backendId.isEmpty()) {
        bool okay = false;
        kissPort = backendId.toInt(&okay);
        if (!okay || kissPort < 1 || kissPort > 65535)
            return errorReply(QStringLiteral("AX.25 KISS port must be 1..65535"));
    }
    module.process = std::make_unique<QProcess>();
    module.outputBuffer.clear();
    module.lastStatus.clear();
    module.lastError.clear();
    module.frames = module.droppedFrames = module.textEvents = module.voiceFrames = 0;
    module.pendingReset = false;
    QProcess *process = module.process.get();
    process->setProgram(executable);
    QStringList arguments{QStringLiteral("rx"), module.name};
    if (module.name == QStringLiteral("ax25")) arguments[1] = QString::number(kissPort);
    else if (module.name == QStringLiteral("monitor")) {
        if (backendId.isEmpty() || !QFileInfo(backendId).isFile())
            return errorReply(QStringLiteral("Monitor requires an existing JSON preset file"));
        arguments[1] = backendId;
    }
    process->setArguments(arguments);
    QObject::connect(process, &QProcess::readyReadStandardOutput, this,
                     [this, &module] { consumeOutput(module); });
    QObject::connect(process, &QProcess::readyReadStandardError, this, [&module] {
        if (!module.process) return;
        const QByteArray detail = module.process->readAllStandardError().trimmed();
        if (!detail.isEmpty()) module.lastError = QString::fromLocal8Bit(detail.right(500));
    });
    QObject::connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                     this, [this, &module](int code, QProcess::ExitStatus state) {
        consumeOutput(module);
        if (code != 0 || state != QProcess::NormalExit)
            module.lastError = module.lastError.isEmpty()
                ? QStringLiteral("Module exited with code %1").arg(code)
                : QStringLiteral("%1 (exit %2)").arg(module.lastError).arg(code);
    });
    process->start();
    if (!process->waitForStarted(3000)) {
        const QString error = process->errorString();
        module.process.reset();
        return errorReply(error);
    }
    // Commit only after launch; failed validation/start never replaces the saved selection.
    const QJsonObject previousProfile = module.digitalProfile;
    const bool previouslySelected = module.profileSelected;
    module.digitalProfile = selectedProfile;
    module.profileSelected = profileSelected;
    const QByteArray bytes = configuration(module);
    if (!submit(module, CliModuleProtocol::Configure, 0, bytes.constData(),
                static_cast<quint32>(bytes.size()))) {
        stop(module);
        module.digitalProfile = previousProfile;
        module.profileSelected = previouslySelected;
        return errorReply(QStringLiteral("Module configuration could not be queued"));
    }
    return {{QStringLiteral("ok"), true}, {QStringLiteral("module"), status(module)}};
}

QJsonObject CliModuleManager::stop(Slot &module) {
    if (!module.process) return {{QStringLiteral("ok"), true}, {QStringLiteral("module"), status(module)}};
    if (running(module)) {
        module.process->closeWriteChannel();
        if (!module.process->waitForFinished(500)) {
            module.process->kill();
            module.process->waitForFinished(1000);
        }
    }
    consumeOutput(module);
    module.process.reset();
    return {{QStringLiteral("ok"), true}, {QStringLiteral("module"), status(module)}};
}

QJsonObject CliModuleManager::command(const QJsonObject &request) {
    const QString operation = request.value(QStringLiteral("operation")).toString();
    const QString name = request.value(QStringLiteral("name")).toString();
    if (operation == QStringLiteral("list") ||
        (operation == QStringLiteral("status") && name.isEmpty()))
        return {{QStringLiteral("ok"), true},
                {QStringLiteral("modules"), QJsonArray{status(ft8), status(ft4), status(m17), status(rtty), status(dmr),
                    status(ax25), status(sstv), status(apt), status(wefax), status(monitor),
                    QJsonObject{{QStringLiteral("name"), QStringLiteral("tx")},
                                {QStringLiteral("available"), QFileInfo::exists(txWorkerPath())},
                                {QStringLiteral("modes"), QJsonArray{QStringLiteral("ft8"),
                                    QStringLiteral("ft4"),
                                    QStringLiteral("cw"), QStringLiteral("rtty"),
                                    QStringLiteral("dmr"), QStringLiteral("lab"),
                                    QStringLiteral("ax25")}},
                                {QStringLiteral("sinks"), QJsonArray{QStringLiteral("file"),
                                    QStringLiteral("virtual"), QStringLiteral("hackrf")}}},
                    QJsonObject{{QStringLiteral("name"), QStringLiteral("m17-symbols")},
                                {QStringLiteral("command"), QStringLiteral("module m17 encode|decode")},
                                {QStringLiteral("available"), QFileInfo::exists(m17WorkerPath())},
                                {QStringLiteral("operations"), QJsonArray{QStringLiteral("encode"),
                                    QStringLiteral("decode")}}}}}};
    Slot *module = slot(name);
    if (!module) return errorReply(QStringLiteral("Unknown module; use module list"));
    if (operation == QStringLiteral("status"))
        return {{QStringLiteral("ok"), true}, {QStringLiteral("module"), status(*module)}};
    if (operation == QStringLiteral("start"))
        return start(*module, request);
    if (operation == QStringLiteral("stop")) return stop(*module);
    if (operation == QStringLiteral("snapshot")) {
        if (module != &sstv && module != &apt && module != &wefax)
            return errorReply(QStringLiteral("Snapshots apply to image modules only"));
        if (!running(*module)) return errorReply(QStringLiteral("Image module is not running"));
        const QByteArray path = request.value(QStringLiteral("path")).toString().toUtf8();
        if (path.isEmpty()) return errorReply(QStringLiteral("Snapshot path is required"));
        submit(*module, CliModuleProtocol::Snapshot, 0, path.constData(),
               static_cast<quint32>(path.size()));
        return {{QStringLiteral("ok"), true}, {QStringLiteral("queued"), true}};
    }
    return errorReply(QStringLiteral("Module operation must be list, start, status, stop or snapshot"));
}

QByteArray CliModuleManager::configuration(const Slot &module) const {
    QJsonObject json{{QStringLiteral("listeningFrequencyHz"), listeningHz}};
    if (CliDigitalProfile::supports(module.name))
        json.insert(QStringLiteral("digitalProfile"), module.digitalProfile);
    return QJsonDocument(json).toJson(QJsonDocument::Compact);
}

bool CliModuleManager::submit(Slot &module, quint32 kind, quint32 sampleRate,
                              const char *data, quint32 size) {
    if (!running(module)) return false;
    if (size > CliModuleProtocol::MaximumPayloadBytes ||
        module.process->bytesToWrite() + size + 24 + CliDigitalProfile::MaximumBytes + 1024 > MaximumQueuedBytes) {
        ++module.droppedFrames;
        module.pendingReset = true;
        return false;
    }
    if (module.pendingReset) {
        const QByteArray resetFrame = CliModuleProtocol::frame(CliModuleProtocol::Reset, 0, nullptr, 0);
        if (module.process->write(resetFrame) != resetFrame.size()) {
            module.lastError = QStringLiteral("Module reset could not be written");
            module.process->kill();
            return false;
        }
        module.pendingReset = false;
        // A retune's Configure may itself have been dropped under backpressure.
        const QByteArray bytes = configuration(module);
        const QByteArray configureFrame = CliModuleProtocol::frame(CliModuleProtocol::Configure, 0,
            bytes.constData(), static_cast<quint32>(bytes.size()));
        if (module.process->write(configureFrame) != configureFrame.size()) {
            module.lastError = QStringLiteral("Module configuration could not be written");
            module.process->kill();
            return false;
        }
    }
    const QByteArray wire = CliModuleProtocol::frame(kind, sampleRate, data, size);
    if (module.process->write(wire) != wire.size()) {
        ++module.droppedFrames;
        module.lastError = QStringLiteral("Module input pipe could not accept a complete frame");
        module.process->kill();
        return false;
    }
    ++module.frames;
    return true;
}

void CliModuleManager::submitPcm(const QByteArray &pcm) {
    if (pcm.isEmpty()) return;
    for (Slot *module : {&ft8, &ft4, &rtty, &ax25, &sstv, &apt, &wefax})
        submit(*module, CliModuleProtocol::Pcm16, 48000, pcm.constData(),
               static_cast<quint32>(pcm.size()));
}

void CliModuleManager::submitChannelIq(const float *interleavedIq, int complexSamples,
                                       double sampleRate) {
    if ((!running(dmr) && !running(m17)) || !interleavedIq || complexSamples <= 0 ||
        !std::isfinite(sampleRate) || sampleRate < 48000.0 || sampleRate > 2000000.0) return;
    const quint32 bytes = static_cast<quint32>(complexSamples * 2 * sizeof(float));
    submit(dmr, CliModuleProtocol::ChannelIqF32, static_cast<quint32>(std::lround(sampleRate)),
           reinterpret_cast<const char *>(interleavedIq), bytes);
    submit(m17, CliModuleProtocol::ChannelIqF32, static_cast<quint32>(std::lround(sampleRate)),
           reinterpret_cast<const char *>(interleavedIq), bytes);
}

void CliModuleManager::submitSpectrum(const std::vector<float> &levelsDbfs,
                                      double centerHz, double sampleRateHz) {
    if (!running(monitor) || levelsDbfs.empty() || !std::isfinite(centerHz) ||
        !std::isfinite(sampleRateHz) || sampleRateHz <= 0) return;
    constexpr std::size_t MaximumBins = 8192;
    const std::size_t bins = (std::min)(levelsDbfs.size(), MaximumBins);
    QByteArray payload(static_cast<int>(16 + bins * sizeof(float)), Qt::Uninitialized);
    std::memcpy(payload.data(), &centerHz, sizeof(centerHz));
    std::memcpy(payload.data() + 8, &sampleRateHz, sizeof(sampleRateHz));
    for (std::size_t i = 0; i < bins; ++i) {
        const std::size_t begin = i * levelsDbfs.size() / bins;
        const std::size_t end = (i + 1) * levelsDbfs.size() / bins;
        float peak = -200.0f;
        for (std::size_t j = begin; j < end; ++j) peak = (std::max)(peak, levelsDbfs[j]);
        std::memcpy(payload.data() + 16 + i * sizeof(float), &peak, sizeof(peak));
    }
    submit(monitor, CliModuleProtocol::SpectrumF32, 0, payload.constData(),
           static_cast<quint32>(payload.size()));
}

void CliModuleManager::reset() {
    for (Slot *module : {&ft8, &ft4, &m17, &rtty, &dmr, &ax25, &sstv, &apt, &wefax, &monitor})
        submit(*module, CliModuleProtocol::Reset, 0, nullptr, 0);
}

void CliModuleManager::setListeningHz(double frequencyHz) {
    if (!std::isfinite(frequencyHz) || frequencyHz <= 0.0 || frequencyHz > 1.0e12) return;
    if (std::abs(frequencyHz - listeningHz) < 0.5) return;
    listeningHz = frequencyHz;
    for (Slot *module : {&ft8, &ft4, &m17, &rtty, &dmr, &ax25, &sstv, &apt, &wefax, &monitor}) {
        if (!running(*module)) continue;
        const QByteArray bytes = configuration(*module);
        submit(*module, CliModuleProtocol::Configure, 0, bytes.constData(),
               static_cast<quint32>(bytes.size()));
    }
    reset();
}

bool CliModuleManager::needsAudio() const {
    return running(ft8) || running(ft4) || running(rtty) || running(ax25) || running(sstv) || running(apt) || running(wefax);
}
bool CliModuleManager::needsChannelIq() const { return running(dmr) || running(m17); }
bool CliModuleManager::needsSpectrum() const { return running(monitor); }

void CliModuleManager::consumeOutput(Slot &module) {
    if (!module.process) return;
    module.outputBuffer += module.process->readAllStandardOutput();
    if (module.outputBuffer.size() > 2 * 1024 * 1024) {
        module.outputBuffer.clear();
        module.lastError = QStringLiteral("Module output exceeded line limit");
        return;
    }
    int newline = 0;
    while ((newline = module.outputBuffer.indexOf('\n')) >= 0) {
        const QByteArray line = module.outputBuffer.left(newline);
        module.outputBuffer.remove(0, newline + 1);
        const QJsonObject event = QJsonDocument::fromJson(line).object();
        if (event.isEmpty()) continue;
        const QString kind = event.value(QStringLiteral("event")).toString();
        if (kind == QStringLiteral("voicePcm")) {
            ++module.voiceFrames;
            if (onVoicePcm) onVoicePcm(QByteArray::fromBase64(
                event.value(QStringLiteral("pcmBase64")).toString().toLatin1()));
        } else if (kind == QStringLiteral("status")) {
            module.lastStatus = event.value(QStringLiteral("text")).toString();
        } else {
            if (kind == QStringLiteral("text") || kind == QStringLiteral("ft8") ||
                kind == QStringLiteral("ax25"))
                ++module.textEvents;
            if (onEvent) onEvent(event);
        }
    }
}

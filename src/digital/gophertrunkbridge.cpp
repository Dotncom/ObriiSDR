#include "gophertrunkbridge.h"

#include "diagnosticlogging.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QStringList>
#include <QTimer>

#include <algorithm>
#include <cmath>

namespace {
constexpr qint64 PROCESS_RESTART_BACKOFF_MAX_MS = 10000;
constexpr int DEFAULT_GOPHER_AUDIO_RATE = 8000;
constexpr int APP_AUDIO_RATE = 48000;
constexpr int UPSAMPLE_FACTOR = APP_AUDIO_RATE / DEFAULT_GOPHER_AUDIO_RATE;
constexpr double GOPHER_OUTPUT_TARGET_PEAK = 0.55;
constexpr double GOPHER_OUTPUT_MAX_GAIN = 16.0;

QString rotateDibitPolarity(const QString &dibits) {
    QString out;
    out.reserve(dibits.size());
    for (QChar ch : dibits) {
        const int value = ch.unicode() - QLatin1Char('0').unicode();
        if (value < 0 || value > 3) {
            out.append(ch);
            continue;
        }
        out.append(QLatin1Char(static_cast<char>('0' + ((value + 2) & 3))));
    }
    return out;
}

int syncDistance(const QString &center, const QString &pattern) {
    if (center.size() != pattern.size()) {
        return 1000;
    }
    int distance = 0;
    for (int i = 0; i < center.size(); ++i) {
        if (center.at(i) != pattern.at(i)) {
            ++distance;
        }
    }
    return distance;
}

int bestVoiceSyncDistance(const QString &center) {
    static const QStringList kVoiceSyncs = {
        QStringLiteral("131111333113313313113313"), // BS voice
        QStringLiteral("133313311131311113313331"), // MS voice
        QStringLiteral("113111131333131311133333"), // DM voice TS1
        QStringLiteral("133133333111331111311133")  // DM voice TS2
    };
    int best = 1000;
    for (const QString &pattern : kVoiceSyncs) {
        best = std::min(best, syncDistance(center, pattern));
    }
    return best;
}

bool detectDmrVoicePolarityFlip(const QString &dibits, bool *flipped) {
    if (dibits.size() != 132) {
        return false;
    }

    const QString center = dibits.mid(54, 24);
    const int normalDistance = bestVoiceSyncDistance(center);
    const int rotatedDistance = bestVoiceSyncDistance(rotateDibitPolarity(center));
    const int bestDistance = qMin(normalDistance, rotatedDistance);
    if (bestDistance > 2) {
        return false;
    }

    if (flipped) {
        *flipped = rotatedDistance < normalDistance;
    }
    return true;
}

QString applyDmrVoicePolarityForGopher(const QString &dibits, bool flip) {
    return flip ? rotateDibitPolarity(dibits) : dibits;
}

struct GopherProcessArguments {
    QStringList actual;
    QStringList display;
};

GopherProcessArguments buildGopherProcessArguments(const QStringList &baseArguments,
                                                   const QString &mode,
                                                   int keyId,
                                                   const QString &keyHex,
                                                   bool forwardToBackend,
                                                   const QString &variant,
                                                   const QString &layout,
                                                   int frameOffset,
                                                   const QString &overridePath) {
    static const QStringList managedOptions = {
        QStringLiteral("-privacy-mode"),
        QStringLiteral("-privacy-key-id"),
        QStringLiteral("-privacy-key-hex"),
        QStringLiteral("-privacy-variant"),
        QStringLiteral("-privacy-layout"),
        QStringLiteral("-privacy-frame-offset"),
        QStringLiteral("-privacy-override-file")};

    GopherProcessArguments result;
    for (int index = 0; index < baseArguments.size(); ++index) {
        const QString argument = baseArguments.at(index);
        if (managedOptions.contains(argument)) {
            if (index + 1 < baseArguments.size()) {
                ++index;
            }
            continue;
        }
        result.actual.append(argument);
        result.display.append(argument);
    }

    const bool active = forwardToBackend && mode != QLatin1String("none");
    const QString effectiveMode = active ? mode : QStringLiteral("none");
    result.actual << QStringLiteral("-privacy-mode") << effectiveMode
                  << QStringLiteral("-privacy-key-id") << QString::number(active ? keyId : 0)
                  << QStringLiteral("-privacy-variant") << variant
                  << QStringLiteral("-privacy-layout") << layout
                  << QStringLiteral("-privacy-frame-offset") << QString::number(frameOffset);
    result.display = result.actual;
    if (active && !keyHex.isEmpty()) {
        result.actual << QStringLiteral("-privacy-key-hex") << keyHex;
        result.display << QStringLiteral("-privacy-key-hex")
                       << QStringLiteral("<redacted:%1-bytes>").arg(keyHex.size() / 2);
    }
    if (!overridePath.isEmpty()) {
        result.actual << QStringLiteral("-privacy-override-file") << overridePath;
        result.display << QStringLiteral("-privacy-override-file") << overridePath;
    }
    return result;
}

qint16 readLe16(const char *data) {
    const auto lo = static_cast<quint8>(data[0]);
    const auto hi = static_cast<quint8>(data[1]);
    return static_cast<qint16>(static_cast<quint16>(lo) |
                               (static_cast<quint16>(hi) << 8));
}

void appendLe16(QByteArray &target, qint16 value) {
    const quint16 raw = static_cast<quint16>(value);
    target.append(static_cast<char>(raw & 0xff));
    target.append(static_cast<char>((raw >> 8) & 0xff));
}

QString compactGopherSummary(const QString &line) {
    const QJsonDocument doc = QJsonDocument::fromJson(line.toUtf8());
    if (!doc.isObject()) {
        return line.left(160);
    }

    const QJsonObject obj = doc.object();
    const QString type = obj.value(QStringLiteral("type")).toString(QStringLiteral("output"));

    QStringList parts;
    parts << type;
    if (obj.contains(QStringLiteral("startDibit"))) {
        parts << QStringLiteral("base=%1").arg(obj.value(QStringLiteral("startDibit")).toInt());
    }
    if (obj.contains(QStringLiteral("phase"))) {
        parts << QStringLiteral("phase=%1").arg(obj.value(QStringLiteral("phase")).toInt());
    }
    if (obj.contains(QStringLiteral("timeslot"))) {
        QString ts = QStringLiteral("ts=%1").arg(obj.value(QStringLiteral("timeslot")).toInt());
        const QString tsSource = obj.value(QStringLiteral("timeslotSource")).toString();
        if (!tsSource.isEmpty()) {
            ts += QStringLiteral("/%1").arg(tsSource);
        }
        parts << ts;
    }
    const QJsonObject cach = obj.value(QStringLiteral("cach")).toObject();
    if (!cach.isEmpty()) {
        if (cach.value(QStringLiteral("decoded")).toBool()) {
            QString cachText =
                QStringLiteral("cach=ch%1/lcss%2")
                    .arg(cach.value(QStringLiteral("channel")).toInt())
                    .arg(cach.value(QStringLiteral("lcss")).toInt());
            const QString map = cach.value(QStringLiteral("map")).toString();
            if (!map.isEmpty()) {
                cachText += QStringLiteral("/%1").arg(map);
            }
            parts << cachText;
        } else {
            parts << QStringLiteral("cach=no");
        }
    }
    const QString sync = obj.value(QStringLiteral("sync")).toString();
    if (!sync.isEmpty()) {
        parts << QStringLiteral("sync=%1").arg(sync);
    }
    if (obj.contains(QStringLiteral("frames"))) {
        parts << QStringLiteral("frames=%1").arg(obj.value(QStringLiteral("frames")).toInt());
    }
    if (obj.contains(QStringLiteral("badFrames"))) {
        parts << QStringLiteral("bad=%1").arg(obj.value(QStringLiteral("badFrames")).toInt());
    }
    if (obj.contains(QStringLiteral("audioFrames"))) {
        parts << QStringLiteral("audio=%1").arg(obj.value(QStringLiteral("audioFrames")).toInt());
    }
    const QJsonObject privacy = obj.value(QStringLiteral("privacy")).toObject();
    if (!privacy.isEmpty()) {
        QString privacyText = privacy.value(QStringLiteral("mode")).toString(QStringLiteral("privacy"));
        if (privacy.contains(QStringLiteral("keyId"))) {
            privacyText += QStringLiteral("/kid%1").arg(privacy.value(QStringLiteral("keyId")).toInt());
        }
        privacyText += privacy.value(QStringLiteral("keySet")).toBool()
                           ? QStringLiteral("/key")
                           : QStringLiteral("/nokey");
        if (privacy.value(QStringLiteral("applied")).toBool()) {
            privacyText += QStringLiteral("/applied");
        }
        if (privacy.value(QStringLiteral("changed")).toBool()) {
            privacyText += QStringLiteral("/changed");
        }
        if (privacy.value(QStringLiteral("muted")).toBool()) {
            privacyText += QStringLiteral("/muted");
            const int mutedFrames = privacy.value(QStringLiteral("mutedFrames")).toInt();
            if (mutedFrames > 0) {
                privacyText += QStringLiteral("%1").arg(mutedFrames);
            }
        }
        const QString lateEntryMi = privacy.value(QStringLiteral("leMi")).toString();
        if (!lateEntryMi.isEmpty()) {
            privacyText += QStringLiteral("/leMi:%1").arg(lateEntryMi);
            if (privacy.value(QStringLiteral("leCrcOk")).toBool()) {
                privacyText += QStringLiteral("/leOk");
            } else {
                privacyText += QStringLiteral("/leBad");
            }
            const QString lateEntrySource = privacy.value(QStringLiteral("leSource")).toString();
            if (!lateEntrySource.isEmpty()) {
                privacyText += QStringLiteral("/%1").arg(lateEntrySource);
            }
        }
        if (privacy.value(QStringLiteral("known")).toBool()) {
            privacyText += privacy.value(QStringLiteral("encrypted")).toBool()
                               ? QStringLiteral("/enc")
                               : QStringLiteral("/clear");
        } else {
            privacyText += QStringLiteral("/unknown");
        }
        const QString privacyError = privacy.value(QStringLiteral("error")).toString();
        if (!privacyError.isEmpty()) {
            privacyText += QStringLiteral("/err:%1").arg(privacyError.left(32));
        }
        parts << QStringLiteral("priv=%1").arg(privacyText);
    }

    const QJsonObject lc = obj.value(QStringLiteral("lc")).toObject();
    if (!lc.isEmpty()) {
        parts << QStringLiteral("src=%1").arg(lc.value(QStringLiteral("src")).toInt());
        parts << QStringLiteral("dst=%1").arg(lc.value(QStringLiteral("dst")).toInt());
    }

    return parts.join(QLatin1Char(' ')).left(220);
}
} // namespace

GopherTrunkBridge::GopherTrunkBridge(QObject *parent)
    : QObject(parent),
      managedProcess(new QProcess(this)),
      tcpSocket(new QTcpSocket(this)),
      udpOutputSocket(new QUdpSocket(this)) {
    connect(tcpSocket, &QTcpSocket::connected, this, [this]() {
        tcpConnectPending = false;
        emitStatus(QStringLiteral("GopherTrunk connected on TCP 127.0.0.1:%1").arg(tcpPort));
        resetStream();
    });
    connect(tcpSocket, &QTcpSocket::disconnected, this, [this]() {
        if (bridgeEnabled) {
            emitStatus(QStringLiteral("GopherTrunk TCP disconnected"));
            QTimer::singleShot(500, this, [this]() { connectTcpIfNeeded(); });
        }
    });
    connect(tcpSocket,
            QOverload<QAbstractSocket::SocketError>::of(&QTcpSocket::errorOccurred),
            this,
            [this](QAbstractSocket::SocketError) {
                if (bridgeEnabled && tcpSocket->state() != QAbstractSocket::ConnectedState) {
                    QTimer::singleShot(750, this, [this]() { connectTcpIfNeeded(); });
                }
            });
    connect(tcpSocket, &QTcpSocket::readyRead, this, &GopherTrunkBridge::readTcpOutput);
    connect(udpOutputSocket, &QUdpSocket::readyRead, this, &GopherTrunkBridge::readUdpOutput);
    connect(managedProcess, &QProcess::readyReadStandardOutput, this, &GopherTrunkBridge::readProcessOutput);
    connect(managedProcess, &QProcess::readyReadStandardError, this, &GopherTrunkBridge::readProcessOutput);
    connect(managedProcess, &QProcess::started, this, [this]() {
        processRestartBackoffMs = 0;
        processRestartBackoffTimer.invalidate();
        QTimer::singleShot(250, this, [this]() { connectTcpIfNeeded(); });
    });
    connect(managedProcess,
            QOverload<QProcess::ProcessError>::of(&QProcess::errorOccurred),
            this,
            [this](QProcess::ProcessError error) {
                if (processStoppingIntentionally) {
                    return;
                }
                armProcessRestartBackoff(managedProcess->errorString());
                emitStatus(QStringLiteral("GopherTrunk process error %1: %2")
                               .arg(static_cast<int>(error))
                               .arg(managedProcess->errorString()));
            });
    connect(managedProcess,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this,
            [this](int exitCode, QProcess::ExitStatus exitStatus) {
                const bool intentionalStop =
                    processStoppingIntentionally || !bridgeEnabled || !processAutoStart;
                processStoppingIntentionally = false;
                const QString stoppedText =
                    QStringLiteral("GopherTrunk stopped: code %1, status %2")
                        .arg(exitCode)
                        .arg(static_cast<int>(exitStatus));
                if (intentionalStop) {
                    if (fobosVerboseLoggingEnabled()) {
                        qDebug() << "[GopherTrunk]" << stoppedText;
                    }
                } else {
                    emitStatus(stoppedText);
                }
                if (!intentionalStop &&
                    (exitCode != 0 || exitStatus != QProcess::NormalExit)) {
                    armProcessRestartBackoff(QStringLiteral("process exited"));
                }
                if (!intentionalStop && bridgeEnabled && processAutoStart) {
                    const int delayMs = static_cast<int>((std::max<qint64>)(250, processRestartBackoffMs));
                    QTimer::singleShot(delayMs, this, [this]() { startProcessIfNeeded(); });
                }
            });
}

GopherTrunkBridge::~GopherTrunkBridge() {
    disconnect(this, nullptr, nullptr, nullptr);
    setEnabled(false);
    if (managedProcess && managedProcess->state() != QProcess::NotRunning) {
        managedProcess->kill();
        managedProcess->waitForFinished(500);
    }
}

void GopherTrunkBridge::configure(bool autoStart,
                                  const QString &program,
                                  quint16 tcpPortValue,
                                  quint16 audioUdpPortValue,
                                  const QString &workingDirectory) {
    const QString nextProgram = program.trimmed();
    const quint16 nextTcpPort = tcpPortValue == 0 ? quint16(7460) : tcpPortValue;
    const quint16 nextAudioUdpPort = audioUdpPortValue == 0 ? quint16(23456) : audioUdpPortValue;
    const QStringList nextArguments = QStringList()
                                      << QStringLiteral("-listen")
                                      << QStringLiteral("127.0.0.1:%1").arg(nextTcpPort)
                                      << QStringLiteral("-interleaved=false")
                                      << QStringLiteral("-audio-udp")
                                      << QStringLiteral("127.0.0.1:%1").arg(nextAudioUdpPort);
    const bool processChanged = processAutoStart != autoStart ||
                                processProgram != nextProgram ||
                                processArguments != nextArguments ||
                                processWorkingDirectory != workingDirectory;
    const bool tcpChanged = tcpPort != nextTcpPort;
    const bool udpChanged = audioUdpPort != nextAudioUdpPort;
    if (bridgeEnabled) {
        if (tcpChanged) {
            disconnectTcp();
        }
        if (processChanged && managedProcess->state() != QProcess::NotRunning) {
            stopProcess();
        }
        if (udpChanged) {
            stopUdpOutput();
        }
    }

    processAutoStart = autoStart;
    processProgram = nextProgram;
    tcpPort = nextTcpPort;
    audioUdpPort = nextAudioUdpPort;
    processArguments = nextArguments;
    processWorkingDirectory = workingDirectory;
    if (!bridgeEnabled) {
        return;
    }
    startUdpOutput();
    if (processAutoStart) {
        startProcessIfNeeded();
    }
    connectTcpIfNeeded();
}

void GopherTrunkBridge::configurePrivacy(const QString &mode,
                                         int keyId,
                                         const QString &keyHex,
                                         bool forwardToBackend,
                                         const QString &variant,
                                         const QString &layout,
                                         int frameOffset) {
    privacyMode = mode.trimmed().toLower();
    if (privacyMode.isEmpty()) {
        privacyMode = QStringLiteral("none");
    }
    privacyKeyId = keyId;
    privacyKeyHex = keyHex.trimmed();
    privacyForwardToBackend = forwardToBackend;
    privacyVariant = variant.trimmed().isEmpty() ? QStringLiteral("dmra") : variant.trimmed().toLower();
    privacyLayout = layout.trimmed().isEmpty() ? QStringLiteral("normal") : layout.trimmed().toLower();
    privacyFrameOffset = (std::clamp)(frameOffset, 0, 17);
    const QString overrideDirectory =
        processWorkingDirectory.trimmed().isEmpty()
            ? QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(QStringLiteral("gophertrunk"))
            : processWorkingDirectory;
    QDir().mkpath(overrideDirectory);
    privacyOverridePath = QDir(overrideDirectory).absoluteFilePath(QStringLiteral("privacy_override.txt"));
    QFile overrideFile(privacyOverridePath);
    if (overrideFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        const QByteArray body =
            QStringLiteral("variant=%1\nlayout=%2\nframeOffset=%3\n")
                .arg(privacyVariant, privacyLayout)
                .arg(privacyFrameOffset)
                .toUtf8();
        overrideFile.write(body);
    } else if (fobosVerboseLoggingEnabled()) {
        qWarning() << "[GopherTrunk] privacy override write failed" << privacyOverridePath
                   << overrideFile.errorString();
    }
    const QString summary = QStringLiteral("%1:%2:%3:%4:%5:%6:%7")
                                .arg(privacyMode)
                                .arg(privacyKeyId)
                                .arg(privacyKeyHex.isEmpty() ? QStringLiteral("empty") : QStringLiteral("set"))
                                .arg(privacyForwardToBackend)
                                .arg(privacyVariant)
                                .arg(privacyLayout)
                                .arg(privacyFrameOffset);
    const QString resetSummary = QStringLiteral("%1:%2:%3:%4:%5:%6:%7")
                                     .arg(privacyMode)
                                     .arg(privacyKeyId)
                                     .arg(privacyKeyHex)
                                     .arg(privacyForwardToBackend)
                                     .arg(privacyVariant)
                                     .arg(privacyLayout)
                                     .arg(privacyFrameOffset);
    if (summary != lastPrivacySummary) {
        lastPrivacySummary = summary;
        emitStatus(QStringLiteral("GopherTrunk privacy draft: %1, keyId %2, key %3, forward %4, %5/%6/off%7")
                       .arg(privacyMode)
                       .arg(privacyKeyId)
                       .arg(privacyKeyHex.isEmpty() ? QStringLiteral("empty") : QStringLiteral("set"))
                       .arg(privacyForwardToBackend)
                       .arg(privacyVariant, privacyLayout)
                       .arg(privacyFrameOffset));
    }
    if (resetSummary != lastPrivacyResetSummary) {
        lastPrivacyResetSummary = resetSummary;
        resetStream();
    }
}

void GopherTrunkBridge::setEnabled(bool enabled) {
    if (bridgeEnabled == enabled) {
        if (bridgeEnabled) {
            startUdpOutput();
            if (processAutoStart) {
                startProcessIfNeeded();
            }
            connectTcpIfNeeded();
        }
        return;
    }

    bridgeEnabled = enabled;
    if (bridgeEnabled) {
        dibitPacketsSent = 0;
        virtualBaseDibit = 0;
        tcpOutputLines = 0;
        udpOutputPacketsReceived = 0;
        streamPolarityKnown = false;
        streamPolarityFlip = false;
        lastColorCode = -1;
        startUdpOutput();
        if (processAutoStart) {
            startProcessIfNeeded();
        }
        connectTcpIfNeeded();
        emitStatus(QStringLiteral("GopherTrunk bridge enabled"));
    } else {
        disconnectTcp();
        stopProcess();
        stopUdpOutput();
        emitStatus(QStringLiteral("GopherTrunk bridge disabled"));
    }
}

QJsonObject GopherTrunkBridge::currentPrivacyObject() const {
    QJsonObject privacy;
    const bool forwardPrivacy =
        privacyForwardToBackend && privacyMode != QStringLiteral("none");
    privacy.insert(QStringLiteral("mode"),
                   forwardPrivacy ? privacyMode : QStringLiteral("none"));
    privacy.insert(QStringLiteral("keyId"), forwardPrivacy ? privacyKeyId : 0);
    privacy.insert(QStringLiteral("keyHex"),
                   forwardPrivacy ? privacyKeyHex : QString());
    privacy.insert(QStringLiteral("variant"), privacyVariant);
    privacy.insert(QStringLiteral("layout"), privacyLayout);
    privacy.insert(QStringLiteral("frameOffset"), privacyFrameOffset);
    return privacy;
}

QString GopherTrunkBridge::normalizedBurstDibits(const QString &dibits, bool dataBurst,
                                               bool voicePolarityFlip) {
    if (dibits.size() != 132) {
        return {};
    }
    if (!dataBurst) {
        return applyDmrVoicePolarityForGopher(dibits, voicePolarityFlip);
    }
    // Data syncs are the polarity-flipped twins of voice syncs. A PI burst
    // is sliced independently, so it must not inherit the last voice polarity.
    bool dataFlip = false;
    if (!detectDmrVoicePolarityFlip(rotateDibitPolarity(dibits), &dataFlip)) {
        return {};
    }
    return applyDmrVoicePolarityForGopher(dibits, dataFlip);
}

void GopherTrunkBridge::resetStream() {
    virtualBaseDibit = 0;
    lastColorCode = -1;
    streamPolarityKnown = false;
    streamPolarityFlip = false;
    if (!bridgeEnabled || tcpSocket->state() != QAbstractSocket::ConnectedState) {
        return;
    }
    QJsonObject packet;
    packet.insert(QStringLiteral("reset"), true);
    packet.insert(QStringLiteral("privacy"), currentPrivacyObject());
    QByteArray line = QJsonDocument(packet).toJson(QJsonDocument::Compact);
    line.append('\n');
    tcpSocket->write(line);
}

void GopherTrunkBridge::sendDibitBurst(const QString &dibits,
                                       const QString &burstKind,
                                       int dataType,
                                       const QString &dataTypeName,
                                       int slotTypeErrors,
                                       quint64 sample,
                                       quint64 baseDibit,
                                       int burstIndex,
                                       int cadenceSymbols,
                                       int colorCode,
                                       int timeslot,
                                       const QString &timeslotSource,
                                       bool cachDecoded,
                                       int cachChannel,
                                       int cachLcss,
                                       const QString &cachMapName,
                                       const QString &cachPayloadBits,
                                       const QStringList &ambe49Payloads) {
    if (!bridgeEnabled || dibits.size() != 132) {
        return;
    }
    if (processAutoStart) {
        startProcessIfNeeded();
    }
    if (tcpSocket->state() != QAbstractSocket::ConnectedState) {
        connectTcpIfNeeded();
        return;
    }

    if (colorCode >= 0) {
        if (fobosVerboseLoggingEnabled() &&
            lastColorCode >= 0 &&
            colorCode != lastColorCode) {
            qDebug() << "[GopherTrunk] color code changed without stream reset"
                     << lastColorCode << "->" << colorCode
                     << "sample" << static_cast<qulonglong>(sample)
                     << "burst" << burstIndex;
        }
        lastColorCode = colorCode;
    }

    const bool isDataBurst = burstKind.compare(QStringLiteral("data"), Qt::CaseInsensitive) == 0;
    bool detectedPolarityFlip = false;
    // Only burst A carries a voice sync. Embedded B-F fields can accidentally
    // resemble one; never let them lock polarity or reset privacy alignment.
    if (!isDataBurst && burstIndex == 0 && detectDmrVoicePolarityFlip(dibits, &detectedPolarityFlip)) {
        if (!streamPolarityKnown || streamPolarityFlip != detectedPolarityFlip) {
            if (fobosVerboseLoggingEnabled()) {
                qDebug() << "[GopherTrunk] stream dibit polarity"
                         << "flip" << detectedPolarityFlip
                         << "sample" << static_cast<qulonglong>(sample)
                         << "burst" << burstIndex
                         << "cc" << colorCode;
            }
        }
        streamPolarityKnown = true;
        streamPolarityFlip = detectedPolarityFlip;
    }
    if (!streamPolarityKnown && !isDataBurst) {
        return;
    }
    const bool polarityFlipped = streamPolarityKnown && streamPolarityFlip;
    const QString normalizedDibits = normalizedBurstDibits(dibits, isDataBurst, polarityFlipped);
    if (normalizedDibits.isEmpty()) {
        return;
    }
    const quint64 packetBaseDibit = virtualBaseDibit;
    QJsonObject packet;
    packet.insert(QStringLiteral("baseIdx"), static_cast<double>(packetBaseDibit));
    packet.insert(QStringLiteral("dibits"), normalizedDibits);
    packet.insert(QStringLiteral("burstKind"), burstKind.isEmpty() ? QStringLiteral("voice") : burstKind);
    if (dataType >= 0) {
        packet.insert(QStringLiteral("dataType"), dataType);
    }
    if (!dataTypeName.isEmpty()) {
        packet.insert(QStringLiteral("dataTypeName"), dataTypeName);
    }
    if (slotTypeErrors >= 0) {
        packet.insert(QStringLiteral("slotTypeErrors"), slotTypeErrors);
    }
    packet.insert(QStringLiteral("sample"), QString::number(sample));
    packet.insert(QStringLiteral("rfBaseIdx"), QString::number(baseDibit));
    packet.insert(QStringLiteral("burst"), burstIndex);
    packet.insert(QStringLiteral("cadenceSymbols"), cadenceSymbols);
    if (timeslot >= 1 && timeslot <= 2) {
        packet.insert(QStringLiteral("timeslot"), timeslot);
    }
    if (!timeslotSource.isEmpty()) {
        packet.insert(QStringLiteral("timeslotSource"), timeslotSource);
    }
    QJsonObject cach;
    cach.insert(QStringLiteral("decoded"), cachDecoded);
    if (cachDecoded) {
        cach.insert(QStringLiteral("channel"), cachChannel);
        cach.insert(QStringLiteral("lcss"), cachLcss);
        cach.insert(QStringLiteral("map"), cachMapName);
        cach.insert(QStringLiteral("payload"), cachPayloadBits);
    }
    packet.insert(QStringLiteral("cach"), cach);
    packet.insert(QStringLiteral("polarityKnown"), streamPolarityKnown);
    packet.insert(QStringLiteral("polarityNormalized"), polarityFlipped);
    if (!ambe49Payloads.isEmpty()) {
        QJsonArray ambe49;
        for (const QString &payload : ambe49Payloads) {
            ambe49.append(payload);
        }
        packet.insert(QStringLiteral("ambe49"), ambe49);
    }
    packet.insert(QStringLiteral("privacy"), currentPrivacyObject());
    if (colorCode >= 0) {
        packet.insert(QStringLiteral("colorCode"), colorCode);
    }
    QByteArray line = QJsonDocument(packet).toJson(QJsonDocument::Compact);
    line.append('\n');
    tcpSocket->write(line);
    virtualBaseDibit += static_cast<quint64>(dibits.size());
    ++dibitPacketsSent;
    if (fobosVerboseLoggingEnabled() &&
        (dibitPacketsSent <= 8 || (dibitPacketsSent % 100) == 0)) {
        qDebug() << "[GopherTrunk] sent dibit burst"
                 << "packet" << dibitPacketsSent
                 << "sample" << static_cast<qulonglong>(sample)
                 << "baseIdx" << static_cast<qulonglong>(packetBaseDibit)
                 << "rfBaseIdx" << static_cast<qulonglong>(baseDibit)
                 << "burst" << burstIndex
                 << "kind" << burstKind
                 << "dt" << dataType
                 << "cc" << colorCode
                 << "ts" << timeslot
                 << "tsSource" << timeslotSource
                 << "cach" << cachDecoded
                 << "cachCh" << cachChannel
                 << "ambe49" << ambe49Payloads.size()
                 << "polarityKnown" << streamPolarityKnown
                 << "polarityFlip" << polarityFlipped;
    }
}

void GopherTrunkBridge::startProcessIfNeeded() {
    if (!processAutoStart || !bridgeEnabled || processProgram.trimmed().isEmpty()) {
        return;
    }
    if (managedProcess->state() != QProcess::NotRunning || processRestartBackoffActive()) {
        return;
    }
    processLogLines = 0;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("FOBOS_DMR_PRIVACY_MODE"), privacyMode);
    environment.insert(QStringLiteral("FOBOS_DMR_PRIVACY_KEY_ID"), QString::number(privacyKeyId));
    environment.insert(QStringLiteral("FOBOS_DMR_PRIVACY_KEY_HEX"), privacyKeyHex);
    environment.insert(QStringLiteral("FOBOS_DMR_PRIVACY_FORWARD"), privacyForwardToBackend ? QStringLiteral("1") : QStringLiteral("0"));
    environment.insert(QStringLiteral("FOBOS_DMR_PRIVACY_VARIANT"), privacyVariant);
    environment.insert(QStringLiteral("FOBOS_DMR_PRIVACY_LAYOUT"), privacyLayout);
    environment.insert(QStringLiteral("FOBOS_DMR_PRIVACY_FRAME_OFFSET"), QString::number(privacyFrameOffset));
    if (!privacyOverridePath.isEmpty()) {
        environment.insert(QStringLiteral("FOBOS_DMR_PRIVACY_OVERRIDE_FILE"), privacyOverridePath);
    }
    const GopherProcessArguments arguments =
        buildGopherProcessArguments(processArguments,
                                    privacyMode,
                                    privacyKeyId,
                                    privacyKeyHex,
                                    privacyForwardToBackend,
                                    privacyVariant,
                                    privacyLayout,
                                    privacyFrameOffset,
                                    privacyOverridePath);
    managedProcess->setProcessEnvironment(environment);
    managedProcess->setProgram(processProgram);
    managedProcess->setArguments(arguments.actual);
    if (!processWorkingDirectory.trimmed().isEmpty()) {
        managedProcess->setWorkingDirectory(processWorkingDirectory);
    }
    managedProcess->start();
    emitStatus(QStringLiteral("Starting GopherTrunk: %1 %2")
                   .arg(processProgram, arguments.display.join(QLatin1Char(' '))));
}

void GopherTrunkBridge::stopProcess() {
    if (!managedProcess || managedProcess->state() == QProcess::NotRunning) {
        return;
    }
    processStoppingIntentionally = true;
    managedProcess->terminate();
    if (!managedProcess->waitForFinished(1500) &&
        managedProcess->state() != QProcess::NotRunning) {
        managedProcess->kill();
        managedProcess->waitForFinished(500);
    }
}

void GopherTrunkBridge::connectTcpIfNeeded() {
    if (!bridgeEnabled || tcpConnectPending ||
        tcpSocket->state() == QAbstractSocket::ConnectedState ||
        tcpSocket->state() == QAbstractSocket::ConnectingState) {
        return;
    }
    tcpConnectPending = true;
    tcpSocket->connectToHost(QHostAddress::LocalHost, tcpPort);
}

void GopherTrunkBridge::disconnectTcp() {
    tcpConnectPending = false;
    if (tcpSocket->state() != QAbstractSocket::UnconnectedState) {
        tcpSocket->disconnectFromHost();
        if (tcpSocket->state() != QAbstractSocket::UnconnectedState) {
            tcpSocket->abort();
        }
    }
}

void GopherTrunkBridge::startUdpOutput() {
    if (!bridgeEnabled) {
        return;
    }
    if (udpOutputSocket->state() == QAbstractSocket::BoundState &&
        udpOutputSocket->localPort() == audioUdpPort) {
        return;
    }
    stopUdpOutput();
    const bool bound = udpOutputSocket->bind(QHostAddress::LocalHost,
                                             audioUdpPort,
                                             QUdpSocket::ShareAddress |
                                                 QUdpSocket::ReuseAddressHint);
    if (!bound) {
        emitStatus(QStringLiteral("GopherTrunk UDP audio bind failed on 127.0.0.1:%1: %2")
                       .arg(audioUdpPort)
                       .arg(udpOutputSocket->errorString()));
        return;
    }
    emitStatus(QStringLiteral("GopherTrunk UDP audio input ready: 127.0.0.1:%1").arg(audioUdpPort));
}

void GopherTrunkBridge::stopUdpOutput() {
    if (udpOutputSocket->state() != QAbstractSocket::UnconnectedState) {
        udpOutputSocket->close();
    }
}

void GopherTrunkBridge::readTcpOutput() {
    while (tcpSocket->canReadLine()) {
        const QString line = QString::fromUtf8(tcpSocket->readLine()).trimmed();
        if (line.isEmpty()) {
            continue;
        }
        ++tcpOutputLines;
        const bool privacyLine =
            line.contains(QStringLiteral("\"privacy\"")) ||
            line.contains(QStringLiteral("\"pi\""));
        if (privacyLine ||
            tcpOutputLines <= 16 ||
            (tcpOutputLines % 40) == 0) {
            emitStatus(QStringLiteral("GopherTrunk decoded: %1").arg(compactGopherSummary(line)));
        }
        if (privacyLine && fobosVerboseLoggingEnabled()) {
            qDebug() << "[GopherTrunk output privacy]" << line;
        } else if (fobosVerboseLoggingEnabled()) {
            qDebug() << "[GopherTrunk output]" << line.left(240);
        }
    }
}

void GopherTrunkBridge::readProcessOutput() {
    const QByteArray output = managedProcess->readAllStandardOutput() +
                              managedProcess->readAllStandardError();
    const QList<QByteArray> lines = output.split('\n');
    for (const QByteArray &rawLine : lines) {
        const QString line = QString::fromUtf8(rawLine).trimmed();
        if (line.isEmpty()) {
            continue;
        }
        ++processLogLines;
        const bool privacyLine =
            line.contains(QStringLiteral("\"privacy\"")) ||
            line.contains(QStringLiteral("\"privacyText\"")) ||
            line.contains(QStringLiteral("\"pi\"")) ||
            line.contains(QStringLiteral("\"piAccepted\"")) ||
            line.contains(QStringLiteral("\"piReject\""));
        if (privacyLine ||
            processLogLines <= 20 ||
            (processLogLines % 50) == 0) {
            emitStatus(QStringLiteral("GopherTrunk: %1").arg(line.left(160)));
        }
        if (privacyLine && fobosVerboseLoggingEnabled()) {
            qDebug() << "[GopherTrunk process privacy]" << line;
        } else if (fobosVerboseLoggingEnabled()) {
            qDebug() << "[GopherTrunk process]" << line;
        }
    }
}

void GopherTrunkBridge::readUdpOutput() {
    while (udpOutputSocket->hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(static_cast<int>(udpOutputSocket->pendingDatagramSize()));
        udpOutputSocket->readDatagram(datagram.data(), datagram.size());
        const QByteArray mono8k = pcm16ToMono(datagram, 1);
        if (mono8k.isEmpty()) {
            continue;
        }
        ++udpOutputPacketsReceived;
        if (udpOutputPacketsReceived <= 8 || (udpOutputPacketsReceived % 100) == 0) {
            emitStatus(QStringLiteral("GopherTrunk audio: packet %1, %2 bytes")
                           .arg(udpOutputPacketsReceived)
                           .arg(mono8k.size()));
        }
        const QByteArray normalized = normalizeOutputPcmForPlayback(mono8k, udpOutputPacketsReceived);
        emit decodedPcmReady(upsample8kMonoTo48k(normalized));
    }
}

void GopherTrunkBridge::emitStatus(const QString &status) {
    if (fobosVerboseLoggingEnabled()) {
        qDebug() << "[GopherTrunk]" << status;
    }
    emit statusChanged(status);
}

bool GopherTrunkBridge::processRestartBackoffActive() const {
    return processRestartBackoffMs > 0 &&
           processRestartBackoffTimer.isValid() &&
           processRestartBackoffTimer.elapsed() < processRestartBackoffMs;
}

void GopherTrunkBridge::armProcessRestartBackoff(const QString &reason) {
    processRestartBackoffMs = processRestartBackoffMs <= 0
                                  ? qint64(1000)
                                  : (std::min)(processRestartBackoffMs * 2, PROCESS_RESTART_BACKOFF_MAX_MS);
    processRestartBackoffTimer.restart();
    emitStatus(QStringLiteral("GopherTrunk restart backoff %1 ms: %2")
                   .arg(processRestartBackoffMs)
                   .arg(reason.left(120)));
}

QByteArray GopherTrunkBridge::pcm16ToMono(const QByteArray &pcmData, int channels) const {
    const int safeChannels = std::clamp(channels, 1, 2);
    const int frameBytes = safeChannels * int(sizeof(qint16));
    const int frames = pcmData.size() / frameBytes;
    if (frames <= 0) {
        return {};
    }
    if (safeChannels == 1) {
        QByteArray mono = pcmData.left(frames * frameBytes);
        if (mono.size() & 1) {
            mono.chop(1);
        }
        return mono;
    }

    QByteArray mono;
    mono.reserve(frames * int(sizeof(qint16)));
    const char *raw = pcmData.constData();
    for (int frame = 0; frame < frames; ++frame) {
        const qint16 left = readLe16(raw + frame * frameBytes);
        const qint16 right = readLe16(raw + frame * frameBytes + int(sizeof(qint16)));
        appendLe16(mono, static_cast<qint16>((static_cast<int>(left) + static_cast<int>(right)) / 2));
    }
    return mono;
}

QByteArray GopherTrunkBridge::normalizeOutputPcmForPlayback(const QByteArray &pcmData,
                                                            quint64 packetIndex) const {
    const int samples = pcmData.size() / int(sizeof(qint16));
    if (samples <= 0) {
        return {};
    }

    const char *raw = pcmData.constData();
    int peak = 0;
    double squareSum = 0.0;
    for (int i = 0; i < samples; ++i) {
        const int sample = readLe16(raw + i * int(sizeof(qint16)));
        peak = (std::max)(peak, std::abs(sample));
        const double normalized = static_cast<double>(sample) / 32768.0;
        squareSum += normalized * normalized;
    }

    const double peakNorm = static_cast<double>(peak) / 32767.0;
    const double rms = std::sqrt(squareSum / static_cast<double>((std::max)(1, samples)));
    double gain = 1.0;
    if (peakNorm > 0.0 && peakNorm < GOPHER_OUTPUT_TARGET_PEAK) {
        gain = (std::min)(GOPHER_OUTPUT_TARGET_PEAK / peakNorm, GOPHER_OUTPUT_MAX_GAIN);
    }

    if (fobosVerboseLoggingEnabled() &&
        (packetIndex <= 8 || (packetIndex % 100) == 0)) {
        qDebug() << "[GopherTrunk] decoded UDP audio"
                 << "packet" << packetIndex
                 << "samples" << samples
                 << "peak" << QString::number(peakNorm, 'f', 4)
                 << "rms" << QString::number(rms, 'f', 4)
                 << "gain" << QString::number(gain, 'f', 2);
    }

    if (gain <= 1.001) {
        return pcmData.left(samples * int(sizeof(qint16)));
    }

    QByteArray output;
    output.reserve(samples * int(sizeof(qint16)));
    for (int i = 0; i < samples; ++i) {
        const int sample = readLe16(raw + i * int(sizeof(qint16)));
        const int scaled = static_cast<int>(std::lrint(static_cast<double>(sample) * gain));
        const int clamped = (std::clamp)(scaled, -32768, 32767);
        appendLe16(output, static_cast<qint16>(clamped));
    }
    return output;
}

QByteArray GopherTrunkBridge::upsample8kMonoTo48k(const QByteArray &pcmData) const {
    const int inputSamples = pcmData.size() / int(sizeof(qint16));
    if (inputSamples <= 0) {
        return {};
    }

    QByteArray output;
    output.reserve(inputSamples * UPSAMPLE_FACTOR * int(sizeof(qint16)));
    const char *raw = pcmData.constData();
    for (int i = 0; i < inputSamples; ++i) {
        const qint16 current = readLe16(raw + i * int(sizeof(qint16)));
        const qint16 next =
            (i + 1 < inputSamples)
                ? readLe16(raw + (i + 1) * int(sizeof(qint16)))
                : current;
        for (int step = 0; step < UPSAMPLE_FACTOR; ++step) {
            const float fraction = static_cast<float>(step) / static_cast<float>(UPSAMPLE_FACTOR);
            const float sample =
                static_cast<float>(current) +
                fraction * static_cast<float>(static_cast<int>(next) - static_cast<int>(current));
            appendLe16(output, static_cast<qint16>((std::clamp)(static_cast<int>(std::lrint(sample)),
                                                                -32768,
                                                                32767)));
        }
    }
    return output;
}

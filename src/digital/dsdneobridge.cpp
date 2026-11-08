#include "dsdneobridge.h"

#include "diagnosticlogging.h"

#include <QDataStream>
#include <QHostAddress>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QTimer>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace {
constexpr qint64 MAX_UDP_INPUT_DATAGRAM_BYTES = 1200;
constexpr qint64 PROCESS_RESTART_BACKOFF_MAX_MS = 10000;
constexpr int DEFAULT_DSD_AUDIO_RATE = 8000;
constexpr int APP_AUDIO_RATE = 48000;
constexpr int UPSAMPLE_FACTOR = APP_AUDIO_RATE / DEFAULT_DSD_AUDIO_RATE;
constexpr int DSD_INPUT_WARMUP_FRAMES = 3;
constexpr int PROCESS_STOP_GRACE_MS = 250;
constexpr int PROCESS_CONFIG_SETTLE_MS = 100;
constexpr int PROCESS_STABLE_RUN_MS = 10000;
constexpr int MAX_STDIN_BACKLOG_MS = 200;

QString defaultDsdNeoProgram() {
#if defined(Q_OS_WIN)
    return QStringLiteral("dsd-neo/dsd-neo.exe");
#else
    return QStringLiteral("dsd-neo");
#endif
}

bool stringListsEqual(const QStringList &lhs, const QStringList &rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (int i = 0; i < lhs.size(); ++i) {
        if (lhs.at(i) != rhs.at(i)) {
            return false;
        }
    }
    return true;
}

QString normalizedPrivacyKey(QString key) {
    key = key.trimmed();
    if (key.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) {
        key.remove(0, 2);
    }
    key.remove(QRegularExpression(QStringLiteral("[\\s:_-]")));
    return key.toUpper();
}

bool isHexKey(const QString &key, int expectedCharacters) {
    if (key.size() != expectedCharacters) {
        return false;
    }
    for (const QChar character : key) {
        const ushort value = character.unicode();
        const bool decimal = value >= '0' && value <= '9';
        const bool upperHex = value >= 'A' && value <= 'F';
        if (!decimal && !upperHex) {
            return false;
        }
    }
    return true;
}

DsdNeoBridge::PrivacyArguments buildDsdProcessArguments(const QStringList &baseArguments,
                                             const QString &mode,
                                             const QString &keyHex,
                                             bool forwardToBackend) {
    DsdNeoBridge::PrivacyArguments result;
    for (int index = 0; index < baseArguments.size(); ++index) {
        const QString argument = baseArguments.at(index);
        // Basic enforcement belongs to the selected mode, not the previous key.
        if (argument == QLatin1String("-4")) continue;
        if (argument == QLatin1String("-1") || argument == QLatin1String("-H")) {
            if (index + 1 < baseArguments.size()) {
                ++index;
            }
            continue;
        }
        result.actual.append(argument);
        result.display.append(argument);
    }

    if (!forwardToBackend) {
        result.privacyMode = QStringLiteral("none");
        return result;
    }

    const QString normalizedMode = mode.trimmed().toLower();
    const QString normalizedKey = normalizedPrivacyKey(keyHex);
    QString flag;
    int expectedCharacters = 0;
    if (normalizedMode == QLatin1String("arc4") || normalizedMode == QLatin1String("rc4")) {
        result.privacyMode = QStringLiteral("arc4");
        flag = QStringLiteral("-1");
        expectedCharacters = 10;
    } else if (normalizedMode == QLatin1String("aes256") ||
               normalizedMode == QLatin1String("aes-256")) {
        result.privacyMode = QStringLiteral("aes256");
        flag = QStringLiteral("-H");
        expectedCharacters = 64;
    } else if (normalizedMode == QLatin1String("hytera-bp")) {
        result.privacyMode = QStringLiteral("hytera-bp");
        flag = QStringLiteral("-H");
        expectedCharacters = normalizedKey.size();
        if (expectedCharacters != 10 && expectedCharacters != 32 && expectedCharacters != 64) {
            result.privacyError = QStringLiteral("Hytera Basic key was not forwarded: expected 10, 32 or 64 hexadecimal characters (40/128/256 bits)");
            return result;
        }
    } else {
        result.privacyMode = QStringLiteral("none");
        return result;
    }

    if (!isHexKey(normalizedKey, expectedCharacters)) {
        result.privacyError = QStringLiteral("%1 key was not forwarded: expected %2 hexadecimal characters")
                                  .arg(result.privacyMode.toUpper())
                                  .arg(expectedCharacters);
        return result;
    }

    QString cliKey = normalizedKey;
    if (result.privacyMode == QLatin1String("hytera-bp")) {
        QStringList words;
        for (int offset = 0; offset < normalizedKey.size(); offset += 16)
            words << normalizedKey.mid(offset, 16);
        cliKey = words.join(QLatin1Char(' '));
        // -H is shared with AES. Explicit Basic enforcement avoids ALG guessing.
        result.actual << QStringLiteral("-4");
        result.display << QStringLiteral("-4");
    }
    result.actual << flag << cliKey;
    result.display << flag
                   << QStringLiteral("<redacted:%1-bytes>").arg(expectedCharacters / 2);
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
} // namespace

DsdNeoBridge::PrivacyArguments DsdNeoBridge::privacyProcessArguments(
    const QStringList &baseArguments, const QString &mode, const QString &keyHex,
    bool forwardToBackend) {
    return buildDsdProcessArguments(baseArguments, mode, keyHex, forwardToBackend);
}

QStringList DsdNeoBridge::dmrProcessArguments(int sampleRate, quint16 outputPort) {
    // -nm forces the legacy mono decoder, not merely a mono audio output.
    // Keep the default DMR slot-aware path and downmix returned stereo in the bridge.
    return {
        QStringLiteral("-fs"),
        QStringLiteral("-i"), QStringLiteral("-"),
        QStringLiteral("-s"), QString::number(sampleRate == 48000 ? 48000 : 24000),
        QStringLiteral("-o"), QStringLiteral("udp:127.0.0.1:%1").arg(outputPort),
        QStringLiteral("-mc")
    };
}

DsdNeoBridge::DsdNeoBridge(QObject *parent)
    : QObject(parent),
      inputServer(new QTcpServer(this)),
      udpInputSocket(new QUdpSocket(this)),
      udpOutputSocket(new QUdpSocket(this)),
      managedProcess(new QProcess(this)),
      processStopTimer(new QTimer(this)),
      processStartTimer(new QTimer(this)) {
    processStopTimer->setSingleShot(true);
    processStartTimer->setSingleShot(true);
    connect(processStopTimer, &QTimer::timeout, this, [this]() {
        if (processStoppingIntentionally && managedProcess->state() != QProcess::NotRunning)
            managedProcess->kill();
    });
    connect(processStartTimer, &QTimer::timeout, this, &DsdNeoBridge::startProcessIfNeeded);
    connect(inputServer, &QTcpServer::newConnection, this, &DsdNeoBridge::acceptInputClient);
    connect(udpOutputSocket, &QUdpSocket::readyRead, this, &DsdNeoBridge::readUdpOutput);
    connect(managedProcess, &QProcess::readyReadStandardOutput, this, &DsdNeoBridge::readProcessOutput);
    connect(managedProcess, &QProcess::readyReadStandardError, this, &DsdNeoBridge::readProcessOutput);
    connect(managedProcess, &QProcess::started, this, [this]() {
        processRunTimer.restart();
        processRestartBackoffTimer.invalidate();
        if (processStoppingIntentionally) managedProcess->terminate();
    });
    connect(managedProcess,
            QOverload<QProcess::ProcessError>::of(&QProcess::errorOccurred),
            this,
            [this](QProcess::ProcessError error) {
                if (processStoppingIntentionally) {
                    if (error == QProcess::FailedToStart) {
                        // FailedToStart has no finished signal to release a pending stop.
                        processStopTimer->stop();
                        processStoppingIntentionally = false;
                        if (bridgeEnabled && processAutoStart) {
                            armProcessRestartBackoff(managedProcess->errorString());
                            processStartTimer->start(static_cast<int>(processRestartBackoffMs));
                        }
                    }
                    emitStatus(QStringLiteral("DSD-neo process stopped"));
                    return;
                }
                if (error == QProcess::FailedToStart) {
                    armProcessRestartBackoff(managedProcess->errorString());
                    if (bridgeEnabled && processAutoStart) {
                        processStartTimer->start(static_cast<int>(processRestartBackoffMs));
                    }
                }
                if (error == QProcess::Crashed) {
                    qWarning() << "[DSD-neo] Decoder process crashed; waiting for exit status before restart";
                }
                emitStatus(QStringLiteral("DSD-neo process error %1: %2")
                               .arg(static_cast<int>(error))
                               .arg(managedProcess->errorString()));
            });
    connect(managedProcess,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this,
            [this](int exitCode, QProcess::ExitStatus exitStatus) {
                readProcessOutput();
                processStopTimer->stop();
                const bool intentionalStop =
                    processStoppingIntentionally || !bridgeEnabled || !processAutoStart;
                processStoppingIntentionally = false;
                const QString stoppedText =
                    QStringLiteral("DSD-neo stopped: code %1, status %2")
                        .arg(exitCode)
                        .arg(static_cast<int>(exitStatus));
                if (intentionalStop) {
                    if (fobosVerboseLoggingEnabled()) {
                        qDebug() << "[DSD-neo]" << stoppedText;
                    }
                } else {
                    qWarning() << "[DSD-neo]" << stoppedText
                               << "nativeExitCode" << QString::number(quint32(exitCode), 16);
                    emitStatus(stoppedText);
                }
                if (!intentionalStop &&
                    (exitCode != 0 || exitStatus != QProcess::NormalExit)) {
                    armProcessRestartBackoff(QStringLiteral("process exited"));
                }
                if (!intentionalStop && bridgeEnabled && processAutoStart) {
                    const int delayMs = static_cast<int>((std::max<qint64>)(250, processRestartBackoffMs));
                    processStartTimer->start(delayMs);
                } else if (intentionalStop && bridgeEnabled && processAutoStart) {
                    // Apply the latest settings once after a burst of UI changes.
                    processStartTimer->start(PROCESS_CONFIG_SETTLE_MS);
                }
            });
    noClientLogTimer.invalidate();
}

DsdNeoBridge::~DsdNeoBridge() {
    managedProcess->disconnect(this);
    processStartTimer->stop();
    setEnabled(false);
    if (managedProcess && managedProcess->state() != QProcess::NotRunning) {
        managedProcess->kill();
        managedProcess->waitForFinished(500);
    }
}

void DsdNeoBridge::configureInputServer(bool enabled, quint16 port) {
    inputServerEnabled = enabled;
    inputPort = port == 0 ? quint16(7355) : port;
    if (!bridgeEnabled) {
        return;
    }
    if (inputServerEnabled) {
        startInputServer();
    } else {
        stopInputServer();
    }
}

void DsdNeoBridge::configureUdpOutput(bool enabled, quint16 port, int channels) {
    udpOutputEnabled = enabled;
    udpOutputPort = port == 0 ? quint16(23456) : port;
    udpOutputChannels = std::clamp(channels, 1, 2);
    if (!bridgeEnabled) {
        return;
    }
    if (udpOutputEnabled) {
        startUdpOutput();
    } else {
        stopUdpOutput();
    }
}

void DsdNeoBridge::configureProcess(bool autoStart,
                                    const QString &program,
                                    const QStringList &arguments,
                                    const QString &workingDirectory) {
    const bool running = managedProcess->state() != QProcess::NotRunning;
    const bool processChanged =
        processProgram != (program.trimmed().isEmpty() ? defaultDsdNeoProgram() : program.trimmed()) ||
        !stringListsEqual(processArguments, arguments) ||
        processWorkingDirectory != workingDirectory;
    processAutoStart = autoStart;
    processProgram = program.trimmed().isEmpty() ? defaultDsdNeoProgram() : program.trimmed();
    processArguments = arguments;
    processWorkingDirectory = workingDirectory;
    if (!bridgeEnabled) {
        return;
    }
    if (running && processChanged) {
        stopProcess();
    }
    if (processAutoStart) {
        startProcessIfNeeded();
    } else {
        stopProcess();
    }
}

void DsdNeoBridge::configurePrivacy(const QString &mode,
                                    int keyId,
                                    const QString &keyHex,
                                    bool forwardToBackend) {
    QString nextMode = mode.trimmed().toLower();
    if (nextMode.isEmpty()) {
        nextMode = QStringLiteral("none");
    }
    const QString nextKeyHex = normalizedPrivacyKey(keyHex);
    const bool changed = privacyMode != nextMode ||
                         privacyKeyId != keyId ||
                         privacyKeyHex != nextKeyHex ||
                         privacyForwardToBackend != forwardToBackend;
    privacyMode = nextMode;
    privacyKeyId = keyId;
    privacyKeyHex = nextKeyHex;
    privacyForwardToBackend = forwardToBackend;
    emitStatus(QStringLiteral("DSD-neo privacy draft: %1, keyId %2, key %3, forward %4")
                   .arg(privacyMode)
                   .arg(privacyKeyId)
                   .arg(privacyKeyHex.isEmpty() ? QStringLiteral("empty") : QStringLiteral("set"))
                   .arg(privacyForwardToBackend));
    if (changed && bridgeEnabled && managedProcess->state() != QProcess::NotRunning) {
        emitStatus(QStringLiteral("DSD-neo privacy changed; restarting decoder process"));
        stopProcess();
    }
}

void DsdNeoBridge::setEnabled(bool enabled) {
    inputSubmissionEnabled.store(enabled);
    if (!enabled) {
        std::lock_guard<std::mutex> lock(inputQueueMutex);
        inputQueue.clear();
    }
    if (bridgeEnabled == enabled) {
        if (bridgeEnabled) {
            if (inputServerEnabled) {
                startInputServer();
            }
            if (udpOutputEnabled) {
                startUdpOutput();
            }
            if (processAutoStart) {
                startProcessIfNeeded();
            }
        }
        return;
    }

    bridgeEnabled = enabled;
    if (bridgeEnabled) {
        pcmFramesForwarded = 0;
        udpInputPacketsForwarded = 0;
        udpOutputPacketsReceived = 0;
        inputSampleRate = 0;
        if (inputServerEnabled) {
            startInputServer();
        }
        if (udpOutputEnabled) {
            startUdpOutput();
        }
        if (processAutoStart) {
            startProcessIfNeeded();
        }
        emitStatus(QStringLiteral("DSD-neo bridge enabled"));
    } else {
        stopProcess();
        stopUdpOutput();
        stopInputServer();
        emitStatus(QStringLiteral("DSD-neo bridge disabled"));
    }
}

void DsdNeoBridge::submitInputPcm(const QByteArray &pcmData, int sampleRate) {
    if (!inputSubmissionEnabled.load() || pcmData.isEmpty()) return;
    bool schedule = false;
    {
        std::lock_guard<std::mutex> lock(inputQueueMutex);
        if (!inputSubmissionEnabled.load()) return;
        if (inputQueue.size() >= 12) {
            inputQueue.pop_front();
            ++inputQueueDrops;
        }
        inputQueue.emplace_back(pcmData, sampleRate);
        if (!inputDrainScheduled) {
            inputDrainScheduled = true;
            schedule = true;
        }
    }
    if (schedule) QMetaObject::invokeMethod(this, [this]() { drainInputPcm(); }, Qt::QueuedConnection);
}

void DsdNeoBridge::drainInputPcm() {
    for (int count = 0; count < 6; ++count) {
        std::pair<QByteArray, int> frame;
        quint64 drops = 0;
        {
            std::lock_guard<std::mutex> lock(inputQueueMutex);
            if (inputQueue.empty()) {
                inputDrainScheduled = false;
                return;
            }
            frame = std::move(inputQueue.front());
            inputQueue.pop_front();
            drops = inputQueueDrops;
            inputQueueDrops = 0;
        }
        if (drops) {
            if (processAutoStart) {
                // A missing PCM interval invalidates symbol and cipher alignment.
                emitStatus(QStringLiteral("DSD-neo PCM discontinuity: %1 frames; restarting decoder").arg(drops));
                stopProcess();
            }
            if (fobosVerboseLoggingEnabled())
                qWarning() << "[DSD-neo transport] dropped stale PCM frames" << drops;
        }
        sendInputPcm(frame.first, frame.second);
    }
    QMetaObject::invokeMethod(this, [this]() { drainInputPcm(); }, Qt::QueuedConnection);
}

void DsdNeoBridge::sendInputPcm(const QByteArray &pcmData, int sampleRate) {
    if (!bridgeEnabled || !inputServerEnabled || pcmData.size() < int(sizeof(qint16))) {
        return;
    }
    if (processAutoStart) {
        startProcessIfNeeded();
    }
    if (processAutoStart &&
        (processStoppingIntentionally || managedProcess->state() != QProcess::Running)) {
        return;
    }

    if (inputSampleRate != sampleRate) {
        inputSampleRate = sampleRate;
        inputWarmupFramesRemaining = usesProcessInput() ? 0 : DSD_INPUT_WARMUP_FRAMES;
        emitStatus(QStringLiteral("DSD-neo input stream: %1 Hz PCM16LE").arg(inputSampleRate));
    }
    if (inputWarmupFramesRemaining > 0) {
        --inputWarmupFramesRemaining;
        return;
    }

    const QByteArray normalizedPcm = prepareInputPcmForDsd(pcmData);
    const int evenSize = normalizedPcm.size() & ~1;
    if (usesProcessInput()) {
        const int rateIndex = processArguments.indexOf(QStringLiteral("-s"));
        if (rateIndex >= 0 && rateIndex + 1 < processArguments.size() &&
            processArguments.at(rateIndex + 1).toInt() != sampleRate) {
            return;
        }
        const qint64 limit = (std::max<qint64>)(4096, qint64(sampleRate) * 2 * MAX_STDIN_BACKLOG_MS / 1000);
        if (managedProcess->bytesToWrite() + evenSize > limit) {
            emitStatus(QStringLiteral("DSD-neo input stalled; restarting to discard stale PCM"));
            stopProcess();
            return;
        }
        if (managedProcess->write(normalizedPcm.constData(), evenSize) != evenSize) {
            emitStatus(QStringLiteral("DSD-neo stdin write failed; restarting decoder"));
            stopProcess();
            return;
        }
        ++pcmFramesForwarded;
        return;
    }
    const char *data = normalizedPcm.constData();
    for (int offset = 0; offset < evenSize; offset += int(MAX_UDP_INPUT_DATAGRAM_BYTES)) {
        int chunk = (std::min)(int(MAX_UDP_INPUT_DATAGRAM_BYTES), evenSize - offset);
        chunk &= ~1;
        if (chunk <= 0) {
            continue;
        }
        const qint64 sent = udpInputSocket->writeDatagram(data + offset,
                                      chunk,
                                      QHostAddress::LocalHost,
                                      inputPort);
        if (sent == chunk) ++udpInputPacketsForwarded;
        else if (fobosVerboseLoggingEnabled())
            qWarning() << "[DSD-neo transport] UDP write failed" << udpInputSocket->errorString();
    }
    ++pcmFramesForwarded;
    if (pcmFramesForwarded <= 8 || (pcmFramesForwarded % 200) == 0) {
        emitStatus(QStringLiteral("DSD-neo input: frame %1, UDP packets %2")
                       .arg(pcmFramesForwarded)
                       .arg(udpInputPacketsForwarded));
    }
}

void DsdNeoBridge::startInputServer() {
    if (!inputServerEnabled || !bridgeEnabled) {
        return;
    }
    if (inputServer->isListening()) {
        inputServer->close();
    }
    emitStatus(usesProcessInput() ? QStringLiteral("DSD-neo PCM input: process stdin")
                                : QStringLiteral("DSD-neo UDP input sender ready: 127.0.0.1:%1").arg(inputPort));
}

void DsdNeoBridge::stopInputServer() {
    for (QTcpSocket *client : std::as_const(inputClients)) {
        if (!client) {
            continue;
        }
        client->disconnect(this);
        client->disconnectFromHost();
        client->deleteLater();
    }
    inputClients.clear();
    if (inputServer->isListening()) {
        inputServer->close();
    }
}

void DsdNeoBridge::startUdpOutput() {
    if (!udpOutputEnabled || !bridgeEnabled) {
        return;
    }
    if (udpOutputSocket->state() == QAbstractSocket::BoundState &&
        udpOutputSocket->localPort() == udpOutputPort) {
        return;
    }
    stopUdpOutput();
    const bool bound = udpOutputSocket->bind(QHostAddress::LocalHost,
                                             udpOutputPort,
                                             QUdpSocket::ShareAddress |
                                                 QUdpSocket::ReuseAddressHint);
    if (!bound) {
        emitStatus(QStringLiteral("DSD-neo UDP audio bind failed on 127.0.0.1:%1: %2")
                       .arg(udpOutputPort)
                       .arg(udpOutputSocket->errorString()));
        return;
    }
    emitStatus(QStringLiteral("DSD-neo UDP audio input ready: 127.0.0.1:%1").arg(udpOutputPort));
}

void DsdNeoBridge::stopUdpOutput() {
    previousOutputSampleValid = false;
    if (udpOutputSocket->state() != QAbstractSocket::UnconnectedState) {
        udpOutputSocket->close();
    }
}

void DsdNeoBridge::startProcessIfNeeded() {
    if (!processAutoStart || !bridgeEnabled || processProgram.trimmed().isEmpty() ||
        processStoppingIntentionally || processStartTimer->isActive()) {
        return;
    }
    if (managedProcess->state() != QProcess::NotRunning) {
        return;
    }
    if (processRestartBackoffActive()) {
        // Coarse timers can fire early; keep recovery alive even without new PCM.
        const qint64 remaining = processRestartBackoffMs - processRestartBackoffTimer.elapsed();
        processStartTimer->start(static_cast<int>((std::max<qint64>)(1, remaining)));
        return;
    }
    processLogLines = 0;
    processStdoutPending.clear();
    processStderrPending.clear();
    previousOutputSampleValid = false;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("DSD_NEO_NO_BOOTSTRAP"), QStringLiteral("1"));
    environment.insert(QStringLiteral("FOBOS_DMR_PRIVACY_MODE"), privacyMode);
    environment.insert(QStringLiteral("FOBOS_DMR_PRIVACY_KEY_ID"), QString::number(privacyKeyId));
    environment.insert(QStringLiteral("FOBOS_DMR_PRIVACY_KEY_HEX"), privacyKeyHex);
    environment.insert(QStringLiteral("FOBOS_DMR_PRIVACY_FORWARD"), privacyForwardToBackend ? QStringLiteral("1") : QStringLiteral("0"));
    const PrivacyArguments arguments =
        privacyProcessArguments(processArguments,
                                 privacyMode,
                                 privacyKeyHex,
                                 privacyForwardToBackend);
    processPrivacyKeyHex = privacyKeyHex;
    managedProcess->setProcessEnvironment(environment);
    managedProcess->setProgram(processProgram);
    managedProcess->setArguments(arguments.actual);
    if (!processWorkingDirectory.trimmed().isEmpty()) {
        managedProcess->setWorkingDirectory(processWorkingDirectory);
    }
    managedProcess->start();
    inputWarmupFramesRemaining = usesProcessInput() ? 0 : DSD_INPUT_WARMUP_FRAMES;
    if (!arguments.privacyError.isEmpty()) {
        emitStatus(QStringLiteral("DSD-neo privacy configuration error: %1")
                       .arg(arguments.privacyError));
    } else if (arguments.privacyMode != QLatin1String("none")) {
        emitStatus(QStringLiteral("DSD-neo privacy CLI enabled: %1, keyId %2")
                       .arg(arguments.privacyMode.toUpper())
                       .arg(privacyKeyId));
    }
    emitStatus(QStringLiteral("Starting DSD-neo: %1 %2")
                   .arg(processProgram, arguments.display.join(QLatin1Char(' '))));
}
bool DsdNeoBridge::usesProcessInput() const {
    const int index = processArguments.indexOf(QStringLiteral("-i"));
    return processAutoStart && index >= 0 && index + 1 < processArguments.size() &&
           processArguments.at(index + 1) == QLatin1String("-");
}

void DsdNeoBridge::stopProcess() {
    processStartTimer->stop();
    if (!managedProcess || managedProcess->state() == QProcess::NotRunning) {
        return;
    }
    if (processStoppingIntentionally) return;
    processStoppingIntentionally = true;
    managedProcess->terminate();
    processStopTimer->start(PROCESS_STOP_GRACE_MS);
}

void DsdNeoBridge::emitStatus(const QString &status) {
    if (fobosVerboseLoggingEnabled()) {
        qDebug() << "[DSD-neo]" << status;
    }
    emit statusChanged(status);
}

bool DsdNeoBridge::processRestartBackoffActive() const {
    return processRestartBackoffMs > 0 &&
           processRestartBackoffTimer.isValid() &&
           processRestartBackoffTimer.elapsed() < processRestartBackoffMs;
}

void DsdNeoBridge::armProcessRestartBackoff(const QString &reason) {
    if (processRunTimer.isValid() && processRunTimer.elapsed() >= PROCESS_STABLE_RUN_MS) {
        processRestartBackoffMs = 0;
    }
    processRunTimer.invalidate();
    processRestartBackoffMs = processRestartBackoffMs <= 0
                                  ? qint64(1000)
                                  : (std::min)(processRestartBackoffMs * 2, PROCESS_RESTART_BACKOFF_MAX_MS);
    processRestartBackoffTimer.restart();
    emitStatus(QStringLiteral("DSD-neo restart backoff %1 ms: %2")
                   .arg(processRestartBackoffMs)
                   .arg(reason.left(120)));
}

void DsdNeoBridge::acceptInputClient() {
    while (inputServer->hasPendingConnections()) {
        QTcpSocket *client = inputServer->nextPendingConnection();
        if (!client) {
            continue;
        }
        client->setParent(this);
        inputClients.append(client);
        connect(client, &QTcpSocket::disconnected, this, [this, client]() {
            removeInputClient(client);
        });
        connect(client,
                QOverload<QAbstractSocket::SocketError>::of(&QTcpSocket::errorOccurred),
                this,
                [this, client](QAbstractSocket::SocketError) {
                    emitStatus(QStringLiteral("DSD-neo TCP client error: %1")
                                   .arg(client ? client->errorString() : QString()));
                });
        emitStatus(QStringLiteral("DSD-neo TCP client connected: %1")
                       .arg(client->peerAddress().toString()));
    }
}

void DsdNeoBridge::removeInputClient(QTcpSocket *client) {
    inputClients.removeAll(client);
    if (client) {
        client->deleteLater();
    }
    emitStatus(QStringLiteral("DSD-neo TCP client disconnected"));
}

void DsdNeoBridge::readUdpOutput() {
    while (udpOutputSocket->hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(static_cast<int>(udpOutputSocket->pendingDatagramSize()));
        udpOutputSocket->readDatagram(datagram.data(), datagram.size());
        if (processAutoStart &&
            (processStoppingIntentionally || managedProcess->state() != QProcess::Running)) {
            continue;
        }
        const QByteArray mono8k = pcm16ToMono(datagram, udpOutputChannels);
        if (mono8k.isEmpty()) {
            continue;
        }
        ++udpOutputPacketsReceived;
        if (udpOutputPacketsReceived <= 8 || (udpOutputPacketsReceived % 100) == 0) {
            emitStatus(QStringLiteral("DSD-neo audio: packet %1, %2 bytes")
                           .arg(udpOutputPacketsReceived)
                           .arg(mono8k.size()));
        }
        if (fobosVerboseLoggingEnabled() &&
            (udpOutputPacketsReceived <= 8 || (udpOutputPacketsReceived % 100) == 0)) {
            qDebug() << "[DSD-neo] decoded UDP audio"
                     << "packet" << udpOutputPacketsReceived
                     << "bytes" << datagram.size()
                     << "monoBytes" << mono8k.size();
        }
        emit decodedPcmReady(upsample8kMonoTo48k(mono8k));
    }
}

void DsdNeoBridge::readProcessOutput() {
    processStdoutPending += managedProcess->readAllStandardOutput();
    processStderrPending += managedProcess->readAllStandardError();
    for (QByteArray *pending : {&processStdoutPending, &processStderrPending}) {
      int newline;
      while ((newline = pending->indexOf('\n')) >= 0) {
        QString line = QString::fromUtf8(pending->left(newline)).trimmed();
        pending->remove(0, newline + 1);
        // The retiring process may still print its old key after settings change.
        for (const QString &key : {privacyKeyHex, processPrivacyKeyHex}) {
            if (!key.isEmpty())
                line.replace(key, QStringLiteral("<redacted>"), Qt::CaseInsensitive);
            if (key.size() == 32 || key.size() == 64) {
                for (int word = 0; word < key.size(); word += 16)
                    line.replace(key.mid(word, 16), QStringLiteral("<redacted>"),
                                 Qt::CaseInsensitive);
            }
        }
        static const QRegularExpression keyDump(
            QStringLiteral("\\b(?:key|k[1-4])\\s*(?:[1-4]\\s*)?[:=]|"
                           "\\b(?:arc4|rc4|aes(?:[- ]?(?:128|256))?)\\s+key\\b"),
            QRegularExpression::CaseInsensitiveOption);
        if (keyDump.match(line).hasMatch())
            line = QStringLiteral("DSD-neo key material: <redacted>");
        if (line.isEmpty()) {
            continue;
        }
        ++processLogLines;
        if (processLogLines <= 20 || (processLogLines % 50) == 0) {
            emitStatus(QStringLiteral("DSD-neo: %1").arg(line.left(160)));
        } else if (fobosVerboseLoggingEnabled()) {
            qDebug() << "[DSD-neo process]" << line;
        }
      }
      if (pending->size() > 16384) pending->clear();
    }
}

QByteArray DsdNeoBridge::prepareInputPcmForDsd(const QByteArray &pcmData) {
    const int samples = pcmData.size() / int(sizeof(qint16));
    if (samples <= 0) {
        return {};
    }

    // This is a discriminator waveform, not speech: frame-wise AGC moves
    // the four symbol levels whenever an unrelated noise peak enters a frame.
    if (!fobosVerboseLoggingEnabled() ||
        (pcmFramesForwarded >= 8 && (pcmFramesForwarded % 200) != 0)) {
        return pcmData.left(samples * int(sizeof(qint16)));
    }

    const char *raw = pcmData.constData();
    int peak = 0;
    int clipped = 0;
    double squareSum = 0.0;
    for (int i = 0; i < samples; ++i) {
        const int sample = readLe16(raw + i * int(sizeof(qint16)));
        const int absSample = std::abs(sample);
        peak = (std::max)(peak, absSample);
        if (absSample >= 32760) {
            ++clipped;
        }
        const double normalized = static_cast<double>(sample) / 32768.0;
        squareSum += normalized * normalized;
    }

    const double peakNorm = static_cast<double>(peak) / 32767.0;
    const double rms = std::sqrt(squareSum / static_cast<double>((std::max)(1, samples)));

    if (fobosVerboseLoggingEnabled() &&
        (pcmFramesForwarded < 8 || (pcmFramesForwarded % 200) == 0)) {
        qDebug() << "[DSD-neo] input PCM"
                 << "frame" << pcmFramesForwarded
                 << "samples" << samples
                 << "peak" << QString::number(peakNorm, 'f', 3)
                 << "rms" << QString::number(rms, 'f', 3)
                 << "scale" << "1.000"
                 << "clipped" << clipped;
    }

    return pcmData.left(samples * int(sizeof(qint16)));
}

QByteArray DsdNeoBridge::pcm16ToMono(const QByteArray &pcmData, int channels) const {
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

QByteArray DsdNeoBridge::upsample8kMonoTo48k(const QByteArray &pcmData) {
    const int inputSamples = pcmData.size() / int(sizeof(qint16));
    if (inputSamples <= 0) {
        return {};
    }

    QByteArray output;
    output.reserve(inputSamples * UPSAMPLE_FACTOR * int(sizeof(qint16)));
    const char *raw = pcmData.constData();
    for (int i = 0; i < inputSamples; ++i) {
        const qint16 current = readLe16(raw + i * int(sizeof(qint16)));
        if (!previousOutputSampleValid) {
            previousOutputSample = current;
            previousOutputSampleValid = true;
        }
        for (int step = 1; step <= UPSAMPLE_FACTOR; ++step) {
            const int mixed =
                (static_cast<int>(previousOutputSample) * (UPSAMPLE_FACTOR - step) +
                 static_cast<int>(current) * step) /
                UPSAMPLE_FACTOR;
            appendLe16(output, static_cast<qint16>(std::clamp(mixed, -32768, 32767)));
        }
        previousOutputSample = current;
    }
    return output;
}

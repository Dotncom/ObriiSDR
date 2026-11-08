#include "gnsssdrstream.h"
#include "gnsssdrdecimator.h"
#include "iqbuffer.h"
#include "playbackmanager.h"
#include <QFileInfo>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QElapsedTimer>
#include <QFile>
#include <QSysInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUuid>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstring>
#include <memory>
#include <vector>

namespace GnssSdr {
LiveSource::LiveSource(double sampleRateHz, double centerHz) : rate(sampleRateHz), center(centerHz) {}
bool LiveSource::failLocked(const QString &message) {
    if (failure.isEmpty()) failure = message;
    changed.notify_all();
    return false;
}
void LiveSource::setReady() {
    std::lock_guard<std::mutex> lock(mutex);
    if (!cancelled && failure.isEmpty() && !eof) ready = true;
    changed.notify_all();
}
bool LiveSource::isReady() const {
    std::lock_guard<std::mutex> lock(mutex);
    return ready && !eof && !cancelled && failure.isEmpty();
}
bool LiveSource::submit(const QByteArray &cf32, double sampleRateHz, double centerHz) {
    std::lock_guard<std::mutex> lock(mutex);
    if (cancelled || !failure.isEmpty()) return false;
    if (!ready || eof) return failLocked(QStringLiteral("Live IQ submitted before readiness or after EOF."));
    if (!std::isfinite(sampleRateHz) || !std::isfinite(centerHz) ||
        sampleRateHz != rate || centerHz != center)
        return failLocked(QStringLiteral("Live IQ source rate or center changed; start a new session."));
    if (cf32.isEmpty() || cf32.size() % 8 || cf32.size() > MaximumBlockBytes)
        return failLocked(QStringLiteral("Live IQ must contain 1..131072 full CF32 complex samples."));
    if (queue.size() >= MaximumQueuedBlocks || cf32.size() > MaximumQueuedBytes - queuedBytes)
        return failLocked(QStringLiteral("Live IQ producer queue overrun; no samples were silently dropped."));
    queue.push_back(cf32);
    queuedBytes += cf32.size();
    accepted += static_cast<quint64>(cf32.size() / 8);
    changed.notify_one();
    return true;
}
bool LiveSource::finish(quint64 finalSourceSamples) {
    std::lock_guard<std::mutex> lock(mutex);
    if (cancelled || !failure.isEmpty()) return false;
    if (!ready || eof || accepted != finalSourceSamples)
        return failLocked(QStringLiteral("Invalid live EOF state or final source sample count."));
    eof = true;
    changed.notify_all();
    return true;
}
void LiveSource::cancel() {
    std::lock_guard<std::mutex> lock(mutex);
    cancelled = true;
    queue.clear();
    queuedBytes = 0;
    changed.notify_all();
}
void LiveSource::fail(const QString &message) {
    std::lock_guard<std::mutex> lock(mutex);
    failLocked(message);
}
LiveSource::Read LiveSource::read(QByteArray &cf32) {
    std::unique_lock<std::mutex> lock(mutex);
    changed.wait_for(lock, std::chrono::milliseconds(20), [this] {
        return cancelled || !failure.isEmpty() || !queue.empty() || eof;
    });
    if (cancelled) return Read::Cancelled;
    if (!failure.isEmpty()) return Read::Error;
    if (queue.empty()) return eof ? Read::End : Read::Wait;
    cf32 = std::move(queue.front());
    queuedBytes -= cf32.size();
    queue.pop_front();
    return Read::Data;
}
QString LiveSource::error() const { std::lock_guard<std::mutex> lock(mutex); return failure; }
quint64 LiveSource::acceptedSamples() const { std::lock_guard<std::mutex> lock(mutex); return accepted; }
bool LiveSource::atEof() const { std::lock_guard<std::mutex> lock(mutex); return eof; }

bool readInputMetadata(Input &in, QString *error) {
    if (error) error->clear();
    const QFileInfo info(in.path);
    if (!info.isFile()) { if (error) *error = QStringLiteral("IQ file does not exist."); return false; }
    in.dataOffset = 0; in.dataBytes = info.size();
    for (const QString &sidecar : {in.path + QStringLiteral(".json"), info.dir().filePath(info.completeBaseName() + QStringLiteral(".json"))}) {
        QFile file(sidecar);
        if (!file.open(QIODevice::ReadOnly) || file.size() > 262144) continue;
        const QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
        const double rate = object.value("sampleRate").toDouble(object.value("sampleRateHz").toDouble());
        const double center = object.value("centerFrequency").toDouble(object.value("centerFrequencyHz").toDouble());
        if (rate > 0) in.sampleRateHz = rate;
        if (center > 0) in.centerHz = center;
        break;
    }
    if (info.suffix().compare(QStringLiteral("wav"), Qt::CaseInsensitive) == 0) {
        PlaybackManager::WavInfo wave;
        if (!PlaybackManager::readWavInfo(in.path, wave, error) || wave.channels != 2) {
            if (error && error->isEmpty()) *error = QStringLiteral("GNSS needs stereo PCM16 IQ, not mono audio."); return false;
        }
        in.format = QStringLiteral("cs16"); in.sampleRateHz = wave.sampleRate;
        in.dataOffset = wave.dataOffset; in.dataBytes = wave.dataSize;
        if (wave.hasRadioSettings) in.centerHz = wave.radioSettings.centerFrequency;
    }
    const int bytes = in.format == QStringLiteral("cf32") ? 8 : in.format == QStringLiteral("cs16") ? 4 : 2;
    if (in.dataBytes % bytes || in.dataOffset + in.dataBytes > static_cast<quint64>(info.size())) {
        if (error) *error = QStringLiteral("IQ file contains an incomplete sample."); return false;
    }
    return true;
}
QByteArray floatToCs16(const float *samples, std::size_t count, float gain) {
    QByteArray result(static_cast<int>(count * 2), Qt::Uninitialized);
    for (std::size_t i = 0; i < count; ++i) {
        const float value = std::isfinite(samples[i]) ? samples[i] : 0.0f;
        const auto quantized = static_cast<qint16>(std::lround(
            (std::clamp)(static_cast<double>(value) * gain, -1.0, 1.0) * 32767.0));
        qToLittleEndian<qint16>(quantized, result.data() + i * 2);
    }
    return result;
}

QStringList trackingArguments(const Input &in, const QStringList &systems,
                              const QString &iqSource, quint16 nmeaPort, QString *error) {
    if (error) error->clear();
    QStringList result;
    const double outputRateHz = in.sampleRateHz / in.decimation;
    const auto covered = [&in, outputRateHz](double frequency, double halfWidth) {
        return outputRateHz > 0 && std::abs(frequency - in.centerHz) + halfWidth <= outputRateHz / 2;
    };
    const auto add = [&result](const QString &signal, const QString &prns) {
        result << QStringLiteral("-sig") << signal << QStringLiteral("-prn") << prns;
    };
    QStringList missing;
    for (const QString &system : systems) {
        if (system == QStringLiteral("GLONASS")) {
            QStringList channels;
            for (int k = -7; k <= 6; ++k)
                if (covered(1602e6 + k * 562500.0, 550000)) channels << QString::number(k);
            if (channels.isEmpty()) missing << system;
            else add(QStringLiteral("G1CA"), channels.join(','));
        } else {
            const double frequency = system == QStringLiteral("BeiDou") ? 1561.098e6 : 1575.42e6;
            const double halfWidth = system == QStringLiteral("Galileo") ? 2.046e6 : 1.023e6;
            if (!covered(frequency, halfWidth)) { missing << system; continue; }
            if (system == QStringLiteral("Galileo")) add(QStringLiteral("E1B"), QStringLiteral("1-36"));
            else if (system == QStringLiteral("BeiDou")) add(QStringLiteral("B1I"), QStringLiteral("1-63"));
            else if (system == QStringLiteral("QZSS")) add(QStringLiteral("L1CA"), QStringLiteral("193-202"));
            else if (system == QStringLiteral("SBAS")) add(QStringLiteral("L1CA"), QStringLiteral("120-158"));
            else if (system == QStringLiteral("GPS")) add(QStringLiteral("L1CA"), QStringLiteral("1-32"));
        }
    }
    if (in.decimation < 1 || in.decimation > 256 || (in.decimation & (in.decimation - 1)) ||
        !std::isfinite(in.sampleRateHz) || !std::isfinite(outputRateHz) ||
        outputRateHz < 2.046e6 || outputRateHz > 32e6) {
        if (error) *error = QStringLiteral("GNSS: PocketSDR needs 2.046..32 MS/s IQ (source %1 MS/s, after decimation %2 MS/s).")
                               .arg(in.sampleRateHz / 1e6, 0, 'g', 8)
                               .arg(outputRateHz / 1e6, 0, 'g', 8);
        return {};
    }
    if (!std::isfinite(in.centerHz) || in.centerHz <= 0 || iqSource.isEmpty() || !nmeaPort || result.isEmpty() || !missing.isEmpty()) {
        if (error) *error = QStringLiteral("GNSS: %1 outside the IQ band (RX center %2 MHz, IQ rate %3 MS/s). For GPS L1, tune the receiver center to 1575.420 MHz.")
                               .arg(missing.isEmpty() ? QStringLiteral("No selected signal") : missing.join(QStringLiteral(", ")))
                               .arg(in.centerHz / 1e6, 0, 'f', 6)
                               .arg(outputRateHz / 1e6, 0, 'g', 8);
        return {};
    }
    result << QStringLiteral("-fmt") << QStringLiteral("CS16")
           << QStringLiteral("-f") << QString::number(outputRateHz / 1e6, 'g', 12)
           << QStringLiteral("-fo") << QString::number(in.centerHz / 1e6, 'g', 12)
           << QStringLiteral("-ti") << QStringLiteral("0")
           << QStringLiteral("-nmea") << QStringLiteral("127.0.0.1:%1").arg(nmeaPort)
           << iqSource;
    return result;
}

QStringList trackingArguments(const Input &in, const QStringList &systems,
                              quint16 iqPort, quint16 nmeaPort, QString *error) {
    return trackingArguments(in, systems,
                             iqPort ? QStringLiteral("127.0.0.1:%1").arg(iqPort) : QString(),
                             nmeaPort, error);
}

bool configureTrackingRate(Input &in, const QStringList &systems, QString *error) {
    in.decimation = 1;
    QString candidateError;
    bool valid = false;
    int fallbackFactor = 1;
    // Keep the backend near 3-5 MS/s when the chosen signals fit; this also
    // avoids streaming the full receiver bandwidth into PocketSDR.
    for (int factor = 1; factor <= 256; factor *= 2) {
        Input candidate = in;
        candidate.decimation = factor;
        if (factor > 1 && candidate.sampleRateHz / factor < 3e6) break;
        if (candidate.sampleRateHz / factor <= 32e6 && fallbackFactor == 1) fallbackFactor = factor;
        if (trackingArguments(candidate, systems, 1, 2, &candidateError).isEmpty()) continue;
        in.decimation = factor;
        valid = true;
    }
    if (!valid) {
        in.decimation = fallbackFactor;
        trackingArguments(in, systems, 1, 2, error);
        return false;
    }
    if (error) error->clear();
    return true;
}
}

GnssSdrStream::GnssSdrStream(const GnssSdr::Input &value, QObject *parent) : QThread(parent), input(value) {}
GnssSdrStream::GnssSdrStream(const GnssSdr::Input &value, std::shared_ptr<GnssSdr::LiveSource> source,
                           QObject *parent) : QThread(parent), input(value), liveSource(std::move(source)) {}
GnssSdrStream::~GnssSdrStream() { requestStop(); wait(); }
void GnssSdrStream::requestStop() { stopping.store(true); if (liveSource) liveSource->cancel(); }

void GnssSdrStream::run() {
    QTcpServer nmeaServer;
#ifdef Q_OS_WIN
    QLocalServer iqServer;
    const QString pipeName = QStringLiteral("obrii-gnss-%1")
                                 .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    const bool iqListening = iqServer.listen(pipeName);
#else
    QTcpServer iqServer;
    const bool iqListening = iqServer.listen(QHostAddress::LocalHost, 0);
#endif
    if (!iqListening || !nmeaServer.listen(QHostAddress::LocalHost, 0)) {
        emit streamError(QStringLiteral("Cannot open local GNSS streams.")); return;
    }
    const QString iqSource =
#ifdef Q_OS_WIN
        iqServer.fullServerName();
#else
        QStringLiteral("127.0.0.1:%1").arg(iqServer.serverPort());
#endif
    emit listening(iqSource, nmeaServer.serverPort());
    QElapsedTimer timeout; timeout.start();
    while (!stopping && !iqServer.hasPendingConnections() && timeout.elapsed() < 30000)
        iqServer.waitForNewConnection(50);
    if (stopping) return;
    std::unique_ptr<QTcpSocket> nmea;
#ifdef Q_OS_WIN
    std::unique_ptr<QLocalSocket> iq(iqServer.nextPendingConnection());
#else
    std::unique_ptr<QTcpSocket> iq(iqServer.nextPendingConnection());
#endif
    if (!iq) { emit streamError(QStringLiteral("PocketSDR did not connect to the IQ stream.")); return; }
#ifndef Q_OS_WIN
    iq->setSocketOption(QAbstractSocket::LowDelayOption, 1);
#endif
    QFile file(input.path);
    int bytesPerComplex = input.format == QStringLiteral("cf32") ? 8 : input.format == QStringLiteral("cs16") ? 4 : 2;
    if (!input.live && (!file.open(QIODevice::ReadOnly) || !file.seek(input.dataOffset))) {
        emit streamError(QStringLiteral("Cannot read a complete raw IQ file (CF32/CS16/CS8 LE).")); return;
    }
    // Preserve snapshot-based GUI behavior; injected CLI sources have their own cursor/EOF.
    const auto initial = input.live && !liveSource ? IqBuffer::stats() : IqBuffer::Stats{};
    quint64 cursor = initial.totalFloatCount, count = 0;
    quint64 remainingBytes = input.dataBytes;
    float liveGain = 0.0f;
    QByteArray nmeaBuffer;
    std::vector<float> samples;
    std::vector<float> converted;
    GnssSdrDecimator decimator(input.decimation);
    QElapsedTimer performance; performance.start();
    qint64 snapshotNs = 0, filterNs = 0, encodeNs = 0, socketWaitNs = 0;
    quint64 sourceSamplesProcessed = 0;
    QElapsedTimer report; report.start();
    bool sourceEnded = false;
    if (liveSource) { liveSource->setReady(); emit sourceReady(); }
    const auto readNmea = [&] {
        if (!nmea && nmeaServer.hasPendingConnections()) nmea.reset(nmeaServer.nextPendingConnection());
        if (!nmea) { nmeaServer.waitForNewConnection(0); return; }
        nmea->waitForReadyRead(0);
        nmeaBuffer += nmea->readAll();
        if (nmeaBuffer.size() > 65536) nmeaBuffer.clear();
        int end;
        while ((end = nmeaBuffer.indexOf('\n')) >= 0) {
            const QByteArray line = nmeaBuffer.left(end).trimmed(); nmeaBuffer.remove(0, end + 1);
            if (line.startsWith('$') && line.size() <= 1024) emit nmeaLine(QString::fromLatin1(line));
        }
    };
    while (!stopping && iq->isOpen()) {
        readNmea();
        QByteArray data;
        if (liveSource) {
            QByteArray raw;
            const auto state = liveSource->read(raw);
            if (state == GnssSdr::LiveSource::Read::Wait) continue;
            if (state == GnssSdr::LiveSource::Read::Error) { emit streamError(liveSource->error()); return; }
            if (state == GnssSdr::LiveSource::Read::Cancelled) return;
            if (state == GnssSdr::LiveSource::Read::End) { sourceEnded = true; break; }
            samples.resize(static_cast<std::size_t>(raw.size() / 4));
            if (QSysInfo::ByteOrder == QSysInfo::LittleEndian) {
                std::memcpy(samples.data(), raw.constData(), static_cast<std::size_t>(raw.size()));
            } else {
                for (std::size_t i = 0; i < samples.size(); ++i) {
                    const quint32 bits = qFromLittleEndian<quint32>(raw.constData() + i * 4);
                    std::memcpy(&samples[i], &bits, 4);
                }
            }
            sourceSamplesProcessed += samples.size() / 2;
            decimator.process(samples.data(), samples.size(), converted);
            // Full-source CLI IQ is already scaled CF32; do not apply GUI live AGC.
            data = GnssSdr::floatToCs16(converted.data(), converted.size());
        } else if (input.live) {
            const auto current = IqBuffer::stats();
            if (current.epoch != initial.epoch || current.totalFloatCount < cursor ||
                current.skippedSnapshotBlocks != initial.skippedSnapshotBlocks) {
                emit streamError(QStringLiteral("GNSS stopped: source IQ discontinuity. Reduce receiver load or use a recording.")); return;
            }
            if (current.totalFloatCount - cursor > current.snapshotSize) {
                const double lagSeconds = (current.totalFloatCount - cursor) /
                    (2.0 * input.sampleRateHz);
                const double elapsed = (std::max)(performance.elapsed() / 1000.0, 0.001);
                emit streamError(QStringLiteral("GNSS stopped: live IQ fell %1 s behind. Source %2 MS/s, worker %3 MS/s; snapshot %4 s, filter %5 s, encode %6 s, socket wait %7 s over %8 s; TCP queued %9 KiB. See which stage is limiting throughput.")
                                     .arg(lagSeconds, 0, 'f', 2)
                                     .arg(input.sampleRateHz / 1e6, 0, 'g', 8)
                                     .arg(sourceSamplesProcessed / elapsed / 1e6, 0, 'f', 2)
                                     .arg(snapshotNs / 1e9, 0, 'f', 2)
                                     .arg(filterNs / 1e9, 0, 'f', 2)
                                     .arg(encodeNs / 1e9, 0, 'f', 2)
                                     .arg(socketWaitNs / 1e9, 0, 'f', 2)
                                     .arg(elapsed, 0, 'f', 2)
                                     .arg(iq->bytesToWrite() / 1024)); return;
            }
            const quint64 available = (current.totalFloatCount - cursor) & ~quint64(1);
            if (!available) { msleep(2); continue; }
            const std::size_t take = static_cast<std::size_t>((std::min)(available, quint64(524288)));
            IqBuffer::BlockMetadata metadata;
            QElapsedTimer stage; stage.start();
            if (!IqBuffer::snapshotRecentEndingAt(samples, take, cursor + take, nullptr, &metadata)) {
                msleep(1); continue;
            }
            snapshotNs += stage.nsecsElapsed();
            if (samples.size() != take || (metadata.valid && (metadata.tuning ||
                std::abs(metadata.centerFrequencyHz - input.centerHz) > 1))) {
                emit streamError(QStringLiteral("GNSS stopped: receiver tuning changed.")); return;
            }
            cursor += take;
            sourceSamplesProcessed += take / 2;
            const float *outputSamples = samples.data();
            std::size_t outputCount = samples.size();
            if (input.decimation != 1) {
                stage.restart();
                decimator.process(samples.data(), samples.size(), converted);
                filterNs += stage.nsecsElapsed();
                outputSamples = converted.data();
                outputCount = converted.size();
            }
            stage.restart();
            double power = 0.0;
            std::size_t measured = 0;
            const std::size_t stride = (std::max)(std::size_t(2), (outputCount / 8192) & ~std::size_t(1));
            for (std::size_t i = 0; i < outputCount; i += stride) {
                const float value = std::isfinite(outputSamples[i]) ? outputSamples[i] : 0.0f;
                power += static_cast<double>(value) * value;
                ++measured;
            }
            const float rms = measured ? static_cast<float>(std::sqrt(power / measured)) : 0.0f;
            const float desired = (std::clamp)(0.03f / (std::max)(rms, 1e-7f), 1.0f, 1024.0f);
            liveGain = liveGain == 0.0f ? desired : liveGain + 0.05f * (desired - liveGain);
            data = GnssSdr::floatToCs16(outputSamples, outputCount, liveGain);
            encodeNs += stage.nsecsElapsed();
        } else {
            const QByteArray raw = file.read(static_cast<qint64>((std::min)(remainingBytes, quint64(65536 * bytesPerComplex))));
            if (raw.isEmpty()) break;
            remainingBytes -= raw.size();
            if (input.format == QStringLiteral("cs16") && input.decimation == 1) data = raw;
            else {
                const int n = raw.size() / (bytesPerComplex / 2);
                samples.resize(static_cast<std::size_t>(n));
                for (int i = 0; i < n; ++i) {
                    if (input.format == QStringLiteral("cs8")) samples[i] = static_cast<qint8>(raw[i]) / 128.0f;
                    else if (input.format == QStringLiteral("cs16")) samples[i] = qFromLittleEndian<qint16>(raw.constData() + i * 2) / 32768.0f;
                    else {
                        const quint32 bits = qFromLittleEndian<quint32>(raw.constData() + i * 4);
                        std::memcpy(&samples[i], &bits, 4);
                    }
                }
                if (input.decimation != 1) {
                    decimator.process(samples.data(), samples.size(), converted);
                    data = GnssSdr::floatToCs16(converted.data(), converted.size());
                } else {
                    data = GnssSdr::floatToCs16(samples.data(), samples.size());
                }
            }
        }
        if (iq->write(data) != data.size()) { emit streamError(iq->errorString()); return; }
        QElapsedTimer socketWait; socketWait.start();
        while (!stopping && iq->bytesToWrite() > 4 * 1024 * 1024) {
            readNmea();
            if (!iq->waitForBytesWritten(2000)) { emit streamError(QStringLiteral("PocketSDR IQ stream stalled.")); return; }
        }
        socketWaitNs += socketWait.nsecsElapsed();
        iq->flush(); count += static_cast<quint64>(data.size() / 4);
        if (report.elapsed() >= 1000) { emit samplesSent(count); report.restart(); }
    }
    if (liveSource && !stopping && !sourceEnded) {
        emit streamError(QStringLiteral("PocketSDR closed before live IQ EOF.")); return;
    }
    while (!stopping && iq->bytesToWrite() > 0) {
        if (!iq->waitForBytesWritten(1000)) {
            if (liveSource) { emit streamError(QStringLiteral("PocketSDR IQ EOF drain failed.")); return; }
            break;
        }
        readNmea();
    }
    if (liveSource && !stopping) emit sourceDrained(sourceSamplesProcessed, count);
#ifdef Q_OS_WIN
    iq->disconnectFromServer();
#else
    iq->disconnectFromHost();
#endif
    // Navigation/PVT outputs may arrive after the last input sample.
    timeout.restart();
    while (!stopping && timeout.elapsed() < 3000) { readNmea(); msleep(10); }
    emit samplesSent(count);
}

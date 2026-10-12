#include "gnsssdrstream.h"
#include "iqbuffer.h"
#include "playbackmanager.h"
#include <QFileInfo>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QElapsedTimer>
#include <QFile>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

namespace GnssSdr {
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
                              quint16 iqPort, quint16 nmeaPort, QString *error) {
    if (error) error->clear();
    QStringList result;
    const auto covered = [&in](double frequency, double halfWidth) {
        return in.sampleRateHz > 0 && std::abs(frequency - in.centerHz) + halfWidth <= in.sampleRateHz / 2;
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
    if (!std::isfinite(in.sampleRateHz) || in.sampleRateHz < 2.046e6 || in.sampleRateHz > 32e6 ||
        !std::isfinite(in.centerHz) || !iqPort || !nmeaPort || result.isEmpty() || !missing.isEmpty()) {
        if (error) *error = QStringLiteral("GNSS: Fs must be 2.046..32 MS/s; selected signals must fit the IQ band. Outside band: %1")
                               .arg(missing.join(QStringLiteral(", ")));
        return {};
    }
    result << QStringLiteral("-fmt") << QStringLiteral("CS16")
           << QStringLiteral("-f") << QString::number(in.sampleRateHz / 1e6, 'g', 12)
           << QStringLiteral("-fo") << QString::number(in.centerHz / 1e6, 'g', 12)
           << QStringLiteral("-ti") << QStringLiteral("0")
           << QStringLiteral("-nmea") << QStringLiteral("127.0.0.1:%1").arg(nmeaPort)
           << QStringLiteral("127.0.0.1:%1").arg(iqPort);
    return result;
}
}

GnssSdrStream::GnssSdrStream(const GnssSdr::Input &value, QObject *parent) : QThread(parent), input(value) {}
GnssSdrStream::~GnssSdrStream() { requestStop(); wait(); }
void GnssSdrStream::requestStop() { stopping.store(true); }

void GnssSdrStream::run() {
    QTcpServer iqServer, nmeaServer;
    if (!iqServer.listen(QHostAddress::LocalHost, 0) || !nmeaServer.listen(QHostAddress::LocalHost, 0)) {
        emit streamError(QStringLiteral("Cannot open local GNSS streams.")); return;
    }
    emit listening(iqServer.serverPort(), nmeaServer.serverPort());
    QElapsedTimer timeout; timeout.start();
    while (!stopping && !iqServer.hasPendingConnections() && timeout.elapsed() < 30000)
        iqServer.waitForNewConnection(50);
    if (stopping) return;
    std::unique_ptr<QTcpSocket> iq(iqServer.nextPendingConnection()), nmea;
    if (!iq) { emit streamError(QStringLiteral("PocketSDR did not connect to the IQ stream.")); return; }
    iq->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    QFile file(input.path);
    int bytesPerComplex = input.format == QStringLiteral("cf32") ? 8 : input.format == QStringLiteral("cs16") ? 4 : 2;
    if (!input.live && (!file.open(QIODevice::ReadOnly) || !file.seek(input.dataOffset))) {
        emit streamError(QStringLiteral("Cannot read a complete raw IQ file (CF32/CS16/CS8 LE).")); return;
    }
    auto initial = IqBuffer::stats();
    quint64 cursor = initial.totalFloatCount, count = 0;
    quint64 remainingBytes = input.dataBytes;
    float liveGain = 0.0f;
    QByteArray nmeaBuffer;
    std::vector<float> samples;
    QElapsedTimer report; report.start();
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
    while (!stopping && iq->state() == QAbstractSocket::ConnectedState) {
        readNmea();
        QByteArray data;
        if (input.live) {
            const auto current = IqBuffer::stats();
            if (current.epoch != initial.epoch || current.totalFloatCount < cursor ||
                current.skippedSnapshotBlocks != initial.skippedSnapshotBlocks) {
                emit streamError(QStringLiteral("GNSS stopped: source IQ discontinuity. Reduce receiver load or use a recording.")); return;
            }
            if (current.totalFloatCount - cursor > current.snapshotSize) {
                const double lagSeconds = (current.totalFloatCount - cursor) /
                    (2.0 * input.sampleRateHz);
                emit streamError(QStringLiteral("GNSS stopped: PocketSDR fell %1 s behind live IQ. Try a lower receiver sample rate (for GPS, center 1575.42 MHz), or use a recording.")
                                     .arg(lagSeconds, 0, 'f', 2)); return;
            }
            const quint64 available = (current.totalFloatCount - cursor) & ~quint64(1);
            if (!available) { msleep(2); continue; }
            const std::size_t take = static_cast<std::size_t>((std::min)(available, quint64(524288)));
            IqBuffer::BlockMetadata metadata;
            if (!IqBuffer::snapshotRecentEndingAt(samples, take, cursor + take, nullptr, &metadata)) {
                msleep(1); continue;
            }
            if (samples.size() != take || (metadata.valid && (metadata.tuning ||
                std::abs(metadata.centerFrequencyHz - input.centerHz) > 1))) {
                emit streamError(QStringLiteral("GNSS stopped: receiver tuning changed.")); return;
            }
            cursor += take;
            double power = 0.0;
            std::size_t measured = 0;
            const std::size_t stride = (std::max)(std::size_t(2), (samples.size() / 8192) & ~std::size_t(1));
            for (std::size_t i = 0; i < samples.size(); i += stride) {
                const float value = std::isfinite(samples[i]) ? samples[i] : 0.0f;
                power += static_cast<double>(value) * value;
                ++measured;
            }
            const float rms = measured ? static_cast<float>(std::sqrt(power / measured)) : 0.0f;
            const float desired = (std::clamp)(0.03f / (std::max)(rms, 1e-7f), 1.0f, 1024.0f);
            liveGain = liveGain == 0.0f ? desired : liveGain + 0.05f * (desired - liveGain);
            data = GnssSdr::floatToCs16(samples.data(), samples.size(), liveGain);
        } else {
            const QByteArray raw = file.read(static_cast<qint64>((std::min)(remainingBytes, quint64(65536 * bytesPerComplex))));
            if (raw.isEmpty()) break;
            remainingBytes -= raw.size();
            if (input.format == QStringLiteral("cs16")) data = raw;
            else {
                const int n = raw.size() / (bytesPerComplex / 2);
                samples.resize(static_cast<std::size_t>(n));
                for (int i = 0; i < n; ++i) {
                    if (input.format == QStringLiteral("cs8")) samples[i] = static_cast<qint8>(raw[i]) / 128.0f;
                    else {
                        const quint32 bits = qFromLittleEndian<quint32>(raw.constData() + i * 4);
                        std::memcpy(&samples[i], &bits, 4);
                    }
                }
                data = GnssSdr::floatToCs16(samples.data(), samples.size());
            }
        }
        if (iq->write(data) != data.size()) { emit streamError(iq->errorString()); return; }
        while (!stopping && iq->bytesToWrite() > 4 * 1024 * 1024) {
            readNmea();
            if (!iq->waitForBytesWritten(2000)) { emit streamError(QStringLiteral("PocketSDR IQ stream stalled.")); return; }
        }
        iq->flush(); count += static_cast<quint64>(data.size() / 4);
        if (report.elapsed() >= 1000) { emit samplesSent(count); report.restart(); }
    }
    while (!stopping && iq->bytesToWrite() > 0 && iq->waitForBytesWritten(1000)) readNmea();
    iq->disconnectFromHost();
    // Navigation/PVT outputs may arrive after the last input sample.
    timeout.restart();
    while (!stopping && timeout.elapsed() < 3000) { readNmea(); msleep(10); }
    emit samplesSent(count);
}

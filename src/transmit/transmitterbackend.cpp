#include "transmitterbackend.h"

#include <algorithm>

QString SimulatorTransmitterBackend::id() const {
    return QStringLiteral("simulator-iq-file");
}

QString SimulatorTransmitterBackend::displayName() const {
    return QStringLiteral("Simulator / IQ file");
}

bool SimulatorTransmitterBackend::isRfCapable() const {
    return false;
}

bool SimulatorTransmitterBackend::start(const TxConfiguration &configuration, QString *error) {
    if (configuration.sampleRate < 8000 || configuration.sampleRate > 1000000000) {
        if (error) {
            *error = QStringLiteral("Unsupported TX sample rate: %1").arg(configuration.sampleRate);
        }
        return false;
    }
    currentConfiguration = configuration;
    iq.clear();
    running = true;
    outputEnabled = true;
    if (error) {
        error->clear();
    }
    return true;
}

bool SimulatorTransmitterBackend::writeIq(const std::complex<float> *samples,
                                          int count,
                                          QString *error) {
    if (!running || !samples || count <= 0) {
        if (error) {
            *error = running ? QStringLiteral("No IQ samples")
                             : QStringLiteral("TX simulator is not running");
        }
        return false;
    }
    if (!outputEnabled) {
        if (error) error->clear();
        return true;
    }
    const qint64 maximumSamples = (std::min<qint64>)(20000000, static_cast<qint64>(currentConfiguration.sampleRate) * 180);
    const qint64 available = (std::max<qint64>)(0, maximumSamples - static_cast<qint64>(iq.size()));
    const int accepted = static_cast<int>((std::min<qint64>)(static_cast<qint64>(count), available));
    if (accepted > 0) {
        iq.reserve(iq.size() + accepted);
        for (int i = 0; i < accepted; ++i) {
            iq.append(samples[i]);
        }
    }
    if (accepted != count) {
        if (error) {
            *error = QStringLiteral("TX simulator capture reached its safety limit (180 seconds or 20 million samples)");
        }
        return false;
    }
    if (error) {
        error->clear();
    }
    return true;
}

bool SimulatorTransmitterBackend::setOutputEnabled(bool enabled, QString *error) {
    outputEnabled = enabled;
    if (error) error->clear();
    return running;
}

bool SimulatorTransmitterBackend::drain(int timeoutMs, QString *error) {
    Q_UNUSED(timeoutMs)
    if (error) error->clear();
    return running;
}

void SimulatorTransmitterBackend::stop() {
    running = false;
    outputEnabled = false;
}

bool SimulatorTransmitterBackend::isRunning() const {
    return running;
}

const QVector<std::complex<float>> &SimulatorTransmitterBackend::capturedIq() const {
    return iq;
}

const TxConfiguration &SimulatorTransmitterBackend::configuration() const {
    return currentConfiguration;
}

void SimulatorTransmitterBackend::clear() {
    iq.clear();
}

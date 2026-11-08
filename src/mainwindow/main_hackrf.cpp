#include "main.h"

#include "hackrfbackend.h"
#include "tuningutils.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QImage>
#include <QJsonDocument>
#include <QMessageBox>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextEdit>
#include <QTimer>

#include <fftw3.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace {

QString portName(int port) {
    static const char *names[] = {"A1", "A2", "A3", "A4", "B1", "B2", "B3", "B4"};
    return QString::fromLatin1(names[(std::clamp)(port, 0, 7)]);
}

QComboBox *makePortCombo(QWidget *parent, int current) {
    auto *combo = new QComboBox(parent);
    for (int port = 0; port < 8; ++port) {
        combo->addItem(portName(port), port);
    }
    combo->setCurrentIndex((std::clamp)(current, 0, 7));
    return combo;
}

QJsonArray parseArray(const QString &json) {
    const QJsonDocument document = QJsonDocument::fromJson(json.toUtf8());
    return document.isArray() ? document.array() : QJsonArray();
}

QString compactJson(const QJsonArray &array) {
    return QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
}

QString diagnosticsText(const HackRfDeviceDiagnostics &info) {
    QStringList lines;
    lines << QStringLiteral("libhackrf: %1 (%2)")
                 .arg(info.libraryVersion.isEmpty() ? QStringLiteral("?") : info.libraryVersion,
                      info.libraryRelease.isEmpty() ? QStringLiteral("?") : info.libraryRelease);
    lines << QStringLiteral("Firmware: %1")
                 .arg(info.firmwareVersion.isEmpty() ? QStringLiteral("?") : info.firmwareVersion);
    lines << QStringLiteral("USB API: %1.%2")
                 .arg((info.usbApiVersion >> 8) & 0xff, 2, 16, QLatin1Char('0'))
                 .arg(info.usbApiVersion & 0xff, 2, 16, QLatin1Char('0'));
    lines << QStringLiteral("Board: %1, revision: %2")
                 .arg(info.boardName.isEmpty() ? QStringLiteral("?") : info.boardName,
                      info.boardRevision.isEmpty() ? QStringLiteral("?") : info.boardRevision);
    lines << QStringLiteral("Platform mask: 0x%1")
                 .arg(info.supportedPlatforms, 0, 16);
    lines << QStringLiteral("CLKIN: %1")
                 .arg(info.clockInputStatusAvailable
                          ? (info.clockInputDetected ? QStringLiteral("detected")
                                                     : QStringLiteral("not detected"))
                          : QStringLiteral("unavailable"));
    lines << QStringLiteral("USB transfers: %1 bytes x %2 buffers")
                 .arg(static_cast<qulonglong>(info.transferBufferBytes))
                 .arg(info.transferQueueDepth);
    QStringList cakes;
    for (const int address : info.operaCakeAddresses) {
        cakes << QString::number(address);
    }
    lines << QStringLiteral("Opera Cake: %1")
                 .arg(cakes.isEmpty() ? QStringLiteral("not detected") : cakes.join(QStringLiteral(", ")));
    if (info.m0StateAvailable) {
        lines << QStringLiteral("M0: active=%1 requested=%2 pending=%3")
                     .arg(info.m0.activeMode)
                     .arg(info.m0.requestedMode)
                     .arg(info.m0.requestPending);
        lines << QStringLiteral("M0/M4 bytes: %1 / %2")
                     .arg(info.m0.m0Count)
                     .arg(info.m0.m4Count);
        lines << QStringLiteral("RX shortfalls: %1, longest=%2 bytes, limit=%3")
                     .arg(info.m0.shortfallCount)
                     .arg(info.m0.longestShortfall)
                     .arg(info.m0.shortfallLimit);
        lines << QStringLiteral("M0 error: %1").arg(info.m0.error);
    }
    return lines.join(QLatin1Char('\n'));
}

QVector<std::uint16_t> parseSweepRanges(const QString &text, QString *error) {
    QVector<std::uint16_t> ranges;
    const QStringList pairs = text.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString &pair : pairs) {
        const QStringList limits = pair.trimmed().split(QLatin1Char(':'));
        bool minOk = false;
        bool maxOk = false;
        const int minimum = limits.value(0).trimmed().toInt(&minOk);
        const int maximum = limits.value(1).trimmed().toInt(&maxOk);
        if (limits.size() != 2 || !minOk || !maxOk ||
            minimum < 0 || maximum > 6000 || maximum <= minimum) {
            if (error) {
                *error = QStringLiteral("Invalid sweep range: %1").arg(pair.trimmed());
            }
            return {};
        }
        ranges << static_cast<std::uint16_t>(minimum)
               << static_cast<std::uint16_t>(maximum);
        if (ranges.size() >= 20) {
            break;
        }
    }
    if (ranges.isEmpty() && error) {
        *error = QStringLiteral("At least one sweep range is required");
    }
    return ranges;
}

QColor sweepColor(float normalized) {
    const float value = (std::clamp)(normalized, 0.0f, 1.0f);
    if (value < 0.25f) {
        return QColor(0, static_cast<int>(value * 4.0f * 180.0f), 80);
    }
    if (value < 0.5f) {
        const float t = (value - 0.25f) * 4.0f;
        return QColor(0, 180 + static_cast<int>(75.0f * t),
                      80 + static_cast<int>(175.0f * t));
    }
    if (value < 0.75f) {
        const float t = (value - 0.5f) * 4.0f;
        return QColor(static_cast<int>(255.0f * t), 255,
                      static_cast<int>(255.0f * (1.0f - t)));
    }
    const float t = (value - 0.75f) * 4.0f;
    return QColor(255, static_cast<int>(255.0f * (1.0f - t)), 0);
}

class HackRfSweepPlot final : public QWidget {
public:
    explicit HackRfSweepPlot(QWidget *parent = nullptr)
        : QWidget(parent) {
        setMinimumSize(560, 360);
    }

    void setFrequencyRange(double minimumHz, double maximumHz) {
        minHz = minimumHz;
        maxHz = maximumHz;
    }

    void setSpectrum(const QVector<float> &values) {
        spectrum = values;
        if (spectrum.isEmpty()) {
            return;
        }
        const int width = spectrum.size();
        if (waterfall.width() != width || waterfall.height() != 240) {
            waterfall = QImage(width, 240, QImage::Format_RGB32);
            waterfall.fill(Qt::black);
        }
        const QImage previous = waterfall;
        waterfall.fill(Qt::black);
        {
            QPainter painter(&waterfall);
            painter.drawImage(0, 1, previous);
        }
        for (int x = 0; x < width; ++x) {
            const float normalized = (spectrum[x] + 140.0f) / 120.0f;
            waterfall.setPixelColor(x, 0, sweepColor(normalized));
        }
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(5, 8, 12));
        const QRect spectrumRect(46, 12, (std::max)(1, width() - 58),
                                 (std::max)(80, height() / 3));
        const QRect waterfallRect(46, spectrumRect.bottom() + 12,
                                  spectrumRect.width(),
                                  (std::max)(1, height() - spectrumRect.bottom() - 42));
        painter.setPen(QColor(75, 85, 96));
        painter.drawRect(spectrumRect);
        painter.drawRect(waterfallRect);
        for (int grid = 1; grid < 4; ++grid) {
            const int y = spectrumRect.top() + grid * spectrumRect.height() / 4;
            painter.drawLine(spectrumRect.left(), y, spectrumRect.right(), y);
        }
        if (!spectrum.isEmpty()) {
            QPolygonF line;
            line.reserve(spectrum.size());
            for (int i = 0; i < spectrum.size(); ++i) {
                const double x = spectrumRect.left() +
                                 spectrumRect.width() * static_cast<double>(i) /
                                     (std::max)(1, spectrum.size() - 1);
                const double normalized = (std::clamp)(
                    (static_cast<double>(spectrum[i]) + 140.0) / 120.0, 0.0, 1.0);
                const double y = spectrumRect.bottom() - normalized * spectrumRect.height();
                line << QPointF(x, y);
            }
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.setPen(QPen(QColor(80, 235, 150), 1.2));
            painter.drawPolyline(line);
        }
        if (!waterfall.isNull()) {
            painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
            painter.drawImage(waterfallRect, waterfall);
        }
        painter.setPen(QColor(215, 220, 226));
        painter.drawText(QRect(0, spectrumRect.top(), 43, 18),
                         Qt::AlignRight, QStringLiteral("-20"));
        painter.drawText(QRect(0, spectrumRect.bottom() - 18, 43, 18),
                         Qt::AlignRight, QStringLiteral("-140"));
        for (int tick = 0; tick <= 4; ++tick) {
            const double frequency = minHz + (maxHz - minHz) * tick / 4.0;
            const int x = waterfallRect.left() + waterfallRect.width() * tick / 4;
            painter.drawText(QRect(x - 55, waterfallRect.bottom() + 4, 110, 20),
                             Qt::AlignHCenter,
                             QStringLiteral("%1 MHz").arg(frequency / 1.0e6, 0, 'f', 1));
        }
    }

private:
    QVector<float> spectrum;
    QImage waterfall;
    double minHz = 0.0;
    double maxHz = 6000000000.0;
};

struct HackRfSweepState {
    std::atomic<bool> running{true};
    QMutex mutex;
    QVector<float> display;
    quint64 generation = 0;
    QString status;
    double sampleRate = 20000000.0;
    double minHz = 0.0;
    double maxHz = 6000000000.0;
    int displayBins = 2048;
    std::chrono::steady_clock::time_point lastPublish;
    QVector<float> accumulation;
    fftwf_complex *fftInput = nullptr;
    fftwf_complex *fftOutput = nullptr;
    fftwf_plan fftPlan = nullptr;
    int fftSize = 8192;

    static int callback(HackRfTransfer *transfer) {
        auto *state = transfer ? static_cast<HackRfSweepState *>(transfer->rxContext) : nullptr;
        if (!state || !state->running.load() || !transfer->buffer || transfer->validLength < 16384) {
            return state && state->running.load() ? 0 : -1;
        }
        constexpr int blockBytes = 16384;
        constexpr int headerBytes = 10;
        for (int offset = 0; offset + blockBytes <= transfer->validLength; offset += blockBytes) {
            const unsigned char *block = transfer->buffer + offset;
            if (block[0] != 0x7f || block[1] != 0x7f) {
                continue;
            }
            std::uint64_t centerHz = 0;
            for (int byte = 0; byte < 8; ++byte) {
                centerHz |= static_cast<std::uint64_t>(block[2 + byte]) << (byte * 8);
            }
            const int iqSamples = (blockBytes - headerBytes) / 2;
            for (int i = 0; i < state->fftSize; ++i) {
                float real = 0.0f;
                float imag = 0.0f;
                if (i < iqSamples) {
                    real = static_cast<float>(
                               static_cast<std::int8_t>(block[headerBytes + i * 2])) /
                           128.0f;
                    imag = static_cast<float>(
                               static_cast<std::int8_t>(block[headerBytes + i * 2 + 1])) /
                           128.0f;
                    const float window = 0.5f -
                                         0.5f * std::cos(
                                                    6.28318530717958647692f * i /
                                                    (state->fftSize - 1));
                    real *= window;
                    imag *= window;
                }
                state->fftInput[i][0] = real;
                state->fftInput[i][1] = imag;
            }
            fftwf_execute(state->fftPlan);
            const int edge = state->fftSize / 16;
            for (int shifted = edge; shifted < state->fftSize - edge; ++shifted) {
                const int fftIndex = (shifted + state->fftSize / 2) % state->fftSize;
                const float real = state->fftOutput[fftIndex][0];
                const float imag = state->fftOutput[fftIndex][1];
                const float power = real * real + imag * imag;
                const float level = 10.0f * std::log10((std::max)(power, 1.0e-20f)) -
                                    20.0f * std::log10(static_cast<float>(state->fftSize));
                const double frequency = static_cast<double>(centerHz) +
                                         (shifted - state->fftSize / 2) *
                                             state->sampleRate / state->fftSize;
                const double ratio = (frequency - state->minHz) /
                                     (state->maxHz - state->minHz);
                const int bin = static_cast<int>(ratio * state->displayBins);
                if (bin >= 0 && bin < state->accumulation.size()) {
                    state->accumulation[bin] = (std::max)(state->accumulation[bin], level);
                }
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - state->lastPublish >= std::chrono::milliseconds(100)) {
            QMutexLocker locker(&state->mutex);
            state->display = state->accumulation;
            std::fill(state->accumulation.begin(), state->accumulation.end(), -160.0f);
            ++state->generation;
            state->lastPublish = now;
        }
        return state->running.load() ? 0 : -1;
    }
};

class HackRfSweepDialog final : public QDialog {
public:
    HackRfSweepDialog(int deviceIndex,
                      const QVector<std::uint16_t> &ranges,
                      double sampleRate,
                      std::uint32_t bandwidth,
                      int lnaDb,
                      int vgaDb,
                      bool amp,
                      bool bias,
                      std::uint32_t bytesPerTune,
                      std::uint32_t stepHz,
                      bool interleaved,
                      QWidget *parent)
        : QDialog(parent),
          state(std::make_shared<HackRfSweepState>()) {
        setWindowTitle(QStringLiteral("HackRF native sweep"));
        resize(900, 620);
        auto *layout = new QVBoxLayout(this);
        plot = new HackRfSweepPlot(this);
        statusLabel = new QLabel(QStringLiteral("Starting..."), this);
        layout->addWidget(plot, 1);
        layout->addWidget(statusLabel);
        state->sampleRate = sampleRate;
        std::uint16_t minimumMhz = ranges.first();
        std::uint16_t maximumMhz = ranges.last();
        for (int index = 0; index + 1 < ranges.size(); index += 2) {
            minimumMhz = (std::min)(minimumMhz, ranges[index]);
            maximumMhz = (std::max)(maximumMhz, ranges[index + 1]);
        }
        state->minHz = static_cast<double>(minimumMhz) * 1.0e6;
        state->maxHz = static_cast<double>(maximumMhz) * 1.0e6;
        state->accumulation.fill(-160.0f, state->displayBins);
        state->display.fill(-160.0f, state->displayBins);
        state->lastPublish = std::chrono::steady_clock::now();
        plot->setFrequencyRange(state->minHz, state->maxHz);

        worker = std::thread([=]() {
            state->fftInput = reinterpret_cast<fftwf_complex *>(
                fftwf_malloc(sizeof(fftwf_complex) * state->fftSize));
            state->fftOutput = reinterpret_cast<fftwf_complex *>(
                fftwf_malloc(sizeof(fftwf_complex) * state->fftSize));
            state->fftPlan = fftwf_plan_dft_1d(state->fftSize,
                                               state->fftInput,
                                               state->fftOutput,
                                               FFTW_FORWARD,
                                               FFTW_ESTIMATE);
            void *device = nullptr;
            int result = openHackRfDeviceSafely(&device, deviceIndex);
            if (result == 0) {
                result = setHackRfSampleRateSafely(device, sampleRate);
            }
            if (result == 0) {
                result = setHackRfBandwidthSafely(device, bandwidth);
            }
            if (result == 0) {
                result = setHackRfAmpEnabledSafely(device, amp);
            }
            if (result == 0) {
                result = setHackRfLnaGainSafely(device, static_cast<std::uint32_t>(lnaDb));
            }
            if (result == 0) {
                result = setHackRfVgaGainSafely(device, static_cast<std::uint32_t>(vgaDb));
            }
            if (result == 0) {
                result = setHackRfBiasTeeEnabledSafely(device, bias);
            }
            if (result == 0) {
                result = initializeHackRfSweepSafely(
                    device, ranges, bytesPerTune, stepHz,
                    static_cast<std::uint32_t>(sampleRate * 0.5), interleaved);
            }
            if (result == 0) {
                result = startHackRfSweepSafely(device, &HackRfSweepState::callback, state.get());
            }
            {
                QMutexLocker locker(&state->mutex);
                state->status = result == 0
                                    ? QStringLiteral("Native sweep is running")
                                    : QStringLiteral("Native sweep failed: %1")
                                          .arg(hackRfLastErrorMessage());
            }
            while (result == 0 && state->running.load() &&
                   isHackRfStreamingSafely(device)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            if (device && isHackRfStreamingSafely(device)) {
                stopHackRfRxSafely(device);
            }
            if (device) {
                closeHackRfDeviceSafely(device);
            }
            if (state->fftPlan) {
                fftwf_destroy_plan(state->fftPlan);
                state->fftPlan = nullptr;
            }
            if (state->fftInput) {
                fftwf_free(state->fftInput);
                state->fftInput = nullptr;
            }
            if (state->fftOutput) {
                fftwf_free(state->fftOutput);
                state->fftOutput = nullptr;
            }
        });
        auto *timer = new QTimer(this);
        timer->setInterval(100);
        connect(timer, &QTimer::timeout, this, [this]() {
            QVector<float> values;
            QString status;
            quint64 generation = 0;
            {
                QMutexLocker locker(&state->mutex);
                generation = state->generation;
                status = state->status;
                if (generation != lastGeneration) {
                    values = state->display;
                }
            }
            if (!status.isEmpty()) {
                statusLabel->setText(status);
            }
            if (generation != lastGeneration && !values.isEmpty()) {
                lastGeneration = generation;
                plot->setSpectrum(values);
            }
        });
        timer->start();
    }

    ~HackRfSweepDialog() override {
        state->running = false;
        if (worker.joinable()) {
            worker.join();
        }
    }

protected:
    void closeEvent(QCloseEvent *event) override {
        state->running = false;
        QDialog::closeEvent(event);
    }

private:
    std::shared_ptr<HackRfSweepState> state;
    std::thread worker;
    HackRfSweepPlot *plot = nullptr;
    QLabel *statusLabel = nullptr;
    quint64 lastGeneration = 0;
};

} // namespace

void YourClassName::showHackRfRxSettings() {
    if (!isHackRfNativeSelected()) {
        QMessageBox::information(
            this,
            uiText(QStringLiteral("hackrf_rx_settings"), QStringLiteral("HackRF RX")),
            uiText(QStringLiteral("hackrf_select_receiver_first"),
                   QStringLiteral("Select a native HackRF receiver first.")));
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(uiText(QStringLiteral("hackrf_rx_settings"),
                                 QStringLiteral("HackRF RX settings")));
    dialog.resize(720, 650);
    auto *root = new QVBoxLayout(&dialog);
    auto *tabs = new QTabWidget(&dialog);
    root->addWidget(tabs, 1);

    auto *rfPage = new QWidget(tabs);
    auto *rfLayout = new QVBoxLayout(rfPage);
    auto *pathBox = new QGroupBox(
        uiText(QStringLiteral("hackrf_rf_path"), QStringLiteral("RF path and filter")), rfPage);
    auto *pathForm = new QFormLayout(pathBox);
    auto *ampCheck = new QCheckBox(
        uiText(QStringLiteral("hackrf_rf_amp"), QStringLiteral("RF amplifier (~11 dB)")), pathBox);
    ampCheck->setChecked(pendingSettings.hackRfAmpEnabled);
    auto *biasCheck = new QCheckBox(
        uiText(QStringLiteral("hackrf_bias_tee"), QStringLiteral("Bias tee 3.3 V / 50 mA")), pathBox);
    biasCheck->setChecked(pendingSettings.hackRfBiasTeeEnabled);
    biasCheck->setToolTip(uiText(
        QStringLiteral("hackrf_bias_tee_warning"),
        QStringLiteral("Enable only for an antenna or preamplifier that accepts DC power.")));
    auto *lnaSpin = new QSpinBox(pathBox);
    lnaSpin->setRange(0, 40);
    lnaSpin->setSingleStep(8);
    lnaSpin->setSuffix(QStringLiteral(" dB"));
    lnaSpin->setValue((std::clamp)(pendingSettings.hackRfLnaGainDb, 0, 40) / 8 * 8);
    auto *vgaSpin = new QSpinBox(pathBox);
    vgaSpin->setRange(0, 62);
    vgaSpin->setSingleStep(2);
    vgaSpin->setSuffix(QStringLiteral(" dB"));
    vgaSpin->setValue((std::clamp)(pendingSettings.hackRfVgaGainDb, 0, 62) / 2 * 2);
    auto *automaticBandwidthCheck = new QCheckBox(
        uiText(QStringLiteral("automatic"), QStringLiteral("Automatic")), pathBox);
    automaticBandwidthCheck->setChecked(pendingSettings.hackRfAutomaticBandwidth);
    auto *bandwidthCombo = new QComboBox(pathBox);
    const QList<int> bandwidths = {1750000, 2500000, 3500000, 5000000, 5500000,
                                   6000000, 7000000, 8000000, 9000000, 10000000,
                                   12000000, 14000000, 15000000, 20000000,
                                   24000000, 28000000};
    for (const int bandwidth : bandwidths) {
        bandwidthCombo->addItem(QStringLiteral("%1 MHz").arg(bandwidth / 1.0e6, 0, 'f', 2),
                                bandwidth);
    }
    int bandwidthIndex = bandwidthCombo->findData(pendingSettings.hackRfBandwidthHz);
    if (bandwidthIndex < 0) {
        bandwidthIndex = bandwidthCombo->findData(
            static_cast<int>(recommendedHackRfBandwidth(
                static_cast<std::uint32_t>(pendingSettings.sampleRate))));
    }
    bandwidthCombo->setCurrentIndex((std::max)(0, bandwidthIndex));
    bandwidthCombo->setEnabled(!automaticBandwidthCheck->isChecked());
    connect(automaticBandwidthCheck, &QCheckBox::toggled,
            bandwidthCombo, &QWidget::setDisabled);
    pathForm->addRow(ampCheck);
    pathForm->addRow(biasCheck);
    pathForm->addRow(QStringLiteral("LNA"), lnaSpin);
    pathForm->addRow(QStringLiteral("VGA"), vgaSpin);
    pathForm->addRow(uiText(QStringLiteral("hackrf_baseband_filter"),
                            QStringLiteral("Baseband filter")),
                     automaticBandwidthCheck);
    pathForm->addRow(uiText(QStringLiteral("manual_bandwidth"),
                            QStringLiteral("Manual bandwidth")),
                     bandwidthCombo);
    rfLayout->addWidget(pathBox);

    auto *tuningBox = new QGroupBox(
        uiText(QStringLiteral("hackrf_tuning"), QStringLiteral("Tuning and image path")), rfPage);
    auto *tuningForm = new QFormLayout(tuningBox);
    auto *explicitCheck = new QCheckBox(
        uiText(QStringLiteral("hackrf_explicit_tuning"), QStringLiteral("Explicit IF/LO tuning")),
        tuningBox);
    explicitCheck->setChecked(pendingSettings.hackRfExplicitTuningEnabled);
    auto *ifSpin = new QDoubleSpinBox(tuningBox);
    ifSpin->setDecimals(6);
    ifSpin->setRange(2150.0, 2750.0);
    ifSpin->setSuffix(QStringLiteral(" MHz"));
    ifSpin->setValue(pendingSettings.hackRfExplicitIfHz / 1.0e6);
    auto *loSpin = new QDoubleSpinBox(tuningBox);
    loSpin->setDecimals(6);
    loSpin->setRange(84.375, 5400.0);
    loSpin->setSuffix(QStringLiteral(" MHz"));
    loSpin->setValue(pendingSettings.hackRfExplicitLoHz / 1.0e6);
    auto *pathCombo = new QComboBox(tuningBox);
    pathCombo->addItem(QStringLiteral("BYPASS: center = IF"), 0);
    pathCombo->addItem(QStringLiteral("LPF: center = IF - LO"), 1);
    pathCombo->addItem(QStringLiteral("HPF: center = IF + LO"), 2);
    pathCombo->setCurrentIndex((std::clamp)(pendingSettings.hackRfExplicitPath, 0, 2));
    auto *resultingCenterLabel = new QLabel(tuningBox);
    const auto updateExplicitUi = [=]() {
        const bool enabled = explicitCheck->isChecked();
        ifSpin->setEnabled(enabled);
        loSpin->setEnabled(enabled && pathCombo->currentData().toInt() != 0);
        pathCombo->setEnabled(enabled);
        double centerMhz = ifSpin->value();
        if (pathCombo->currentData().toInt() == 1) {
            centerMhz -= loSpin->value();
        } else if (pathCombo->currentData().toInt() == 2) {
            centerMhz += loSpin->value();
        }
        resultingCenterLabel->setText(QStringLiteral("%1 MHz").arg(centerMhz, 0, 'f', 6));
    };
    connect(explicitCheck, &QCheckBox::toggled, &dialog, updateExplicitUi);
    connect(ifSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), &dialog,
            [=](double) { updateExplicitUi(); });
    connect(loSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), &dialog,
            [=](double) { updateExplicitUi(); });
    connect(pathCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog,
            [=](int) { updateExplicitUi(); });
    updateExplicitUi();
    tuningForm->addRow(explicitCheck);
    tuningForm->addRow(QStringLiteral("IF"), ifSpin);
    tuningForm->addRow(QStringLiteral("LO"), loSpin);
    tuningForm->addRow(uiText(QStringLiteral("hackrf_image_path"),
                              QStringLiteral("Image-reject path")),
                       pathCombo);
    tuningForm->addRow(uiText(QStringLiteral("center_frequency"),
                              QStringLiteral("Center frequency")),
                       resultingCenterLabel);
    rfLayout->addWidget(tuningBox);

    auto *clockBox = new QGroupBox(
        uiText(QStringLiteral("hackrf_clock_sync"), QStringLiteral("Clock and synchronization")),
        rfPage);
    auto *clockForm = new QFormLayout(clockBox);
    auto *clockOutCheck = new QCheckBox(QStringLiteral("10 MHz CLKOUT"), clockBox);
    clockOutCheck->setChecked(pendingSettings.hackRfClockOutEnabled);
    auto *hardwareSyncCheck = new QCheckBox(
        uiText(QStringLiteral("hackrf_hw_sync"), QStringLiteral("Hardware trigger/sync")),
        clockBox);
    hardwareSyncCheck->setChecked(pendingSettings.hackRfHardwareSyncEnabled);
    hardwareSyncCheck->setToolTip(uiText(
        QStringLiteral("hackrf_hw_sync_tooltip"),
        QStringLiteral("RX waits for the external hardware trigger when this is enabled.")));
    auto *overrunSpin = new QSpinBox(clockBox);
    overrunSpin->setRange(0, 2000000000);
    overrunSpin->setSpecialValueText(uiText(QStringLiteral("disabled"),
                                            QStringLiteral("Disabled")));
    overrunSpin->setSuffix(QStringLiteral(" samples"));
    overrunSpin->setValue(pendingSettings.hackRfRxOverrunLimit);
    clockForm->addRow(clockOutCheck);
    clockForm->addRow(hardwareSyncCheck);
    clockForm->addRow(uiText(QStringLiteral("hackrf_rx_overrun_limit"),
                             QStringLiteral("RX overrun stop limit")),
                      overrunSpin);
    rfLayout->addWidget(clockBox);
    rfLayout->addStretch(1);
    tabs->addTab(rfPage, uiText(QStringLiteral("receiver"), QStringLiteral("Receiver")));

    auto *operaPage = new QWidget(tabs);
    auto *operaLayout = new QVBoxLayout(operaPage);
    auto *operaEnabled = new QCheckBox(
        uiText(QStringLiteral("hackrf_operacake_enable"), QStringLiteral("Enable Opera Cake")),
        operaPage);
    operaEnabled->setChecked(pendingSettings.hackRfOperaCakeEnabled);
    operaLayout->addWidget(operaEnabled);
    auto *operaForm = new QFormLayout();
    auto *addressSpin = new QSpinBox(operaPage);
    addressSpin->setRange(0, 7);
    addressSpin->setValue(pendingSettings.hackRfOperaCakeAddress);
    auto *operaMode = new QComboBox(operaPage);
    operaMode->addItem(uiText(QStringLiteral("manual"), QStringLiteral("Manual")), 0);
    operaMode->addItem(uiText(QStringLiteral("frequency"), QStringLiteral("Frequency")), 1);
    operaMode->addItem(uiText(QStringLiteral("time"), QStringLiteral("Time")), 2);
    operaMode->setCurrentIndex((std::clamp)(pendingSettings.hackRfOperaCakeMode, 0, 2));
    auto *portA = makePortCombo(operaPage, pendingSettings.hackRfOperaCakePortA);
    auto *portB = makePortCombo(operaPage, pendingSettings.hackRfOperaCakePortB);
    operaForm->addRow(uiText(QStringLiteral("address"), QStringLiteral("Address")), addressSpin);
    operaForm->addRow(uiText(QStringLiteral("mode"), QStringLiteral("Mode")), operaMode);
    operaForm->addRow(QStringLiteral("A0 ->"), portA);
    operaForm->addRow(QStringLiteral("B0 ->"), portB);
    operaLayout->addLayout(operaForm);

    auto *rangeTable = new QTableWidget(8, 3, operaPage);
    rangeTable->setHorizontalHeaderLabels(
        {uiText(QStringLiteral("start_mhz"), QStringLiteral("Start MHz")),
         uiText(QStringLiteral("end_mhz"), QStringLiteral("End MHz")),
         uiText(QStringLiteral("port"), QStringLiteral("Port"))});
    rangeTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    const QJsonArray rangeArray = parseArray(pendingSettings.hackRfOperaCakeRangesJson);
    for (int row = 0; row < rangeTable->rowCount(); ++row) {
        const QJsonObject object = row < rangeArray.size() ? rangeArray.at(row).toObject()
                                                           : QJsonObject();
        rangeTable->setItem(row, 0, new QTableWidgetItem(
            object.contains(QStringLiteral("minMhz"))
                ? QString::number(object.value(QStringLiteral("minMhz")).toInt())
                : QString()));
        rangeTable->setItem(row, 1, new QTableWidgetItem(
            object.contains(QStringLiteral("maxMhz"))
                ? QString::number(object.value(QStringLiteral("maxMhz")).toInt())
                : QString()));
        rangeTable->setCellWidget(row, 2,
                                 makePortCombo(rangeTable,
                                               object.value(QStringLiteral("port")).toInt()));
    }
    operaLayout->addWidget(new QLabel(
        uiText(QStringLiteral("hackrf_operacake_ranges"),
               QStringLiteral("Frequency switching ranges (priority order)")),
        operaPage));
    operaLayout->addWidget(rangeTable, 1);

    auto *dwellTable = new QTableWidget(16, 2, operaPage);
    dwellTable->setHorizontalHeaderLabels(
        {uiText(QStringLiteral("samples"), QStringLiteral("Samples")),
         uiText(QStringLiteral("port"), QStringLiteral("Port"))});
    dwellTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    const QJsonArray dwellArray = parseArray(pendingSettings.hackRfOperaCakeDwellsJson);
    for (int row = 0; row < dwellTable->rowCount(); ++row) {
        const QJsonObject object = row < dwellArray.size() ? dwellArray.at(row).toObject()
                                                           : QJsonObject();
        dwellTable->setItem(row, 0, new QTableWidgetItem(
            object.contains(QStringLiteral("samples"))
                ? QString::number(object.value(QStringLiteral("samples")).toVariant().toLongLong())
                : QString()));
        dwellTable->setCellWidget(row, 1,
                                 makePortCombo(dwellTable,
                                               object.value(QStringLiteral("port")).toInt()));
    }
    operaLayout->addWidget(new QLabel(
        uiText(QStringLiteral("hackrf_operacake_dwells"),
               QStringLiteral("Time switching dwell list")),
        operaPage));
    operaLayout->addWidget(dwellTable, 1);
    const auto updateOperaUi = [=]() {
        const bool enabled = operaEnabled->isChecked();
        const int mode = operaMode->currentData().toInt();
        addressSpin->setEnabled(enabled);
        operaMode->setEnabled(enabled);
        portA->setEnabled(enabled && mode == 0);
        portB->setEnabled(enabled && mode == 0);
        rangeTable->setEnabled(enabled && mode == 1);
        dwellTable->setEnabled(enabled && mode == 2);
    };
    connect(operaEnabled, &QCheckBox::toggled, &dialog, updateOperaUi);
    connect(operaMode, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog,
            [=](int) { updateOperaUi(); });
    updateOperaUi();
    tabs->addTab(operaPage, QStringLiteral("Opera Cake"));

    auto *sweepPage = new QWidget(tabs);
    auto *sweepForm = new QFormLayout(sweepPage);
    auto *sweepRangesEdit = new QLineEdit(pendingSettings.hackRfSweepRanges, sweepPage);
    sweepRangesEdit->setPlaceholderText(QStringLiteral("0:1000, 2400:2500"));
    auto *sweepStepSpin = new QSpinBox(sweepPage);
    sweepStepSpin->setRange(1, 20000);
    sweepStepSpin->setSuffix(QStringLiteral(" kHz"));
    sweepStepSpin->setValue((std::max)(1, pendingSettings.hackRfSweepStepHz / 1000));
    auto *bytesPerTuneCombo = new QComboBox(sweepPage);
    for (int blocks : {1, 2, 4, 8, 16, 32, 64}) {
        const int bytes = 16384 * blocks;
        bytesPerTuneCombo->addItem(QStringLiteral("%1 (%2 block%3)")
                                       .arg(bytes)
                                       .arg(blocks)
                                       .arg(blocks == 1 ? QString() : QStringLiteral("s")),
                                   bytes);
    }
    int bytesIndex = bytesPerTuneCombo->findData(pendingSettings.hackRfSweepBytesPerTune);
    bytesPerTuneCombo->setCurrentIndex(bytesIndex >= 0 ? bytesIndex : 0);
    auto *interleavedCheck = new QCheckBox(
        uiText(QStringLiteral("hackrf_sweep_interleaved"), QStringLiteral("Interleaved sweep")),
        sweepPage);
    interleavedCheck->setChecked(pendingSettings.hackRfSweepInterleaved);
    auto *sweepNote = new QLabel(
        uiText(QStringLiteral("hackrf_sweep_note"),
               QStringLiteral("Native sweep uses frequency-tagged blocks and is separate from raw-IQ reception.")),
        sweepPage);
    sweepNote->setWordWrap(true);
    auto *openSweepButton = new QPushButton(
        uiText(QStringLiteral("hackrf_open_native_sweep"),
               QStringLiteral("Open native sweep")),
        sweepPage);
    sweepForm->addRow(uiText(QStringLiteral("ranges_mhz"), QStringLiteral("Ranges MHz")),
                      sweepRangesEdit);
    sweepForm->addRow(uiText(QStringLiteral("step"), QStringLiteral("Step")), sweepStepSpin);
    sweepForm->addRow(uiText(QStringLiteral("bytes_per_tune"),
                             QStringLiteral("Bytes per tuning")),
                      bytesPerTuneCombo);
    sweepForm->addRow(interleavedCheck);
    sweepForm->addRow(sweepNote);
    sweepForm->addRow(openSweepButton);
    connect(openSweepButton, &QPushButton::clicked, &dialog, [&, this]() {
        if (!isIdle() || (processor && processor->isRunning())) {
            QMessageBox::information(
                &dialog,
                uiText(QStringLiteral("hackrf_open_native_sweep"),
                       QStringLiteral("HackRF native sweep")),
                uiText(QStringLiteral("hackrf_sweep_stop_receiver"),
                       QStringLiteral("Stop normal reception before opening native sweep.")));
            return;
        }
        QString rangeError;
        const QVector<std::uint16_t> ranges =
            parseSweepRanges(sweepRangesEdit->text(), &rangeError);
        if (ranges.isEmpty()) {
            QMessageBox::warning(&dialog,
                                 QStringLiteral("HackRF native sweep"),
                                 rangeError);
            return;
        }
        if (!pendingSettings.hackRfBiasTeeEnabled && biasCheck->isChecked()) {
            const auto answer = QMessageBox::warning(
                &dialog,
                uiText(QStringLiteral("hackrf_bias_tee"), QStringLiteral("HackRF bias tee")),
                uiText(QStringLiteral("hackrf_bias_tee_confirm"),
                       QStringLiteral("This enables 3.3 V DC on the antenna connector. Continue only if the connected hardware accepts DC power.")),
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No);
            if (answer != QMessageBox::Yes) {
                return;
            }
        }
        const double sampleRate = (std::clamp)(pendingSettings.sampleRate,
                                               2000000.0,
                                               20000000.0);
        const std::uint32_t bandwidth =
            automaticBandwidthCheck->isChecked()
                ? recommendedHackRfBandwidth(static_cast<std::uint32_t>(sampleRate))
                : static_cast<std::uint32_t>(bandwidthCombo->currentData().toUInt());
        HackRfSweepDialog sweep(selectedHackRfNativeIndex(),
                                ranges,
                                sampleRate,
                                nearestHackRfBandwidth(
                                    (std::min)(bandwidth,
                                               static_cast<std::uint32_t>(sampleRate))),
                                (std::clamp)(lnaSpin->value(), 0, 40) / 8 * 8,
                                (std::clamp)(vgaSpin->value(), 0, 62) / 2 * 2,
                                ampCheck->isChecked(),
                                biasCheck->isChecked(),
                                static_cast<std::uint32_t>(bytesPerTuneCombo->currentData().toUInt()),
                                static_cast<std::uint32_t>(sweepStepSpin->value() * 1000),
                                interleavedCheck->isChecked(),
                                &dialog);
        sweep.exec();
    });
    tabs->addTab(sweepPage, uiText(QStringLiteral("scan"), QStringLiteral("Sweep")));

    auto *diagnosticsPage = new QWidget(tabs);
    auto *diagnosticsLayout = new QVBoxLayout(diagnosticsPage);
    auto *diagnosticsView = new QTextEdit(diagnosticsPage);
    diagnosticsView->setReadOnly(true);
    auto *refreshDiagnostics = new QPushButton(
        uiText(QStringLiteral("refresh"), QStringLiteral("Refresh")), diagnosticsPage);
    diagnosticsLayout->addWidget(diagnosticsView, 1);
    diagnosticsLayout->addWidget(refreshDiagnostics);
    const auto refresh = [this, diagnosticsView]() {
        HackRfDeviceDiagnostics diagnostics;
        QString error;
        bool ok = false;
        if (processor && processor->isRunning() && isHackRfNativeSelected()) {
            ok = processor->queryHackRfDiagnostics(&diagnostics, &error);
        } else {
            ok = queryHackRfDeviceDiagnostics(selectedHackRfNativeIndex(), &diagnostics, &error);
        }
        diagnosticsView->setPlainText(
            ok ? diagnosticsText(diagnostics)
               : uiText(QStringLiteral("hackrf_diagnostics_failed"),
                        QStringLiteral("Diagnostics failed: %1")).arg(error));
    };
    connect(refreshDiagnostics, &QPushButton::clicked, &dialog, refresh);
    tabs->addTab(diagnosticsPage,
                 uiText(QStringLiteral("diagnostics"), QStringLiteral("Diagnostics")));
    refresh();

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok |
                                         QDialogButtonBox::Cancel,
                                         &dialog);
    root->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    if (!pendingSettings.hackRfBiasTeeEnabled && biasCheck->isChecked()) {
        const auto answer = QMessageBox::warning(
            this,
            uiText(QStringLiteral("hackrf_bias_tee"), QStringLiteral("HackRF bias tee")),
            uiText(QStringLiteral("hackrf_bias_tee_confirm"),
                   QStringLiteral("This enables 3.3 V DC on the antenna connector. Continue only if the connected hardware accepts DC power.")),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (answer != QMessageBox::Yes) {
            biasCheck->setChecked(false);
        }
    }

    pendingSettings.hackRfAmpEnabled = ampCheck->isChecked();
    pendingSettings.hackRfBiasTeeEnabled = biasCheck->isChecked();
    pendingSettings.hackRfLnaGainDb =
        (std::clamp)(lnaSpin->value(), 0, 40) / 8 * 8;
    pendingSettings.hackRfVgaGainDb =
        (std::clamp)(vgaSpin->value(), 0, 62) / 2 * 2;
    pendingSettings.hackRfAutomaticBandwidth = automaticBandwidthCheck->isChecked();
    pendingSettings.hackRfBandwidthHz = bandwidthCombo->currentData().toInt();
    pendingSettings.hackRfExplicitTuningEnabled = explicitCheck->isChecked();
    pendingSettings.hackRfExplicitIfHz = ifSpin->value() * 1.0e6;
    pendingSettings.hackRfExplicitLoHz = loSpin->value() * 1.0e6;
    pendingSettings.hackRfExplicitPath = pathCombo->currentData().toInt();
    pendingSettings.hackRfClockOutEnabled = clockOutCheck->isChecked();
    pendingSettings.hackRfHardwareSyncEnabled = hardwareSyncCheck->isChecked();
    pendingSettings.hackRfRxOverrunLimit = overrunSpin->value();
    pendingSettings.hackRfOperaCakeEnabled = operaEnabled->isChecked();
    pendingSettings.hackRfOperaCakeAddress = addressSpin->value();
    pendingSettings.hackRfOperaCakeMode = operaMode->currentData().toInt();
    pendingSettings.hackRfOperaCakePortA = portA->currentData().toInt();
    pendingSettings.hackRfOperaCakePortB = portB->currentData().toInt();
    if (pendingSettings.hackRfOperaCakeEnabled &&
        pendingSettings.hackRfOperaCakeMode == 0 &&
        pendingSettings.hackRfOperaCakePortA / 4 ==
            pendingSettings.hackRfOperaCakePortB / 4) {
        QMessageBox::warning(
            this,
            QStringLiteral("Opera Cake"),
            uiText(QStringLiteral("hackrf_operacake_manual_invalid"),
                   QStringLiteral("Manual A0 and B0 ports must be on opposite sides. Opera Cake was disabled.")));
        pendingSettings.hackRfOperaCakeEnabled = false;
    }

    QJsonArray outputRanges;
    for (int row = 0; row < rangeTable->rowCount(); ++row) {
        bool minOk = false;
        bool maxOk = false;
        const int minimum = rangeTable->item(row, 0)
                                ? rangeTable->item(row, 0)->text().toInt(&minOk)
                                : 0;
        const int maximum = rangeTable->item(row, 1)
                                ? rangeTable->item(row, 1)->text().toInt(&maxOk)
                                : 0;
        auto *port = qobject_cast<QComboBox *>(rangeTable->cellWidget(row, 2));
        if (minOk && maxOk && maximum > minimum && port) {
            outputRanges.append(QJsonObject{{QStringLiteral("minMhz"), minimum},
                                            {QStringLiteral("maxMhz"), maximum},
                                            {QStringLiteral("port"), port->currentData().toInt()}});
        }
    }
    pendingSettings.hackRfOperaCakeRangesJson = compactJson(outputRanges);

    QJsonArray outputDwells;
    for (int row = 0; row < dwellTable->rowCount(); ++row) {
        bool samplesOk = false;
        const qint64 samples = dwellTable->item(row, 0)
                                   ? dwellTable->item(row, 0)->text().toLongLong(&samplesOk)
                                   : 0;
        auto *port = qobject_cast<QComboBox *>(dwellTable->cellWidget(row, 1));
        if (samplesOk && samples > 0 && port) {
            outputDwells.append(QJsonObject{{QStringLiteral("samples"), samples},
                                            {QStringLiteral("port"), port->currentData().toInt()}});
        }
    }
    pendingSettings.hackRfOperaCakeDwellsJson = compactJson(outputDwells);
    pendingSettings.hackRfSweepRanges = sweepRangesEdit->text().trimmed();
    pendingSettings.hackRfSweepStepHz = sweepStepSpin->value() * 1000;
    pendingSettings.hackRfSweepBytesPerTune = bytesPerTuneCombo->currentData().toInt();
    pendingSettings.hackRfSweepInterleaved = interleavedCheck->isChecked();

    if (pendingSettings.hackRfExplicitTuningEnabled) {
        double center = pendingSettings.hackRfExplicitIfHz;
        if (pendingSettings.hackRfExplicitPath == 1) {
            center -= pendingSettings.hackRfExplicitLoHz;
        } else if (pendingSettings.hackRfExplicitPath == 2) {
            center += pendingSettings.hackRfExplicitLoHz;
        }
        if (center < 1000000.0 || center > 6000000000.0) {
            QMessageBox::warning(
                this,
                uiText(QStringLiteral("hackrf_explicit_tuning"),
                       QStringLiteral("HackRF explicit tuning")),
                uiText(QStringLiteral("hackrf_explicit_invalid"),
                       QStringLiteral("The selected IF/LO/path combination produces a center frequency outside 1-6000 MHz. Explicit tuning was disabled.")));
            pendingSettings.hackRfExplicitTuningEnabled = false;
        } else {
            pendingSettings.centerFrequency = center;
            pendingSettings.actualFrequency = center;
            normalizeTuning(pendingSettings, true);
            if (frequencyControl) {
                QSignalBlocker blocker(frequencyControl);
                frequencyControl->setValueHz(pendingSettings.centerFrequency);
            }
        }
    }

    publishSettingsToGlobals();
    updateReceiverSpecificControls();
    savePersistentSettings();
    if (isNetworkClientMode()) {
        scheduleRemoteSettingsCommand();
    } else if (processor && processor->isRunning()) {
        restartStreamForHardwareChange();
    }
}

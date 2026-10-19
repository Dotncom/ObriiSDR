#include "transmitdialog.h"

#include "appsettingsutils.h"
#include "dmrprivacyutils.h"
#include "dmrtransmitencoder.h"
#include "hackrfbackend.h"
#include "sstvimageeditordialog.h"
#include "signallabdialog.h"
#include "signalunits.h"
#include "transmitmediagenerator.h"
#ifdef _WIN32
#include "windowsmicrophonecapture.h"
#endif

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QStringList>
#include <QStyle>
#include <QStandardItemModel>
#include <QStandardItem>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#ifdef FOBOSAPP_HAS_QT_MULTIMEDIA
#include <QAudioDeviceInfo>
#include <QAudioFormat>
#include <QAudioInput>
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

#ifdef FOBOSAPP_HAS_QT_MULTIMEDIA
namespace {
QString audioInputErrorText(QAudio::Error error) {
    switch (error) {
    case QAudio::NoError: return QStringLiteral("no error");
    case QAudio::OpenError: return QStringLiteral("device open error");
    case QAudio::IOError: return QStringLiteral("device I/O error");
    case QAudio::UnderrunError: return QStringLiteral("capture overrun");
    case QAudio::FatalError: return QStringLiteral("fatal audio backend error");
    }
    return QStringLiteral("unknown audio error");
}

bool supportedAudioCaptureFormat(const QAudioFormat &format) {
    if (format.codec() != QStringLiteral("audio/pcm") ||
        format.sampleRate() < 8000 || format.sampleRate() > 192000 ||
        format.channelCount() < 1 || format.channelCount() > 2) {
        return false;
    }
    if (format.sampleType() == QAudioFormat::Float) {
        return format.sampleSize() == 32;
    }
    if (format.sampleType() == QAudioFormat::SignedInt) {
        return format.sampleSize() == 8 || format.sampleSize() == 16 ||
               format.sampleSize() == 24 || format.sampleSize() == 32;
    }
    return format.sampleType() == QAudioFormat::UnSignedInt && format.sampleSize() == 8;
}
}
#endif

namespace {
bool constrainVoiceDeviation(TxConfiguration &configuration) {
    if (configuration.signalBandwidthHz <= 0.0) return false;
    double maximumDeviation = 0.0;
    if (configuration.modulation == TxModulation::Nfm) {
        // Reserve at least half of the one-sided channel width for voice.
        maximumDeviation = configuration.signalBandwidthHz * 0.25;
    } else if (configuration.modulation == TxModulation::Wfm) {
        // Broadcast-style FM conventionally uses roughly 75 kHz in 200 kHz.
        maximumDeviation = configuration.signalBandwidthHz * 0.40;
    }
    if (maximumDeviation <= 0.0 || configuration.deviationHz <= maximumDeviation) {
        return false;
    }
    configuration.deviationHz = (std::max)(100.0, maximumDeviation);
    return true;
}

float softLimitMicrophone(float sample) {
    constexpr float LinearLimit = 0.85f;
    constexpr float RemainingHeadroom = 1.0f - LinearLimit;
    const float magnitude = std::abs(sample);
    if (magnitude <= LinearLimit) return sample;
    const float limited = LinearLimit + RemainingHeadroom *
        std::tanh((magnitude - LinearLimit) / RemainingHeadroom);
    return std::copysign(limited, sample);
}
}

class TxWaveformPreviewWidget final : public QWidget {
public:
    explicit TxWaveformPreviewWidget(QWidget *parent = nullptr) : QWidget(parent) {
        setMinimumHeight(170);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }

    void setIq(const QVector<std::complex<float>> &samples) {
        iMinimum.clear();
        iMaximum.clear();
        qMinimum.clear();
        qMaximum.clear();
        if (!samples.isEmpty()) {
            const int target = (std::min)(samples.size(), 1800);
            iMinimum.reserve(target);
            iMaximum.reserve(target);
            qMinimum.reserve(target);
            qMaximum.reserve(target);
            for (int x = 0; x < target; ++x) {
                const int begin = static_cast<int>((static_cast<qint64>(x) * samples.size()) / target);
                const int end = (std::max)(begin + 1,
                    static_cast<int>((static_cast<qint64>(x + 1) * samples.size()) / target));
                float minI = 1.0f;
                float maxI = -1.0f;
                float minQ = 1.0f;
                float maxQ = -1.0f;
                for (int i = begin; i < (std::min)(end, samples.size()); ++i) {
                    minI = (std::min)(minI, samples[i].real());
                    maxI = (std::max)(maxI, samples[i].real());
                    minQ = (std::min)(minQ, samples[i].imag());
                    maxQ = (std::max)(maxQ, samples[i].imag());
                }
                iMinimum.append(minI);
                iMaximum.append(maxI);
                qMinimum.append(minQ);
                qMaximum.append(maxQ);
            }
        }
        update();
    }

    void clear() {
        iMinimum.clear();
        iMaximum.clear();
        qMinimum.clear();
        qMaximum.clear();
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(15, 18, 22));
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.setPen(QColor(55, 63, 72));
        painter.drawLine(0, height() / 2, width(), height() / 2);
        for (int x = 0; x < width(); x += (std::max)(1, width() / 10)) {
            painter.drawLine(x, 0, x, height());
        }
        if (iMinimum.size() < 2) return;
        const double half = height() * 0.46;
        const auto drawEnvelope = [&](const QVector<float> &minimum,
                                      const QVector<float> &maximum,
                                      const QColor &color) {
            painter.setPen(QPen(color, 1.0));
            for (int i = 0; i < minimum.size(); ++i) {
                const double x = static_cast<double>(i) * (width() - 1) /
                                 static_cast<double>(minimum.size() - 1);
                const double top = height() * 0.5 -
                    (std::clamp)(maximum[i], -1.0f, 1.0f) * half;
                const double bottom = height() * 0.5 -
                    (std::clamp)(minimum[i], -1.0f, 1.0f) * half;
                painter.drawLine(QPointF(x, top), QPointF(x, bottom));
            }
        };
        drawEnvelope(iMinimum, iMaximum, QColor(42, 210, 154, 220));
        drawEnvelope(qMinimum, qMaximum, QColor(75, 155, 255, 190));
        painter.setPen(QColor(42, 210, 154));
        painter.drawText(7, 16, QStringLiteral("I"));
        painter.setPen(QColor(75, 155, 255));
        painter.drawText(20, 16, QStringLiteral("Q"));
    }

private:
    QVector<float> iMinimum;
    QVector<float> iMaximum;
    QVector<float> qMinimum;
    QVector<float> qMaximum;
};

TransmitDialog::TransmitDialog(const QString &language, QWidget *parent)
    : QDialog(parent), language(language) {
    setAttribute(Qt::WA_DeleteOnClose, false);
    resize(760, 650);
    buildUi();
    populateAudioInputs();
    populateHackRfDevices();
    loadSettings();
    updateTexts();
    updateModeUi();
    QTimer::singleShot(750, this, [this]() {
        if (audioInputCombo->currentData().toString().isEmpty()) {
            populateAudioInputs();
            updateModeUi();
        }
    });
    qApp->installEventFilter(this);
}

TransmitDialog::~TransmitDialog() {
    qApp->removeEventFilter(this);
    stopTransmission();
}

bool TransmitDialog::eventFilter(QObject *watched, QEvent *event) {
    auto *watchedWidget = qobject_cast<QWidget *>(watched);
    const bool insideDialog = watchedWidget &&
                              (watchedWidget == this || isAncestorOf(watchedWidget));
    const QString source = sourceCombo->currentData().toString();
    const bool liveSource = source == QStringLiteral("manual-cw") ||
                            source == QStringLiteral("microphone-ptt");
    if (!insideDialog || !isVisible() || !liveSource) {
        return QDialog::eventFilter(watched, event);
    }
    if (event->type() == QEvent::WindowDeactivate && liveKeyPressed) {
        setLiveKey(false);
        return QDialog::eventFilter(watched, event);
    }
    auto isLiveHotkey = [](const QKeyEvent *key) {
        return key->key() == Qt::Key_F12;
    };
    if (event->type() == QEvent::ShortcutOverride) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (isLiveHotkey(key)) {
            key->accept();
            return true;
        }
    }
    if (event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (isLiveHotkey(key)) {
            if (!key->isAutoRepeat() && activeBackend && activeBackend->isRunning()) {
                setLiveKey(true);
            }
            return true;
        }
    } else if (event->type() == QEvent::KeyRelease) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_F12) {
            if (!key->isAutoRepeat() && liveKeyPressed) setLiveKey(false);
            return true;
        }
    }
    return QDialog::eventFilter(watched, event);
}

QString TransmitDialog::text(const QString &english, const QString &ukrainian) const {
    return language.toLower().startsWith(QStringLiteral("uk")) ? ukrainian : english;
}

void TransmitDialog::buildUi() {
    QVBoxLayout *root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(8);

    QGroupBox *hardwareBox = new QGroupBox(this);
    hardwareBox->setObjectName(QStringLiteral("txHardwareBox"));
    QGridLayout *hardware = new QGridLayout(hardwareBox);
    backendCombo = new QComboBox(hardwareBox);
    backendCombo->addItem(QStringLiteral("Simulator / IQ file"), QStringLiteral("simulator-iq-file"));
    backendCombo->addItem(QStringLiteral("HackRF native"), QStringLiteral("hackrf-native"));
    deviceCombo = new QComboBox(hardwareBox);
    frequencySpin = new QDoubleSpinBox(hardwareBox);
    frequencySpin->setRange(1.0, 7250.0);
    frequencySpin->setDecimals(6);
    frequencySpin->setSingleStep(0.0125);
    frequencySpin->setSuffix(QStringLiteral(" MHz"));
    sampleRateCombo = new QComboBox(hardwareBox);
    sampleRateCombo->setEditable(true);
    sampleRateCombo->setInsertPolicy(QComboBox::NoInsert);
    for (int rate : {12000, 24000, 48000, 96000, 192000, 1000000, 2000000, 20000000,
                     100000000, 500000000, 1000000000}) {
        sampleRateCombo->addItem(signalunits::sampleRateText(rate), rate);
    }
    levelSpin = new QSpinBox(hardwareBox);
    levelSpin->setRange(1, 90);
    levelSpin->setSuffix(QStringLiteral(" %"));
    deviceSampleRateCombo = new QComboBox(hardwareBox);
    for (int rate : {2000000, 4000000, 8000000, 10000000, 12000000, 16000000, 20000000}) {
        deviceSampleRateCombo->addItem(QStringLiteral("%1 Msps").arg(rate / 1000000), rate);
    }
    bandwidthCombo = new QComboBox(hardwareBox);
    bandwidthCombo->addItem(QStringLiteral("Auto"), 0);
    for (int bandwidth : {1750000, 2500000, 3500000, 5000000, 7000000, 10000000,
                          12000000, 14000000, 15000000, 20000000}) {
        bandwidthCombo->addItem(QStringLiteral("%1 MHz").arg(bandwidth / 1e6, 0, 'f', 2), bandwidth);
    }
    txGainSpin = new QSpinBox(hardwareBox);
    txGainSpin->setRange(0, 47);
    txGainSpin->setSuffix(QStringLiteral(" dB"));
    watchdogSpin = new QSpinBox(hardwareBox);
    watchdogSpin->setRange(1, 3600);
    watchdogSpin->setValue(30);
    watchdogSpin->setSuffix(QStringLiteral(" s"));
    rfAmpCheck = new QCheckBox(hardwareBox);
    armCheck = new QCheckBox(hardwareBox);
    armCheck->setChecked(false);
    rfLockLabel = new QLabel(hardwareBox);
    rfLockLabel->setWordWrap(true);
    rfLockLabel->setStyleSheet(QStringLiteral("color: #d0a43b; font-weight: 600;"));
    backendLabel = new QLabel(hardwareBox);
    deviceLabel = new QLabel(hardwareBox);
    rfLabel = new QLabel(hardwareBox);
    basebandLabel = new QLabel(hardwareBox);
    deviceRateLabel = new QLabel(hardwareBox);
    bandwidthLabel = new QLabel(hardwareBox);
    levelLabel = new QLabel(hardwareBox);
    txGainLabel = new QLabel(hardwareBox);
    watchdogLabel = new QLabel(hardwareBox);
    hardware->addWidget(backendLabel, 0, 0);
    hardware->addWidget(backendCombo, 0, 1);
    hardware->addWidget(deviceLabel, 0, 2);
    hardware->addWidget(deviceCombo, 0, 3);
    hardware->addWidget(rfLabel, 1, 0);
    hardware->addWidget(frequencySpin, 1, 1);
    hardware->addWidget(deviceRateLabel, 1, 2);
    hardware->addWidget(deviceSampleRateCombo, 1, 3);
    hardware->addWidget(bandwidthLabel, 2, 0);
    hardware->addWidget(bandwidthCombo, 2, 1);
    hardware->addWidget(txGainLabel, 2, 2);
    hardware->addWidget(txGainSpin, 2, 3);
    hardware->addWidget(watchdogLabel, 3, 0);
    hardware->addWidget(watchdogSpin, 3, 1);
    hardware->addWidget(rfAmpCheck, 3, 2);
    hardware->addWidget(armCheck, 3, 3);
    hardware->addWidget(rfLockLabel, 4, 0, 1, 4);
    hardware->setColumnStretch(1, 1);
    hardware->setColumnStretch(3, 1);

    QGroupBox *signalBox = new QGroupBox(this);
    signalBox->setObjectName(QStringLiteral("txSignalBox"));
    QGridLayout *signal = new QGridLayout(signalBox);
    modulationCombo = new QComboBox(signalBox);
    const QVector<TxModulation> modes = {
        TxModulation::Am, TxModulation::Nfm, TxModulation::Wfm, TxModulation::Dsb,
        TxModulation::Usb, TxModulation::Lsb, TxModulation::Cw, TxModulation::Ft8,
        TxModulation::Ook, TxModulation::Ask, TxModulation::Fsk, TxModulation::Gfsk,
        TxModulation::Gmsk, TxModulation::Bpsk, TxModulation::Qpsk, TxModulation::Rtty,
        TxModulation::Dmr
    };
    for (TxModulation mode : modes) {
        modulationCombo->addItem(TransmitWaveformGenerator::modulationName(mode), static_cast<int>(mode));
    }
    sourceCombo = new QComboBox(signalBox);
    sourceCombo->addItem(QStringLiteral("Microphone"), QStringLiteral("microphone"));
    sourceCombo->addItem(QStringLiteral("Microphone PTT"), QStringLiteral("microphone-ptt"));
    sourceCombo->addItem(QStringLiteral("Text"), QStringLiteral("text"));
    sourceCombo->addItem(QStringLiteral("Manual CW"), QStringLiteral("manual-cw"));
    sourceCombo->addItem(QStringLiteral("Audio file"), QStringLiteral("audio-file"));
    sourceCombo->addItem(QStringLiteral("SSTV image"), QStringLiteral("image-sstv"));
    sourceCombo->addItem(QStringLiteral("Analog video frame"), QStringLiteral("image-atv"));
    audioInputCombo = new QComboBox(signalBox);
    audioRefreshButton = new QToolButton(signalBox);
    audioRefreshButton->setAutoRaise(true);
    audioRefreshButton->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
    microphoneLevelSlider = new QSlider(Qt::Horizontal, signalBox);
    microphoneLevelSlider->setRange(0, 130);
    microphoneLevelSlider->setValue(100);
    microphoneLevelSlider->setSingleStep(1);
    microphoneLevelSlider->setPageStep(5);
    microphoneLevelValueLabel = new QLabel(signalBox);
    microphoneLevelValueLabel->setMinimumWidth(88);
    microphoneLevelValueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    signalBandwidthCombo = new QComboBox(signalBox);
    signalBandwidthCombo->setEditable(true);
    signalBandwidthCombo->setInsertPolicy(QComboBox::NoInsert);
    signalBandwidthCombo->addItem(QStringLiteral("Auto / unlimited"), 0.0);
    signalBandwidthCombo->addItem(QStringLiteral("100 Hz - FT8"), 100.0);
    signalBandwidthCombo->addItem(QStringLiteral("250 Hz - narrow CW"), 250.0);
    signalBandwidthCombo->addItem(QStringLiteral("500 Hz - CW / RTTY"), 500.0);
    signalBandwidthCombo->addItem(QStringLiteral("1 kHz - wide CW"), 1000.0);
    signalBandwidthCombo->addItem(QStringLiteral("2.4 kHz - narrow data"), 2400.0);
    signalBandwidthCombo->addItem(QStringLiteral("2.7 kHz - SSB voice"), 2700.0);
    signalBandwidthCombo->addItem(QStringLiteral("5 kHz - packet data"), 5000.0);
    signalBandwidthCombo->addItem(QStringLiteral("6 kHz - AM voice"), 6000.0);
    signalBandwidthCombo->addItem(QStringLiteral("8.33 kHz"), 8330.0);
    signalBandwidthCombo->addItem(QStringLiteral("10 kHz - wide AM voice"), 10000.0);
    signalBandwidthCombo->addItem(QStringLiteral("12.5 kHz - NFM / SSTV"), 12500.0);
    signalBandwidthCombo->addItem(QStringLiteral("25 kHz - NFM / SSTV"), 25000.0);
    signalBandwidthCombo->addItem(QStringLiteral("50 kHz"), 50000.0);
    signalBandwidthCombo->addItem(QStringLiteral("100 kHz"), 100000.0);
    signalBandwidthCombo->addItem(QStringLiteral("200 kHz - WFM"), 200000.0);
    signalBandwidthCombo->addItem(QStringLiteral("500 kHz - narrow ATV"), 500000.0);
    signalBandwidthCombo->addItem(QStringLiteral("1 MHz - ATV"), 1000000.0);
    signalBandwidthCombo->addItem(QStringLiteral("1.5 MHz - ATV"), 1500000.0);
    deviationSpin = new QDoubleSpinBox(signalBox);
    deviationSpin->setRange(1.0, 1000000.0);
    deviationSpin->setDecimals(0);
    deviationSpin->setSingleStep(100.0);
    deviationSpin->setSuffix(QStringLiteral(" Hz"));
    toneSpin = new QDoubleSpinBox(signalBox);
    toneSpin->setRange(0.0, 1000000.0);
    toneSpin->setDecimals(1);
    toneSpin->setSingleStep(10.0);
    toneSpin->setSuffix(QStringLiteral(" Hz"));
    cwWpmSpin = new QSpinBox(signalBox);
    cwWpmSpin->setRange(5, 60);
    cwWpmSpin->setSuffix(QStringLiteral(" WPM"));
    symbolRateSpin = new QDoubleSpinBox(signalBox);
    symbolRateSpin->setRange(1.0, 1000000.0);
    symbolRateSpin->setDecimals(2);
    symbolRateSpin->setSingleStep(50.0);
    symbolRateSpin->setSuffix(QStringLiteral(" Bd"));
    gaussianBtSpin = new QDoubleSpinBox(signalBox);
    gaussianBtSpin->setRange(0.20, 1.00);
    gaussianBtSpin->setDecimals(2);
    gaussianBtSpin->setSingleStep(0.05);
    modulationDepthSpin = new QSpinBox(signalBox);
    modulationDepthSpin->setRange(0, 100);
    modulationDepthSpin->setSuffix(QStringLiteral(" %"));
    modulationDepthSpin->setValue(amDepthPercent);
    invertDataCheck = new QCheckBox(signalBox);
    ft8UtcCheck = new QCheckBox(signalBox);
    ft8SlotCombo = new QComboBox(signalBox);
    ft8SlotCombo->addItem(QString(), -1);
    ft8SlotCombo->addItem(QString(), 0);
    ft8SlotCombo->addItem(QString(), 1);
    ft8UtcCheck->setChecked(true);
    liveKeyButton = new QPushButton(signalBox);
    liveKeyButton->setEnabled(false);
    liveKeyButton->setAutoRepeat(false);
    liveKeyButton->setAutoDefault(false);
    liveKeyButton->setDefault(false);
    liveKeyButton->setFocusPolicy(Qt::StrongFocus);
    liveKeyButton->setMinimumHeight(42);
    modeLabel = new QLabel(signalBox);
    sourceLabel = new QLabel(signalBox);
    audioInputLabel = new QLabel(signalBox);
    microphoneLevelLabel = new QLabel(signalBox);
    deviationLabel = new QLabel(signalBox);
    signalBandwidthLabel = new QLabel(signalBox);
    toneLabel = new QLabel(signalBox);
    cwSpeedLabel = new QLabel(signalBox);
    symbolRateLabel = new QLabel(signalBox);
    gaussianBtLabel = new QLabel(signalBox);
    modulationDepthLabel = new QLabel(signalBox);
    signal->addWidget(basebandLabel, 0, 0);
    signal->addWidget(sampleRateCombo, 0, 1);
    signal->addWidget(levelLabel, 0, 2);
    signal->addWidget(levelSpin, 0, 3);
    signal->addWidget(modeLabel, 1, 0);
    signal->addWidget(modulationCombo, 1, 1);
    signal->addWidget(sourceLabel, 1, 2);
    signal->addWidget(sourceCombo, 1, 3);
    signal->addWidget(audioInputLabel, 2, 0);
    signal->addWidget(audioInputCombo, 2, 1, 1, 2);
    signal->addWidget(audioRefreshButton, 2, 3);
    signal->addWidget(microphoneLevelLabel, 3, 0);
    signal->addWidget(microphoneLevelSlider, 3, 1, 1, 2);
    signal->addWidget(microphoneLevelValueLabel, 3, 3);
    signal->addWidget(deviationLabel, 4, 0);
    signal->addWidget(deviationSpin, 4, 1);
    signal->addWidget(toneLabel, 4, 2);
    signal->addWidget(toneSpin, 4, 3);
    signal->addWidget(signalBandwidthLabel, 5, 0);
    signal->addWidget(signalBandwidthCombo, 5, 1, 1, 3);
    signal->addWidget(cwSpeedLabel, 6, 0);
    signal->addWidget(cwWpmSpin, 6, 1);
    signal->addWidget(ft8UtcCheck, 6, 2);
    signal->addWidget(ft8SlotCombo, 6, 3);
    signal->addWidget(symbolRateLabel, 7, 0);
    signal->addWidget(symbolRateSpin, 7, 1);
    signal->addWidget(gaussianBtLabel, 7, 2);
    signal->addWidget(gaussianBtSpin, 7, 3);
    signal->addWidget(modulationDepthLabel, 8, 0);
    signal->addWidget(modulationDepthSpin, 8, 1);
    signal->addWidget(invertDataCheck, 8, 2, 1, 2);
    signal->addWidget(liveKeyButton, 9, 0, 1, 4);
    signal->setColumnStretch(1, 1);
    signal->setColumnStretch(3, 1);

    dmrBox = new QGroupBox(this);
    dmrBox->setObjectName(QStringLiteral("txDmrBox"));
    QGridLayout *dmr = new QGridLayout(dmrBox);
    dmrColorCodeLabel = new QLabel(dmrBox);
    dmrColorCodeSpin = new QSpinBox(dmrBox);
    dmrColorCodeSpin->setRange(0, 15);
    dmrColorCodeSpin->setValue(1);
    dmrTimeslotLabel = new QLabel(dmrBox);
    dmrTimeslotCombo = new QComboBox(dmrBox);
    dmrTimeslotCombo->addItem(QStringLiteral("1"), 1);
    dmrTimeslotCombo->addItem(QStringLiteral("2"), 2);
    dmrSourceIdLabel = new QLabel(dmrBox);
    dmrSourceIdSpin = new QSpinBox(dmrBox);
    dmrSourceIdSpin->setRange(1, 0xFFFFFF);
    dmrSourceIdSpin->setValue(1);
    dmrDestinationIdLabel = new QLabel(dmrBox);
    dmrDestinationIdSpin = new QSpinBox(dmrBox);
    dmrDestinationIdSpin->setRange(1, 0xFFFFFF);
    dmrDestinationIdSpin->setValue(9);
    dmrCallTypeLabel = new QLabel(dmrBox);
    dmrCallTypeCombo = new QComboBox(dmrBox);
    dmrCallTypeCombo->addItem(QStringLiteral("Group"), false);
    dmrCallTypeCombo->addItem(QStringLiteral("Private"), true);
    dmrPrivacyLabel = new QLabel(dmrBox);
    dmrPrivacyCombo = new QComboBox(dmrBox);
    dmrPrivacyCombo->addItem(QStringLiteral("None"), DMR_PRIVACY_NONE);
    dmrPrivacyCombo->addItem(QStringLiteral("ARC4 (40-bit)"), DMR_PRIVACY_ARC4);
    dmrPrivacyCombo->addItem(QStringLiteral("AES-256"), DMR_PRIVACY_AES256);
    dmrPrivacyKeyIdLabel = new QLabel(dmrBox);
    dmrPrivacyKeyIdSpin = new QSpinBox(dmrBox);
    dmrPrivacyKeyIdSpin->setRange(0, 255);
    dmrPrivacyKeyLabel = new QLabel(dmrBox);
    dmrPrivacyKeyEdit = new QLineEdit(dmrBox);
    dmrPrivacyKeyEdit->setMaxLength(64);
    dmrPrivacyKeyEdit->setEchoMode(QLineEdit::PasswordEchoOnEdit);
    dmr->addWidget(dmrColorCodeLabel, 0, 0);
    dmr->addWidget(dmrColorCodeSpin, 0, 1);
    dmr->addWidget(dmrTimeslotLabel, 0, 2);
    dmr->addWidget(dmrTimeslotCombo, 0, 3);
    dmr->addWidget(dmrCallTypeLabel, 0, 4);
    dmr->addWidget(dmrCallTypeCombo, 0, 5);
    dmr->addWidget(dmrSourceIdLabel, 1, 0);
    dmr->addWidget(dmrSourceIdSpin, 1, 1, 1, 2);
    dmr->addWidget(dmrDestinationIdLabel, 1, 3);
    dmr->addWidget(dmrDestinationIdSpin, 1, 4, 1, 2);
    dmr->addWidget(dmrPrivacyLabel, 2, 0);
    dmr->addWidget(dmrPrivacyCombo, 2, 1, 1, 2);
    dmr->addWidget(dmrPrivacyKeyIdLabel, 2, 3);
    dmr->addWidget(dmrPrivacyKeyIdSpin, 2, 4, 1, 2);
    dmr->addWidget(dmrPrivacyKeyLabel, 3, 0);
    dmr->addWidget(dmrPrivacyKeyEdit, 3, 1, 1, 5);
    dmr->setColumnStretch(1, 1);
    dmr->setColumnStretch(4, 1);
    dmrBox->setVisible(false);

    QGroupBox *messageBox = new QGroupBox(this);
    messageBox->setObjectName(QStringLiteral("txMessageBox"));
    QVBoxLayout *messageLayout = new QVBoxLayout(messageBox);
    messageEdit = new QPlainTextEdit(messageBox);
    messageEdit->setPlaceholderText(QStringLiteral("CQ TEST KN34 / HELLO WORLD"));
    messageEdit->setMaximumBlockCount(20);
    messageLayout->addWidget(messageEdit);

    QGroupBox *mediaBox = new QGroupBox(this);
    mediaBox->setObjectName(QStringLiteral("txMediaBox"));
    QGridLayout *mediaLayout = new QGridLayout(mediaBox);
    mediaPathLabel = new QLabel(mediaBox);
    mediaPathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    mediaPathLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    mediaBrowseButton = new QPushButton(mediaBox);
    mediaEditButton = new QPushButton(mediaBox);
    mediaModeLabel = new QLabel(mediaBox);
    mediaModeCombo = new QComboBox(mediaBox);
    mediaModeCombo->addItem(QStringLiteral("Robot 36"), QStringLiteral("robot36"));
    mediaModeCombo->addItem(QStringLiteral("Martin M1"), QStringLiteral("martin-m1"));
    mediaModeCombo->addItem(QStringLiteral("Martin M2"), QStringLiteral("martin-m2"));
    mediaModeCombo->addItem(QStringLiteral("Analog TV / AM"), QStringLiteral("atv-am"));
    mediaModeCombo->addItem(QStringLiteral("Analog TV / FM"), QStringLiteral("atv-fm"));
    mediaPlayButton = new QPushButton(mediaBox);
    mediaPauseButton = new QPushButton(mediaBox);
    mediaStopButton = new QPushButton(mediaBox);
    mediaLayout->addWidget(mediaPathLabel, 0, 0, 1, 3);
    mediaLayout->addWidget(mediaBrowseButton, 0, 3);
    mediaLayout->addWidget(mediaEditButton, 0, 4);
    mediaLayout->addWidget(mediaModeLabel, 1, 0);
    mediaLayout->addWidget(mediaModeCombo, 1, 1);
    mediaLayout->addWidget(mediaPlayButton, 1, 2);
    mediaLayout->addWidget(mediaPauseButton, 1, 3);
    mediaLayout->addWidget(mediaStopButton, 1, 4);
    mediaLayout->setColumnStretch(1, 1);

    preview = new TxWaveformPreviewWidget(this);
    progressBar = new QProgressBar(this);
    progressBar->setRange(0, 1000);
    progressBar->setValue(0);
    statusLabel = new QLabel(this);
    statusLabel->setWordWrap(true);
    statsLabel = new QLabel(this);
    statsLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

    QHBoxLayout *actions = new QHBoxLayout();
    startButton = new QPushButton(this);
    stopButton = new QPushButton(this);
    exportButton = new QPushButton(this);
    signalLabButton = new QPushButton(this);
    stopButton->setEnabled(false);
    exportButton->setEnabled(false);
    actions->addWidget(startButton);
    actions->addWidget(stopButton);
    actions->addWidget(signalLabButton);
    actions->addStretch(1);
    actions->addWidget(exportButton);

    root->addWidget(hardwareBox);
    root->addWidget(signalBox);
    root->addWidget(dmrBox);
    root->addWidget(messageBox);
    root->addWidget(mediaBox);
    root->addWidget(preview, 1);
    root->addWidget(progressBar);
    root->addWidget(statusLabel);
    root->addWidget(statsLabel);
    root->addLayout(actions);

    progressTimer = new QTimer(this);
    ft8StartTimer = new QTimer(this);
    ft8StartTimer->setSingleShot(true);
    ft8StartTimer->setTimerType(Qt::PreciseTimer);
    progressTimer->setInterval(40);
    sourceTimer = new QTimer(this);
    sourceTimer->setInterval(10);
    sourceTimer->setTimerType(Qt::PreciseTimer);
    microphoneLevelApplyTimer = new QTimer(this);
    microphoneLevelApplyTimer->setSingleShot(true);
    microphoneLevelApplyTimer->setInterval(120);
    connect(progressTimer, &QTimer::timeout, this, [this]() { updateProgress(); });
    connect(ft8StartTimer, &QTimer::timeout, this, [this]() { beginTextWaveform(); });
    connect(sourceTimer, &QTimer::timeout, this, [this]() { processLiveSource(); });
    connect(microphoneLevelApplyTimer, &QTimer::timeout,
            this, [this]() { applyMicrophoneLevel(); });
    connect(modulationCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this]() {
                applyModulationPreset(selectedModulation());
                updateModeUi();
            });
    connect(backendCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this]() {
                armCheck->setChecked(false);
                updateModeUi();
            });
    connect(armCheck, &QCheckBox::toggled, this, [this]() {
        if (!(activeBackend && activeBackend->isRunning())) updateTexts();
    });
    connect(sourceCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this]() { updateModeUi(); });
    connect(dmrPrivacyCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this]() { updateModeUi(); });
    connect(ft8UtcCheck, &QCheckBox::toggled, this, [this]() { updateModeUi(); });
    connect(audioRefreshButton, &QToolButton::clicked, this, [this]() {
        populateAudioInputs();
        updateModeUi();
        const bool found = !audioInputCombo->currentData().toString().isEmpty();
        setStatus(found
                      ? text(QStringLiteral("Microphone list refreshed: %1 device(s)"),
                             QString::fromUtf8(u8"Список мікрофонів оновлено: %1 пристр."))
                            .arg(audioInputCombo->count())
                      : text(QStringLiteral("No microphone input is currently available"),
                             QString::fromUtf8(u8"Зараз немає доступного мікрофонного входу")),
                  !found);
    });
    connect(audioInputCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this]() {
                if (isVisible()) microphoneLevelApplyTimer->start();
            });
    connect(microphoneLevelSlider, &QSlider::valueChanged, this, [this]() {
        updateMicrophoneLevelText();
        const int boostDb = (std::max)(0, microphoneLevelSlider->value() - 100);
        microphoneSoftwareGain = static_cast<float>(std::pow(10.0, boostDb / 20.0));
#ifdef FOBOSAPP_HAS_QT_MULTIMEDIA
        if (audioInput) {
            audioInput->setVolume((std::min)(100, microphoneLevelSlider->value()) / 100.0);
        }
#endif
        if (isVisible()) microphoneLevelApplyTimer->start();
    });
    connect(signalBandwidthCombo, &QComboBox::currentTextChanged,
            this, [this]() { applyLiveSignalShape(); });
    connect(deviationSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, [this]() { applyLiveSignalShape(); });
    connect(symbolRateSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, [this]() { applyLiveSignalShape(); });
    connect(gaussianBtSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, [this]() { applyLiveSignalShape(); });
    connect(modulationDepthSpin, qOverload<int>(&QSpinBox::valueChanged),
            this, [this](int value) {
                if (selectedModulation() == TxModulation::Am) {
                    amDepthPercent = value;
                } else if (selectedModulation() == TxModulation::Ask) {
                    askDepthPercent = value;
                }
                applyLiveSignalShape();
            });
    connect(invertDataCheck, &QCheckBox::toggled,
            this, [this]() { applyLiveSignalShape(); });
    connect(liveKeyButton, &QPushButton::pressed, this, [this]() {
        if (activeBackend && activeBackend->isRunning()) setLiveKey(true);
    });
    connect(liveKeyButton, &QPushButton::released, this, [this]() {
        if (activeBackend && activeBackend->isRunning()) {
            setLiveKey(false);
        } else {
            startTransmission();
        }
        if (activeBackend && activeBackend->isRunning()) {
            liveKeyButton->setFocus(Qt::MouseFocusReason);
            updateTexts();
        }
    });
    connect(startButton, &QPushButton::clicked, this, [this]() { startTransmission(); });
    connect(stopButton, &QPushButton::clicked, this, [this]() {
        stopTransmission(text(QStringLiteral("Stopped by user"), QString::fromUtf8(u8"Зупинено користувачем")));
    });
    connect(exportButton, &QPushButton::clicked, this, [this]() { exportIq(); });
    connect(signalLabButton, &QPushButton::clicked, this, [this]() {
        if (!signalLabDialog) {
            signalLabDialog = new SignalLabDialog(
                language,
                [this](const SignalLabGenerationResult &result) {
                    startLaboratoryTransmission(result);
                },
                [this]() {
                    stopTransmission(text(QStringLiteral("Stopped by user"),
                                          QString::fromUtf8(u8"Зупинено користувачем")));
                },
                this);
        }
        signalLabDialog->setLanguage(language);
        signalLabDialog->setInitialSampleRate(configurationFromUi().sampleRate);
        signalLabDialog->show();
        signalLabDialog->raise();
        signalLabDialog->activateWindow();
    });
    connect(mediaBrowseButton, &QPushButton::clicked, this, [this]() { chooseMediaFile(); });
    connect(mediaEditButton, &QPushButton::clicked, this, [this]() { editMediaImage(); });
    connect(mediaModeCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this]() {
        preparedMediaImage = QImage();
    });
    connect(mediaPlayButton, &QPushButton::clicked, this, [this]() {
        if (activeBackend && activeBackend->isRunning()) {
            mediaPaused = false;
            sourceTimer->start();
        } else {
            startTransmission();
        }
    });
    connect(mediaPauseButton, &QPushButton::clicked, this, [this]() {
        mediaPaused = true;
        sourceTimer->stop();
    });
    connect(mediaStopButton, &QPushButton::clicked, this, [this]() {
        stopTransmission(text(QStringLiteral("Media transmission stopped"),
                              QString::fromUtf8(u8"Передачу медіа зупинено")));
    });
}

void TransmitDialog::populateAudioInputs() {
    const QString previousDevice = audioInputCombo->currentData().toString();
    audioInputCombo->clear();
    audioInputCombo->setEnabled(true);
#ifdef FOBOSAPP_HAS_QT_MULTIMEDIA
    QList<QAudioDeviceInfo> devices = QAudioDeviceInfo::availableDevices(QAudio::AudioInput);
    const QAudioDeviceInfo defaultDevice = QAudioDeviceInfo::defaultInputDevice();
    if (!defaultDevice.isNull()) {
        bool alreadyListed = false;
        for (const QAudioDeviceInfo &device : devices) {
            if (device.deviceName() == defaultDevice.deviceName()) {
                alreadyListed = true;
                break;
            }
        }
        if (!alreadyListed) devices.prepend(defaultDevice);
    }
    QStringList names;
    for (const QAudioDeviceInfo &device : devices) {
        const QString name = device.deviceName().trimmed();
        if (name.isEmpty() || names.contains(name)) continue;
        names.append(name);
        audioInputCombo->addItem(name, name);
    }
    int selectedIndex = previousDevice.isEmpty() ? -1 : audioInputCombo->findData(previousDevice);
    if (selectedIndex < 0 && !defaultDevice.isNull()) {
        selectedIndex = audioInputCombo->findData(defaultDevice.deviceName());
    }
    if (selectedIndex >= 0) audioInputCombo->setCurrentIndex(selectedIndex);
    if (audioInputCombo->count() == 0) {
        audioInputCombo->addItem(
            text(QStringLiteral("No microphone devices"),
                 QString::fromUtf8(u8"Мікрофонні пристрої не знайдено")),
            QString());
        audioInputCombo->setEnabled(false);
    }
    qInfo().noquote() << "[TX audio] microphone inputs" << names.size()
                      << (names.isEmpty() ? QStringLiteral("none") : names.join(QStringLiteral(" | ")));
#else
    audioInputCombo->addItem(
        text(QStringLiteral("Qt Multimedia is unavailable"),
             QString::fromUtf8(u8"Qt Multimedia недоступна")),
        QString());
    audioInputCombo->setEnabled(false);
    qWarning().noquote() << "[TX audio] Qt Multimedia microphone support is unavailable";
#endif
}

void TransmitDialog::updateMicrophoneLevelText() {
    if (!microphoneLevelSlider || !microphoneLevelValueLabel) return;
    const int value = microphoneLevelSlider->value();
    microphoneLevelValueLabel->setText(
        value <= 100
            ? QStringLiteral("%1 %").arg(value)
            : QStringLiteral("100 % / +%1 dB").arg(value - 100));
}

void TransmitDialog::applyMicrophoneLevel() {
    if (!microphoneLevelSlider) return;
    const int value = microphoneLevelSlider->value();
    const float systemLevel = (std::min)(100, value) / 100.0f;
    const int boostDb = (std::max)(0, value - 100);
    microphoneSoftwareGain = static_cast<float>(std::pow(10.0, boostDb / 20.0));
#ifdef FOBOSAPP_HAS_QT_MULTIMEDIA
    if (audioInput) audioInput->setVolume(systemLevel);
#endif
#ifdef _WIN32
    const QString requestedDevice = audioInputCombo->currentData().toString();
    if (!requestedDevice.isEmpty() && windowsMicrophoneCapture &&
        windowsMicrophoneCapture->isRunning()) {
        QString error;
        if (!WindowsMicrophoneCapture::setDeviceLevel(requestedDevice, systemLevel,
                                                       &error, nullptr) &&
            !error.isEmpty()) {
            qWarning().noquote() << "[TX audio] microphone level change failed" << error;
        }
    }
#endif
}

void TransmitDialog::populateHackRfDevices() {
    const int previousDevice = deviceCombo->currentData().toInt();
    deviceCombo->clear();
    HackRfRuntimeStatus status;
    const QVector<HackRfDeviceInfo> devices = enumerateHackRfDevices(&status);
    for (const HackRfDeviceInfo &device : devices) {
        QString label = device.label;
        if (hackRfReceiverUseChecker && hackRfReceiverUseChecker(device.nativeIndex)) {
            label += text(QStringLiteral(" (active RX)"),
                          QString::fromUtf8(u8" (активний RX)"));
        }
        deviceCombo->addItem(label, device.nativeIndex);
    }
    const bool available = status.libraryAvailable && !devices.isEmpty();
    if (!available) {
        deviceCombo->addItem(text(QStringLiteral("No HackRF detected"),
                                  QString::fromUtf8(u8"HackRF не знайдено")), -1);
    }
    if (auto *model = qobject_cast<QStandardItemModel *>(backendCombo->model())) {
        if (QStandardItem *item = model->item(1)) item->setEnabled(available);
    }
    const int previousIndex = deviceCombo->findData(previousDevice);
    if (previousIndex >= 0) {
        deviceCombo->setCurrentIndex(previousIndex);
    }
    if (!available && hackRfSelected()) {
        backendCombo->setCurrentIndex(0);
    }
}

void TransmitDialog::setHackRfReceiverUseChecker(HackRfReceiverUseChecker checker) {
    hackRfReceiverUseChecker = std::move(checker);
    populateHackRfDevices();
    updateModeUi();
}

void TransmitDialog::setLanguage(const QString &nextLanguage) {
    language = nextLanguage;
    if (signalLabDialog) signalLabDialog->setLanguage(language);
    updateTexts();
    updateModeUi();
}

void TransmitDialog::prepareFt8Message(const QString &message,
                                       double dialFrequencyHz,
                                       double audioFrequencyHz,
                                       int slotParity) {
    const int modulationIndex = modulationCombo->findData(static_cast<int>(TxModulation::Ft8));
    if (modulationIndex >= 0) modulationCombo->setCurrentIndex(modulationIndex);
    const int sourceIndex = sourceCombo->findData(QStringLiteral("text"));
    if (sourceIndex >= 0) sourceCombo->setCurrentIndex(sourceIndex);
    messageEdit->setPlainText(message.simplified().toUpper());
    if (dialFrequencyHz > 0.0) frequencySpin->setValue(dialFrequencyHz / 1e6);
    if (audioFrequencyHz > 0.0) toneSpin->setValue(audioFrequencyHz);
    ft8UtcCheck->setChecked(true);
    const int slotIndex = ft8SlotCombo->findData(slotParity);
    ft8SlotCombo->setCurrentIndex(slotIndex >= 0 ? slotIndex : 0);
    updateModeUi();
    setStatus(text(QStringLiteral("FT8 message prepared; verify RF settings and press Start."),
                   QString::fromUtf8(u8"Повідомлення FT8 підготовлено; перевірте RF-налаштування й натисніть Старт.")));
}
bool TransmitDialog::startPreparedFt8Automatically(QString *error) {
    auto fail = [this, error](const QString &message) {
        if (error) *error = message;
        setStatus(message, true);
        return false;
    };
    if (!hackRfSelected()) {
        return fail(text(QStringLiteral("Automatic FT8 requires an RF-capable HackRF output; select HackRF native in TX setup."),
                         QString::fromUtf8(u8"Для автоматичного FT8 потрібен RF-вихід HackRF; у налаштуваннях TX оберіть HackRF native.")));
    }
    if (activeBackend && activeBackend->isRunning()) {
        return fail(text(QStringLiteral("The transmitter is busy with another transmission."),
                         QString::fromUtf8(u8"Передавач зайнятий іншою передачею.")));
    }
    if (!armCheck->isChecked()) {
        return fail(text(QStringLiteral("RF output is not armed. Open TX setup, verify the antenna/load and arm RF output."),
                         QString::fromUtf8(u8"RF-вихід не озброєно. Відкрийте налаштування TX, перевірте антену/навантаження та озбройте RF-вихід.")));
    }
    if (deviceCombo->currentData().toInt() < 0) {
        return fail(text(QStringLiteral("No HackRF device is available for automatic FT8."),
                         QString::fromUtf8(u8"Для автоматичного FT8 немає доступного HackRF.")));
    }
    if (selectedHackRfBusyForReception()) {
        return fail(text(QStringLiteral("The selected HackRF is already receiving; select a different TX device."),
                         QString::fromUtf8(u8"Вибраний HackRF уже працює на прийом; оберіть інший TX-пристрій.")));
    }
    if (selectedModulation() != TxModulation::Ft8 ||
        sourceCombo->currentData().toString() != QStringLiteral("text")) {
        return fail(text(QStringLiteral("No prepared FT8 text message is available."),
                         QString::fromUtf8(u8"Немає підготовленого текстового FT8-повідомлення.")));
    }

    automaticFt8Active = true;
    startTransmission();
    const bool accepted = activeBackend && activeBackend->isRunning() &&
                          ((ft8StartTimer && ft8StartTimer->isActive()) ||
                           (progressTimer && progressTimer->isActive()));
    if (!accepted) {
        automaticFt8Active = false;
        const QString details = statusLabel ? statusLabel->text() : QString();
        return fail(details.isEmpty()
                        ? text(QStringLiteral("Automatic FT8 transmission could not be started."),
                               QString::fromUtf8(u8"Не вдалося запустити автоматичну FT8-передачу."))
                        : details);
    }
    if (error) error->clear();
    return true;
}
void TransmitDialog::cancelAutomaticFt8() {
    if (!automaticFt8Active) return;
    stopTransmission(text(QStringLiteral("Automatic FT8 transmission cancelled."),
                          QString::fromUtf8(u8"Автоматичну FT8-передачу скасовано.")));
}
void TransmitDialog::updateTexts() {
    setWindowTitle(text(QStringLiteral("Transmitter laboratory"), QString::fromUtf8(u8"Лабораторія передавача")));
    if (auto *box = findChild<QGroupBox *>(QStringLiteral("txHardwareBox")))
        box->setTitle(text(QStringLiteral("Device / backend and safety"), QString::fromUtf8(u8"Пристрій / бекенд і безпека")));
    if (auto *box = findChild<QGroupBox *>(QStringLiteral("txSignalBox")))
        box->setTitle(text(QStringLiteral("Software generator and modulation"), QString::fromUtf8(u8"Генератор програми і модуляція")));
    if (dmrBox)
        dmrBox->setTitle(text(QStringLiteral("DMR Tier II direct mode"),
                              QString::fromUtf8(u8"DMR Tier II, прямий режим")));
    if (auto *box = findChild<QGroupBox *>(QStringLiteral("txMessageBox")))
        box->setTitle(text(QStringLiteral("Text message"), QString::fromUtf8(u8"Текстове повідомлення")));
    if (auto *box = findChild<QGroupBox *>(QStringLiteral("txMediaBox")))
        box->setTitle(text(QStringLiteral("Media source"), QString::fromUtf8(u8"Медіаджерело")));
    backendLabel->setText(text(QStringLiteral("Output:"), QString::fromUtf8(u8"Вихід:")));
    deviceLabel->setText(text(QStringLiteral("Device:"), QString::fromUtf8(u8"Пристрій:")));
    rfLabel->setText(text(QStringLiteral("RF carrier:"), QString::fromUtf8(u8"Несуча RF:")));
    basebandLabel->setText(text(QStringLiteral("Generator sample rate:"),
                                QString::fromUtf8(u8"Частота семплів генератора:")));
    deviceRateLabel->setText(text(QStringLiteral("HackRF IQ rate:"),
                                  QString::fromUtf8(u8"Частота передавання IQ:")));
    bandwidthLabel->setText(text(QStringLiteral("HackRF RF filter:"),
                                 QString::fromUtf8(u8"Апаратний RF-фільтр:")));
    levelLabel->setText(text(QStringLiteral("IQ amplitude:"), QString::fromUtf8(u8"Амплітуда IQ:")));
    txGainLabel->setText(text(QStringLiteral("HackRF TX VGA:"), QString::fromUtf8(u8"HackRF TX VGA:")));
    watchdogLabel->setText(text(QStringLiteral("RF watchdog:"), QString::fromUtf8(u8"Таймер RF:")));
    modeLabel->setText(text(QStringLiteral("Mode:"), QString::fromUtf8(u8"Режим:")));
    sourceLabel->setText(text(QStringLiteral("Source:"), QString::fromUtf8(u8"Джерело:")));
    audioInputLabel->setText(text(QStringLiteral("Microphone input:"), QString::fromUtf8(u8"Мікрофонний вхід:")));
    microphoneLevelLabel->setText(
        text(QStringLiteral("Microphone level / boost:"),
             QString::fromUtf8(u8"Рівень / підсилення мікрофона:")));
    const QString microphoneLevelHelp = text(
        QStringLiteral("0-100% controls the selected Windows microphone endpoint. Above 100%, the endpoint remains at 100% and Obrii adds up to +30 dB of software gain. Software gain cannot restore samples that the driver returns as digital silence."),
        QString::fromUtf8(u8"0-100% керує вибраним мікрофонним входом Windows. Вище 100% системний рівень лишається 100%, а Obrii додає до +30 dB програмного підсилення. Програмне підсилення не відновить семпли, якщо драйвер повертає цифрову тишу."));
    microphoneLevelSlider->setToolTip(microphoneLevelHelp);
    microphoneLevelValueLabel->setToolTip(microphoneLevelHelp);
    updateMicrophoneLevelText();
    audioRefreshButton->setToolTip(text(QStringLiteral("Refresh microphone devices"),
                                        QString::fromUtf8(u8"Оновити список мікрофонів")));
    const TxModulation modulation = selectedModulation();
    const bool fskMode = modulation == TxModulation::Fsk ||
                         modulation == TxModulation::Gfsk ||
                         modulation == TxModulation::Gmsk ||
                         modulation == TxModulation::Rtty;
    deviationLabel->setText(
        fskMode
            ? text(QStringLiteral("FSK deviation (one side):"),
                   QString::fromUtf8(u8"Девіація FSK (в один бік):"))
            : text(QStringLiteral("FM deviation:"), QString::fromUtf8(u8"Девіація FM:")));
    signalBandwidthLabel->setText(text(QStringLiteral("TX signal bandwidth:"),
                                       QString::fromUtf8(u8"Смуга TX-сигналу:")));
    const auto setSignalBandwidthText = [this](double bandwidth, const QString &english,
                                                const QString &ukrainian) {
        const int index = signalBandwidthCombo->findData(bandwidth);
        if (index >= 0) signalBandwidthCombo->setItemText(index, text(english, ukrainian));
    };
    setSignalBandwidthText(0.0, QStringLiteral("Auto / unlimited"),
                           QString::fromUtf8(u8"Авто / без обмеження"));
    setSignalBandwidthText(100.0, QStringLiteral("100 Hz - FT8"), QString::fromUtf8(u8"100 Гц - FT8"));
    setSignalBandwidthText(250.0, QStringLiteral("250 Hz - narrow CW"), QString::fromUtf8(u8"250 Гц - вузький CW"));
    setSignalBandwidthText(500.0, QStringLiteral("500 Hz - CW / RTTY"), QString::fromUtf8(u8"500 Гц - CW / RTTY"));
    setSignalBandwidthText(1000.0, QStringLiteral("1 kHz - wide CW"), QString::fromUtf8(u8"1 кГц - широкий CW"));
    setSignalBandwidthText(2400.0, QStringLiteral("2.4 kHz - narrow data"), QString::fromUtf8(u8"2,4 кГц - вузькі дані"));
    setSignalBandwidthText(2700.0, QStringLiteral("2.7 kHz - SSB voice"), QString::fromUtf8(u8"2,7 кГц - голос SSB"));
    setSignalBandwidthText(5000.0, QStringLiteral("5 kHz - packet data"), QString::fromUtf8(u8"5 кГц - пакетні дані"));
    setSignalBandwidthText(6000.0, QStringLiteral("6 kHz - AM voice"), QString::fromUtf8(u8"6 кГц - голос AM"));
    setSignalBandwidthText(8330.0, QStringLiteral("8.33 kHz - channel"), QString::fromUtf8(u8"8,33 кГц - канал"));
    setSignalBandwidthText(10000.0, QStringLiteral("10 kHz - wide AM voice"), QString::fromUtf8(u8"10 кГц - широкий голос AM"));
    setSignalBandwidthText(12500.0, QStringLiteral("12.5 kHz - NFM / SSTV"), QString::fromUtf8(u8"12,5 кГц - NFM / SSTV"));
    setSignalBandwidthText(25000.0, QStringLiteral("25 kHz - NFM / SSTV"), QString::fromUtf8(u8"25 кГц - NFM / SSTV"));
    setSignalBandwidthText(50000.0, QStringLiteral("50 kHz - wide FM"), QString::fromUtf8(u8"50 кГц - широка FM"));
    setSignalBandwidthText(100000.0, QStringLiteral("100 kHz - wide FM"), QString::fromUtf8(u8"100 кГц - широка FM"));
    setSignalBandwidthText(200000.0, QStringLiteral("200 kHz - WFM"), QString::fromUtf8(u8"200 кГц - WFM"));
    setSignalBandwidthText(500000.0, QStringLiteral("500 kHz - narrow ATV"), QString::fromUtf8(u8"500 кГц - вузьке ATV"));
    setSignalBandwidthText(1000000.0, QStringLiteral("1 MHz - ATV"), QString::fromUtf8(u8"1 МГц - ATV"));
    setSignalBandwidthText(1500000.0, QStringLiteral("1.5 MHz - ATV"), QString::fromUtf8(u8"1,5 МГц - ATV"));
    signalBandwidthCombo->setToolTip(text(
        QStringLiteral("Full two-sided occupied IQ bandwidth. Type Hz, kHz, MHz or GHz (e.g. 12.5 kHz), or select a preset. The occupied band must fit the generator and hardware; HackRF RF filter limits are separate."),
        QString::fromUtf8(u8"Повна двостороння зайнята смуга IQ. Введіть Hz, kHz, MHz чи GHz (наприклад 12.5 kHz) або оберіть пресет. Смуга має вміщуватися в генератор і апаратуру; RF-фільтр HackRF має окремі обмеження.")));
    toneLabel->setText(
        TransmitWaveformGenerator::isDigitalTextMode(modulation)
            ? text(QStringLiteral("Carrier / center offset:"),
                   QString::fromUtf8(u8"Зсув несучої / центра:"))
            : text(QStringLiteral("CW / FT8 tone:"), QString::fromUtf8(u8"Тон CW / FT8:")));
    basebandLabel->setToolTip(text(
        QStringLiteral("Complex IQ generation rate before hardware resampling. Type ksps, Msps or Gsps, e.g. 48 ksps. Signal Lab supports up to 1 Gsps; live/text generation up to 20 Msps. Each hardware backend has independent limits."),
        QString::fromUtf8(u8"Дискретизація комплексного IQ до апаратної передискретизації. Введіть ksps, Msps чи Gsps, наприклад 48 ksps. Лабораторія сигналів: до 1 Gsps; голос/текст: до 20 Msps. Апаратні бекенди мають власні обмеження.")));
    deviceRateLabel->setToolTip(text(
        QStringLiteral("Actual IQ sample rate sent to the HackRF DAC. It may be higher than the generated signal rate."),
        QString::fromUtf8(u8"Фактична частота IQ, що подається на DAC HackRF. Вона може бути вищою за частоту формування сигналу.")));
    bandwidthLabel->setToolTip(text(
        QStringLiteral("Hardware HackRF baseband filter. Keep it wider than the transmitted signal bandwidth."),
        QString::fromUtf8(u8"Апаратний фільтр HackRF. Його смуга має бути ширшою за смугу переданого сигналу.")));
    cwSpeedLabel->setText(text(QStringLiteral("CW speed:"), QString::fromUtf8(u8"Швидкість CW:")));
    symbolRateLabel->setText(text(QStringLiteral("Symbol rate:"),
                                  QString::fromUtf8(u8"Символьна швидкість:")));
    gaussianBtLabel->setText(text(QStringLiteral("Gaussian BT:"),
                                  QString::fromUtf8(u8"Gaussian BT:")));
    modulationDepthLabel->setText(
        text(QStringLiteral("Modulation depth:"),
             QString::fromUtf8(u8"Глибина модуляції:")));
    const QString modulationDepthHelp = text(
        QStringLiteral("AM: carrier amplitude modulation index. ASK: difference between mark and space amplitudes. FM uses deviation instead; this control does not change IQ output amplitude."),
        QString::fromUtf8(u8"AM: індекс амплітудної модуляції несучої. ASK: різниця амплітуд між mark і space. Для FM використовується девіація; цей регулятор не змінює вихідну амплітуду IQ."));
    modulationDepthLabel->setToolTip(modulationDepthHelp);
    modulationDepthSpin->setToolTip(modulationDepthHelp);
    dmrColorCodeLabel->setText(text(QStringLiteral("Color Code:"), QString::fromUtf8(u8"Color Code:")));
    dmrTimeslotLabel->setText(text(QStringLiteral("Time slot:"), QString::fromUtf8(u8"Таймслот:")));
    dmrSourceIdLabel->setText(text(QStringLiteral("Source ID:"), QString::fromUtf8(u8"ID джерела:")));
    dmrDestinationIdLabel->setText(text(QStringLiteral("Destination ID:"), QString::fromUtf8(u8"ID призначення:")));
    dmrCallTypeLabel->setText(text(QStringLiteral("Call type:"), QString::fromUtf8(u8"Тип виклику:")));
    dmrCallTypeCombo->setItemText(0, text(QStringLiteral("Group"), QString::fromUtf8(u8"Груповий")));
    dmrCallTypeCombo->setItemText(1, text(QStringLiteral("Private"), QString::fromUtf8(u8"Приватний")));
    dmrPrivacyLabel->setText(text(QStringLiteral("Privacy:"), QString::fromUtf8(u8"Шифрування:")));
    dmrPrivacyCombo->setItemText(0, text(QStringLiteral("None"), QString::fromUtf8(u8"Вимкнено")));
    dmrPrivacyKeyIdLabel->setText(QStringLiteral("KID:"));
    dmrPrivacyKeyLabel->setText(text(QStringLiteral("Key (hex):"), QString::fromUtf8(u8"Ключ (hex):")));
    invertDataCheck->setText(text(QStringLiteral("Invert data polarity"),
                                  QString::fromUtf8(u8"Інвертувати полярність даних")));
    symbolRateSpin->setToolTip(text(
        QStringLiteral("Symbols per second. Raw packet modes use an Obrii laboratory frame with preamble, sync, UTF-8 length and CRC-16."),
        QString::fromUtf8(u8"Символів за секунду. Сирі пакетні режими використовують лабораторний кадр Obrii з преамбулою, sync, довжиною UTF-8 та CRC-16.")));
    gaussianBtSpin->setToolTip(text(
        QStringLiteral("Gaussian bandwidth-time product for GFSK/GMSK shaping."),
        QString::fromUtf8(u8"Добуток смуги на час Gaussian-фільтра для формування GFSK/GMSK.")));
    backendCombo->setItemText(0, text(QStringLiteral("Simulator / IQ file"),
                                      QString::fromUtf8(u8"Симулятор / файл IQ")));
    backendCombo->setItemText(1, QStringLiteral("HackRF native"));
    const auto setSourceText = [this](const QString &id, const QString &english, const QString &ukrainian) {
        const int index = sourceCombo->findData(id);
        if (index >= 0) sourceCombo->setItemText(index, text(english, ukrainian));
    };
    setSourceText(QStringLiteral("microphone"), QStringLiteral("Microphone"), QString::fromUtf8(u8"Мікрофон"));
    setSourceText(QStringLiteral("microphone-ptt"), QStringLiteral("Microphone PTT"),
                  QString::fromUtf8(u8"Мікрофон PTT"));
    setSourceText(QStringLiteral("text"), QStringLiteral("Text"), QString::fromUtf8(u8"Текст"));
    setSourceText(QStringLiteral("manual-cw"), QStringLiteral("Manual CW key"),
                  QString::fromUtf8(u8"Ручний ключ CW"));
    setSourceText(QStringLiteral("audio-file"), QStringLiteral("Audio file"), QString::fromUtf8(u8"Аудіофайл"));
    setSourceText(QStringLiteral("image-sstv"), QStringLiteral("SSTV image"), QString::fromUtf8(u8"Зображення SSTV"));
    setSourceText(QStringLiteral("image-atv"), QStringLiteral("Analog video test frame"),
                  QString::fromUtf8(u8"Тестовий кадр аналогового відео"));
    mediaBrowseButton->setText(text(QStringLiteral("Browse..."), QString::fromUtf8(u8"Обрати...")));
    mediaEditButton->setText(text(QStringLiteral("Prepare image..."),
                                  QString::fromUtf8(u8"Підготувати фото...")));
    mediaPlayButton->setText(text(QStringLiteral("Play / transmit"), QString::fromUtf8(u8"Відтворити / передати")));
    mediaPauseButton->setText(text(QStringLiteral("Pause"), QString::fromUtf8(u8"Пауза")));
    mediaStopButton->setText(text(QStringLiteral("Stop media"), QString::fromUtf8(u8"Стоп медіа")));
    if (!liveKeyPressed) {
        const bool liveReady = activeBackend && activeBackend->isRunning();
        liveKeyButton->setText(
            liveReady
                ? text(QStringLiteral("PTT / CW KEY — hold Space"),
                       QString::fromUtf8(u8"PTT / КЛЮЧ CW — утримуйте Пробіл"))
                : text(QStringLiteral("Start transmission"),
                       QString::fromUtf8(u8"Почати передачу")));
        liveKeyButton->setStyleSheet(
            liveReady
                ? QStringLiteral("background:#218c4c;color:white;font-weight:600;")
                : QStringLiteral("background:#6b7078;color:white;font-weight:600;"));
    }
    liveKeyButton->setToolTip(
        text(QStringLiteral("When gray, click once to start and arm transmission. When green, hold Space "
                            "or hold this button to transmit; release to stop. F12 works anywhere in this window."),
             QString::fromUtf8(u8"Коли кнопка сіра, клацніть один раз для запуску й підготовки передачі. "
                               u8"Коли зелена, утримуйте Пробіл або саму кнопку для передачі; відпустіть "
                               u8"для зупинки. F12 працює в усьому вікні.")));
    mediaModeLabel->setText(text(QStringLiteral("Image/video mode:"), QString::fromUtf8(u8"Режим зображення/відео:")));
    mediaPathLabel->setText(mediaPath.isEmpty()
                                ? text(QStringLiteral("No media file selected"),
                                       QString::fromUtf8(u8"Медіафайл не обрано"))
                                : QFileInfo(mediaPath).fileName());
    startButton->setText(hackRfSelected()
                             ? text(QStringLiteral("Start RF transmission"), QString::fromUtf8(u8"Почати RF-передачу"))
                             : text(QStringLiteral("Start simulation"), QString::fromUtf8(u8"Почати симуляцію")));
    stopButton->setText(text(QStringLiteral("Stop"), QString::fromUtf8(u8"Зупинити")));
    exportButton->setText(text(QStringLiteral("Export IQ..."), QString::fromUtf8(u8"Експорт IQ...")));
    signalLabButton->setText(text(QStringLiteral("Signal lab..."),
                                  QString::fromUtf8(u8"Лабораторія сигналів...")));
    ft8UtcCheck->setText(text(QStringLiteral("Align FT8 to UTC 15 s slot"),
                              QString::fromUtf8(u8"Прив'язати FT8 до 15-секундного UTC-слота")));
    ft8SlotCombo->setItemText(0, text(QStringLiteral("Next slot"), QString::fromUtf8(u8"Наступний слот")));
    ft8SlotCombo->setItemText(1, text(QStringLiteral("Even slot"), QString::fromUtf8(u8"Парний слот")));
    ft8SlotCombo->setItemText(2, text(QStringLiteral("Odd slot"), QString::fromUtf8(u8"Непарний слот")));
    rfAmpCheck->setText(text(QStringLiteral("RF amp (+~11 dB)"),
                             QString::fromUtf8(u8"RF-підсилювач (+~11 дБ)")));
    armCheck->setText(text(QStringLiteral("ARM RF output"),
                           QString::fromUtf8(u8"ОЗБРОЇТИ RF-вихід")));
    rfLockLabel->setText(hackRfSelected()
        ? text(QStringLiteral("Connect a suitable antenna or dummy load. Verify frequency, local rules and gain. Stop reception on this HackRF before transmitting."),
               QString::fromUtf8(u8"Підключіть відповідну антену або еквівалент навантаження. Перевірте частоту, місцеві правила та підсилення. Перед передачею зупиніть прийом на цьому HackRF."))
        : text(QStringLiteral("Simulation and IQ export do not emit RF."),
               QString::fromUtf8(u8"Симуляція та експорт IQ не випромінюють RF.")));
    if (!(activeBackend && activeBackend->isRunning())) {
        setStatus(hackRfSelected()
                      ? (armCheck->isChecked()
                             ? text(QStringLiteral("Ready; RF output is armed for this dialog session"),
                                    QString::fromUtf8(u8"Готово; RF-вихід озброєно на цю сесію вікна"))
                             : text(QStringLiteral("Ready; RF output is disarmed"),
                                    QString::fromUtf8(u8"Готово; RF-вихід не озброєний")))
                      : text(QStringLiteral("Ready; no RF can be emitted"),
                             QString::fromUtf8(u8"Готово; випромінювання RF неможливе")));
    }
}

bool TransmitDialog::hackRfSelected() const {
    return backendCombo && backendCombo->currentData().toString() == QStringLiteral("hackrf-native");
}

bool TransmitDialog::selectedHackRfBusyForReception() const {
    return hackRfSelected() && deviceCombo && hackRfReceiverUseChecker &&
           hackRfReceiverUseChecker(deviceCombo->currentData().toInt());
}

TransmitterBackend *TransmitDialog::selectedBackend() {
    return hackRfSelected() ? static_cast<TransmitterBackend *>(&hackRf)
                            : static_cast<TransmitterBackend *>(&simulator);
}

TxModulation TransmitDialog::selectedModulation() const {
    return static_cast<TxModulation>(modulationCombo->currentData().toInt());
}

double TransmitDialog::signalBandwidthFromUi() const {
    if (!signalBandwidthCombo) return 0.0;
    const QString value = signalBandwidthCombo->currentText().trimmed().toLower();
    if (value == "auto" || value == QString::fromUtf8(u8"авто")) return 0.0;
    const double bandwidth = signalunits::comboValue(signalBandwidthCombo, false);
    return bandwidth >= 0.0 && bandwidth <= 1e9 ? bandwidth : std::numeric_limits<double>::quiet_NaN();
}

bool TransmitDialog::validateSignalBandwidth(const TxConfiguration &configuration) {
    if (configuration.sampleRate < 8000 || configuration.sampleRate > 20000000 ||
        !std::isfinite(configuration.signalBandwidthHz)) {
        setStatus(text(QStringLiteral("Enter a valid rate (e.g. 48 ksps) and bandwidth (e.g. 12.5 kHz). Live/text generation supports up to 20 Msps; higher rates up to 1 Gsps are available in Signal Lab."),
            QString::fromUtf8(u8"Введіть коректну дискретизацію (48 ksps) і смугу (12.5 kHz). Голос/текст: до 20 Msps; вищі значення до 1 Gsps доступні в лабораторії сигналів.")), true);
        return false;
    }
    const bool digitalText = TransmitWaveformGenerator::isDigitalTextMode(configuration.modulation);
    if (digitalText && (!std::isfinite(configuration.symbolRate) ||
                        configuration.symbolRate <= 0.0 ||
                        configuration.symbolRate > configuration.sampleRate * 0.25)) {
        setStatus(text(
            QStringLiteral("Symbol rate must be positive and no more than one quarter of the generator rate."),
            QString::fromUtf8(u8"Символьна швидкість має бути додатною і не більшою за чверть частоти генератора.")),
            true);
        return false;
    }

    double estimatedDigitalBandwidth = 0.0;
    if (digitalText) {
        switch (configuration.modulation) {
        case TxModulation::Fsk:
        case TxModulation::Rtty:
            estimatedDigitalBandwidth =
                2.0 * TransmitWaveformGenerator::effectiveFskDeviationHz(configuration) +
                configuration.symbolRate;
            break;
        case TxModulation::Gfsk:
        case TxModulation::Gmsk:
            estimatedDigitalBandwidth =
                2.0 * TransmitWaveformGenerator::effectiveFskDeviationHz(configuration) +
                configuration.symbolRate * 0.70;
            break;
        case TxModulation::Ook:
        case TxModulation::Ask:
        case TxModulation::Bpsk:
        case TxModulation::Qpsk:
            estimatedDigitalBandwidth = configuration.symbolRate * 1.50;
            break;
        default:
            break;
        }
    }

    if (configuration.signalBandwidthHz > 0.0) {
        const double maximum = configuration.sampleRate * 0.90;
        if (configuration.signalBandwidthHz > maximum) {
            setStatus(text(
                QStringLiteral("Signal bandwidth %1 Hz does not fit the %2 Hz generator rate. Increase the generator rate or choose a narrower preset."),
                QString::fromUtf8(u8"Смуга сигналу %1 Гц не вміщується у частоту генератора %2 Гц. Збільште частоту генератора або оберіть вужчий пресет."))
                          .arg(configuration.signalBandwidthHz, 0, 'f', 0)
                          .arg(configuration.sampleRate), true);
            return false;
        }
        if (configuration.modulation == TxModulation::Nfm ||
            configuration.modulation == TxModulation::Wfm) {
            const double deviation =
                TransmitWaveformGenerator::effectiveFmDeviationHz(configuration);
            const double audioCutoff = configuration.signalBandwidthHz * 0.5 - deviation;
            if (audioCutoff < 100.0) {
                const double minimumBandwidth = 2.0 * (deviation + 100.0);
                setStatus(text(
                    QStringLiteral("The selected FM bandwidth is incompatible with %1 Hz deviation. Use at least %2 Hz bandwidth or reduce deviation."),
                    QString::fromUtf8(u8"Обрана смуга FM несумісна з девіацією %1 Гц. Задайте смугу не менше %2 Гц або зменште девіацію."))
                              .arg(deviation, 0, 'f', 0)
                              .arg(minimumBandwidth, 0, 'f', 0), true);
                return false;
            }
        }
        if (estimatedDigitalBandwidth > 0.0 &&
            configuration.signalBandwidthHz < estimatedDigitalBandwidth) {
            setStatus(text(
                QStringLiteral("The selected digital bandwidth is too narrow. Use at least %1 Hz for the current symbol rate and deviation."),
                QString::fromUtf8(u8"Обрана цифрова смуга занадто вузька. Для поточної символьної швидкості та девіації потрібно щонайменше %1 Гц."))
                          .arg(std::ceil(estimatedDigitalBandwidth), 0, 'f', 0), true);
            return false;
        }
    }

    const double center = std::abs(TransmitWaveformGenerator::signalCenterHz(configuration));
    double occupiedBandwidth = configuration.signalBandwidthHz;
    if (occupiedBandwidth <= 0.0) {
        occupiedBandwidth = estimatedDigitalBandwidth;
        if (configuration.modulation == TxModulation::Ft8) occupiedBandwidth = 100.0;
        if (configuration.modulation == TxModulation::Cw) occupiedBandwidth = 500.0;
    }
    if (center + occupiedBandwidth * 0.5 >= configuration.sampleRate * 0.49) {
        setStatus(text(
            QStringLiteral("The selected carrier offset and occupied bandwidth do not fit the generator rate."),
            QString::fromUtf8(u8"Обраний зсув несучої та зайнята смуга не вміщуються у частоту генератора.")), true);
        return false;
    }
    return true;
}
QString TransmitDialog::signalShapeSummary(const TxConfiguration &configuration) const {
    const auto frequencyText = [](double value) {
        if (value >= 1.0e6) return QStringLiteral("%1 MHz").arg(value / 1.0e6, 0, 'f', 3);
        if (value >= 1.0e3) return QStringLiteral("%1 kHz").arg(value / 1.0e3, 0, 'f', 2);
        return QStringLiteral("%1 Hz").arg(value, 0, 'f', 0);
    };
    QString summary = configuration.signalBandwidthHz <= 0.0
                          ? text(QStringLiteral("TX bandwidth: Auto"),
                                 QString::fromUtf8(u8"Смуга TX: Авто"))
                          : text(QStringLiteral("TX bandwidth: %1"),
                                 QString::fromUtf8(u8"Смуга TX: %1"))
                                .arg(frequencyText(configuration.signalBandwidthHz));
    const double audioCutoff =
        TransmitWaveformGenerator::audioLowPassCutoffHz(configuration);
    if (audioCutoff > 0.0) {
        summary += text(QStringLiteral(" | audio LPF: %1"),
                        QString::fromUtf8(u8" | ФНЧ аудіо: %1"))
                       .arg(frequencyText(audioCutoff));
    }
    if (configuration.modulation == TxModulation::Nfm ||
        configuration.modulation == TxModulation::Wfm) {
        summary += text(QStringLiteral(" | deviation: %1"),
                        QString::fromUtf8(u8" | девіація: %1"))
                       .arg(frequencyText(
                           TransmitWaveformGenerator::effectiveFmDeviationHz(configuration)));
    }
    if (configuration.modulation == TxModulation::Dmr) {
        summary += text(QStringLiteral(" | CC %1 | TS %2 | %3 -> %4 | %5"),
                        QString::fromUtf8(u8" | CC %1 | TS %2 | %3 -> %4 | %5"))
                       .arg(configuration.dmrColorCode)
                       .arg(configuration.dmrTimeslot)
                       .arg(configuration.dmrSourceId)
                       .arg(configuration.dmrDestinationId)
                       .arg(configuration.dmrPrivateCall
                                ? text(QStringLiteral("private"), QString::fromUtf8(u8"приватний"))
                                : text(QStringLiteral("group"), QString::fromUtf8(u8"груповий")));
    }
    if (TransmitWaveformGenerator::isDigitalTextMode(configuration.modulation)) {
        summary += text(QStringLiteral(" | symbols: %1 Bd"),
                        QString::fromUtf8(u8" | символи: %1 Бод"))
                       .arg(configuration.symbolRate, 0, 'f', 2);
        const double fskDeviation =
            TransmitWaveformGenerator::effectiveFskDeviationHz(configuration);
        if (fskDeviation > 0.0) {
            summary += text(QStringLiteral(" | FSK deviation: %1"),
                            QString::fromUtf8(u8" | девіація FSK: %1"))
                           .arg(frequencyText(fskDeviation));
        }
        if (configuration.modulation == TxModulation::Gfsk ||
            configuration.modulation == TxModulation::Gmsk) {
            summary += QStringLiteral(" | BT: %1").arg(configuration.gaussianBt, 0, 'f', 2);
        }
        if (configuration.modulation == TxModulation::Am) {
            summary += text(QStringLiteral(" | AM depth: %1%"),
                            QString::fromUtf8(u8" | глибина AM: %1%"))
                           .arg(configuration.amDepth * 100.0f, 0, 'f', 0);
        } else if (configuration.modulation == TxModulation::Ask) {
            summary += text(QStringLiteral(" | ASK depth: %1%"),
                            QString::fromUtf8(u8" | глибина ASK: %1%"))
                           .arg(configuration.askDepth * 100.0f, 0, 'f', 0);
        }
        if (configuration.invertData) {
            summary += text(QStringLiteral(" | inverted"),
                            QString::fromUtf8(u8" | інверсія"));
        }
    }
    return summary;
}
void TransmitDialog::applyLiveSignalShape() {
    if (!activeBackend || !activeBackend->isRunning()) return;
    TxConfiguration next = activeConfiguration;
    next.signalBandwidthHz = signalBandwidthFromUi();
    next.deviationHz = deviationSpin->value();
    next.toneHz = toneSpin->value();
    next.symbolRate = symbolRateSpin->value();
    next.gaussianBt = gaussianBtSpin->value();
    next.amDepth = static_cast<float>(amDepthPercent) / 100.0f;
    next.askDepth = static_cast<float>(askDepthPercent) / 100.0f;
    next.invertData = invertDataCheck->isChecked();
    if (!validateSignalBandwidth(next)) return;
    activeConfiguration = next;
    bandwidthLimiterState.reset();
    modulatorState.resetAudioFilter();
    const QString summary = signalShapeSummary(activeConfiguration);
    setStatus(text(QStringLiteral("Live TX shaping updated. %1"),
                   QString::fromUtf8(u8"Формування TX оновлено наживо. %1"))
                  .arg(summary));
    qDebug().noquote() << "[TX shaping] live update"
                      << "modulation" << TransmitWaveformGenerator::modulationName(activeConfiguration.modulation)
                      << "bandwidthHz" << activeConfiguration.signalBandwidthHz
                      << "deviationHz" << activeConfiguration.deviationHz
                      << "symbolRate" << activeConfiguration.symbolRate
                      << "gaussianBt" << activeConfiguration.gaussianBt
                      << "amDepth" << activeConfiguration.amDepth
                      << "askDepth" << activeConfiguration.askDepth
                      << "invertData" << activeConfiguration.invertData
                      << "audioLowPassHz"
                      << TransmitWaveformGenerator::audioLowPassCutoffHz(activeConfiguration);
}

bool TransmitDialog::writeActiveIq(const std::complex<float> *samples, int count, QString *error) {
    if (!activeBackend) {
        if (error) *error = QStringLiteral("No active transmitter backend");
        return false;
    }
    // DMR is already pulse-shaped as continuous-phase 4FSK by
    // DmrTransmitEncoder. Re-filtering the complex IQ here changes its phase
    // trajectory and destroys the intended constant-envelope waveform.
    if (activeConfiguration.modulation != TxModulation::Dmr &&
        TransmitWaveformGenerator::limitSignalBandwidth(activeConfiguration, samples, count,
                                                         &bandwidthLimitedIq,
                                                         &bandwidthLimiterState)) {
        return activeBackend->writeIq(bandwidthLimitedIq.constData(), bandwidthLimitedIq.size(), error);
    }
    return activeBackend->writeIq(samples, count, error);
}
TxConfiguration TransmitDialog::configurationFromUi() const {
    TxConfiguration configuration;
    configuration.modulation = selectedModulation();
    configuration.frequencyHz = frequencySpin->value() * 1e6;
    configuration.sampleRate = signalunits::sampleRateValue(sampleRateCombo);
    configuration.level = static_cast<float>(levelSpin->value()) / 100.0f;
    configuration.deviationHz = deviationSpin->value();
    configuration.signalBandwidthHz = signalBandwidthFromUi();
    configuration.toneHz = toneSpin->value();
    configuration.cwWpm = cwWpmSpin->value();
    configuration.symbolRate = symbolRateSpin->value();
    configuration.gaussianBt = gaussianBtSpin->value();
    configuration.amDepth = static_cast<float>(amDepthPercent) / 100.0f;
    configuration.askDepth = static_cast<float>(askDepthPercent) / 100.0f;
    configuration.invertData = invertDataCheck->isChecked();
    configuration.alignFt8ToUtcSlot = ft8UtcCheck->isChecked();
    configuration.ft8SlotParity = ft8SlotCombo->currentData().toInt();
    configuration.dmrColorCode = dmrColorCodeSpin->value();
    configuration.dmrTimeslot = dmrTimeslotCombo->currentData().toInt();
    configuration.dmrSourceId = static_cast<std::uint32_t>(dmrSourceIdSpin->value());
    configuration.dmrDestinationId = static_cast<std::uint32_t>(dmrDestinationIdSpin->value());
    configuration.dmrPrivateCall = dmrCallTypeCombo->currentData().toBool();
    configuration.dmrPrivacyMode = dmrPrivacyCombo->currentData().toInt();
    configuration.dmrPrivacyKeyId = dmrPrivacyKeyIdSpin->value();
    QString privacyHex = dmrPrivacyKeyEdit->text().trimmed();
    privacyHex.remove(QLatin1Char(' '));
    privacyHex.remove(QLatin1Char('-'));
    configuration.dmrPrivacyKey = QByteArray::fromHex(privacyHex.toLatin1());
    configuration.deviceIndex = deviceCombo->currentData().toInt();
    configuration.deviceSampleRate = deviceSampleRateCombo->currentData().toInt();
    configuration.bandwidthHz = static_cast<std::uint32_t>(bandwidthCombo->currentData().toUInt());
    configuration.txVgaGainDb = txGainSpin->value();
    configuration.rfAmpEnabled = rfAmpCheck->isChecked();
    return configuration;
}

void TransmitDialog::applyModulationPreset(TxModulation modulation) {
    const auto setBandwidth = [this](double bandwidth) {
        const int index = signalBandwidthCombo->findData(bandwidth);
        if (index >= 0) signalBandwidthCombo->setCurrentIndex(index);
    };
    switch (modulation) {
    case TxModulation::Am:
    case TxModulation::Dsb:
        setBandwidth(10000.0);
        break;
    case TxModulation::Nfm:
        deviationSpin->setValue(2500.0);
        setBandwidth(12500.0);
        break;
    case TxModulation::Wfm:
        deviationSpin->setValue(75000.0);
        setBandwidth(200000.0);
        break;
    case TxModulation::Usb:
    case TxModulation::Lsb:
        setBandwidth(2700.0);
        break;
    case TxModulation::Cw:
        toneSpin->setValue(700.0);
        setBandwidth(500.0);
        break;
    case TxModulation::Ft8:
        toneSpin->setValue(1500.0);
        setBandwidth(100.0);
        break;
    case TxModulation::Ook:
    case TxModulation::Ask:
    case TxModulation::Bpsk:
        symbolRateSpin->setValue(1200.0);
        toneSpin->setValue(0.0);
        setBandwidth(2400.0);
        break;
    case TxModulation::Fsk:
    case TxModulation::Gfsk:
        symbolRateSpin->setValue(1200.0);
        deviationSpin->setValue(600.0);
        gaussianBtSpin->setValue(0.50);
        toneSpin->setValue(0.0);
        setBandwidth(5000.0);
        break;
    case TxModulation::Gmsk:
        symbolRateSpin->setValue(9600.0);
        gaussianBtSpin->setValue(0.30);
        toneSpin->setValue(0.0);
        setBandwidth(12500.0);
        break;
    case TxModulation::Qpsk:
        symbolRateSpin->setValue(2400.0);
        toneSpin->setValue(0.0);
        setBandwidth(5000.0);
        break;
    case TxModulation::Rtty:
        symbolRateSpin->setValue(45.45);
        deviationSpin->setValue(85.0);
        toneSpin->setValue(2210.0);
        setBandwidth(500.0);
        break;
    case TxModulation::Dmr: {
        const int rateIndex = sampleRateCombo->findData(48000);
        if (rateIndex >= 0) sampleRateCombo->setCurrentIndex(rateIndex);
        symbolRateSpin->setValue(4800.0);
        toneSpin->setValue(0.0);
        setBandwidth(12500.0);
        break;
    }
    }
}
void TransmitDialog::updateModeUi() {
    const TxModulation modulation = selectedModulation();
    QString source = sourceCombo->currentData().toString();
    const bool dmrMode = modulation == TxModulation::Dmr;
    if (dmrMode && source != QStringLiteral("microphone") &&
        source != QStringLiteral("microphone-ptt") &&
        source != QStringLiteral("audio-file")) {
        sourceCombo->setCurrentIndex(sourceCombo->findData(QStringLiteral("microphone")));
        source = QStringLiteral("microphone");
    } else if ((modulation == TxModulation::Ft8 ||
         TransmitWaveformGenerator::isDigitalTextMode(modulation)) &&
        source != QStringLiteral("text")) {
        sourceCombo->setCurrentIndex(sourceCombo->findData(QStringLiteral("text")));
        source = QStringLiteral("text");
    } else if (modulation == TxModulation::Cw && source != QStringLiteral("text") &&
               source != QStringLiteral("manual-cw")) {
        sourceCombo->setCurrentIndex(sourceCombo->findData(QStringLiteral("text")));
        source = QStringLiteral("text");
    }
    sourceCombo->setEnabled(true);
    const bool textSource = source == QStringLiteral("text");
    const bool microphoneSource = source == QStringLiteral("microphone") ||
                                  source == QStringLiteral("microphone-ptt");
    const bool mediaSource = source == QStringLiteral("audio-file") ||
                             source == QStringLiteral("image-sstv") ||
                             source == QStringLiteral("image-atv");
    const bool imageSource = source.startsWith(QStringLiteral("image-"));
    const bool liveKeySource = source == QStringLiteral("manual-cw") ||
                               source == QStringLiteral("microphone-ptt");
    if (source == QStringLiteral("image-sstv") &&
        mediaModeCombo->currentData().toString().startsWith(QStringLiteral("atv-"))) {
        mediaModeCombo->setCurrentIndex(mediaModeCombo->findData(QStringLiteral("robot36")));
    } else if (source == QStringLiteral("image-atv") &&
               !mediaModeCombo->currentData().toString().startsWith(QStringLiteral("atv-"))) {
        mediaModeCombo->setCurrentIndex(mediaModeCombo->findData(QStringLiteral("atv-am")));
    }
    messageEdit->setEnabled(textSource);
    if (auto *box = findChild<QGroupBox *>(QStringLiteral("txMessageBox"))) box->setVisible(textSource);
    if (auto *box = findChild<QGroupBox *>(QStringLiteral("txMediaBox"))) box->setVisible(mediaSource);
    if (dmrBox) dmrBox->setVisible(dmrMode);
    const bool privacyEnabled = dmrMode &&
                                dmrPrivacyCombo->currentData().toInt() != DMR_PRIVACY_NONE;
    dmrPrivacyKeyIdSpin->setEnabled(privacyEnabled);
    dmrPrivacyKeyEdit->setEnabled(privacyEnabled);
    dmrPrivacyKeyEdit->setPlaceholderText(
        dmrPrivacyCombo->currentData().toInt() == DMR_PRIVACY_AES256
            ? QStringLiteral("64 hex digits")
            : QStringLiteral("10 hex digits"));
    mediaModeCombo->setEnabled(imageSource);
    mediaEditButton->setEnabled(imageSource && !mediaPath.isEmpty());
    mediaPauseButton->setEnabled(source == QStringLiteral("audio-file") ||
                                 source == QStringLiteral("image-atv"));
    const bool microphoneAvailable = audioInputCombo->count() > 0 &&
                                     !audioInputCombo->currentData().toString().isEmpty();
    audioInputCombo->setEnabled(microphoneSource && microphoneAvailable);
    microphoneLevelSlider->setEnabled(microphoneSource && microphoneAvailable);
    audioRefreshButton->setEnabled(microphoneSource && !(activeBackend && activeBackend->isRunning()));
    const bool digitalText = TransmitWaveformGenerator::isDigitalTextMode(modulation);
    const bool fskMode = modulation == TxModulation::Fsk ||
                         modulation == TxModulation::Gfsk ||
                         modulation == TxModulation::Gmsk ||
                         modulation == TxModulation::Rtty;
    deviationSpin->setEnabled(modulation == TxModulation::Nfm ||
                              modulation == TxModulation::Wfm ||
                              (fskMode && modulation != TxModulation::Gmsk));
    signalBandwidthCombo->setEnabled(!dmrMode);
    toneSpin->setEnabled(TransmitWaveformGenerator::isTextMode(modulation) ||
                         source == QStringLiteral("manual-cw"));
    cwWpmSpin->setEnabled(modulation == TxModulation::Cw);
    symbolRateSpin->setEnabled(digitalText);
    gaussianBtSpin->setEnabled(modulation == TxModulation::Gfsk ||
                               modulation == TxModulation::Gmsk);
    const bool depthMode = modulation == TxModulation::Am || modulation == TxModulation::Ask;
    modulationDepthSpin->setEnabled(depthMode);
    modulationDepthSpin->setValue(modulation == TxModulation::Ask
                                      ? askDepthPercent
                                      : amDepthPercent);
    invertDataCheck->setEnabled(digitalText);
    ft8UtcCheck->setEnabled(modulation == TxModulation::Ft8);
    ft8SlotCombo->setEnabled(modulation == TxModulation::Ft8 && ft8UtcCheck->isChecked());
    messageEdit->setPlaceholderText(
        digitalText
            ? text(QStringLiteral("UTF-8 payload / RTTY text"),
                   QString::fromUtf8(u8"UTF-8 payload / текст RTTY"))
            : QStringLiteral("CQ TEST KN34 / HELLO WORLD"));
    liveKeyButton->setVisible(liveKeySource);
    liveKeyButton->setEnabled(liveKeySource);
    const bool hardware = hackRfSelected();
    deviceCombo->setEnabled(hardware);
    deviceSampleRateCombo->setEnabled(hardware);
    bandwidthCombo->setEnabled(hardware);
    txGainSpin->setEnabled(hardware);
    watchdogSpin->setEnabled(hardware);
    rfAmpCheck->setEnabled(hardware);
    armCheck->setEnabled(hardware);
    frequencySpin->setMaximum(hardware ? 6000.0 : 7250.0);
    updateTexts();
}

void TransmitDialog::startTransmission() {
    if (activeBackend && activeBackend->isRunning()) return;
    hackRfStoppedStreamPolls = 0;
    activeBackend = selectedBackend();
    if (activeBackend->isRfCapable()) {
        if (!armCheck->isChecked()) {
            setStatus(text(QStringLiteral("Arm the RF output before starting"),
                           QString::fromUtf8(u8"Перед запуском озбройте RF-вихід")), true);
            return;
        }
        if (deviceCombo->currentData().toInt() < 0) {
            setStatus(text(QStringLiteral("No HackRF device is available"),
                           QString::fromUtf8(u8"Немає доступного пристрою HackRF")), true);
            return;
        }
        if (selectedHackRfBusyForReception()) {
            setStatus(text(
                QStringLiteral("This HackRF is already receiving. Select another HackRF or stop RX before transmitting."),
                QString::fromUtf8(u8"Цей HackRF уже працює на прийом. Оберіть інший HackRF або зупиніть RX перед передачею.")),
                true);
            return;
        }
    }
    saveSettings();
    preview->clear();
    progressBar->setValue(0);
    statsLabel->clear();
    modulatorState = TxModulatorState{};
    bandwidthLimiterState.reset();
    bandwidthLimitedIq.clear();
    TxConfiguration configuration = configurationFromUi();
    if (!validateSignalBandwidth(configuration)) return;
    const QString source = sourceCombo->currentData().toString();
    pttMode = source == QStringLiteral("microphone-ptt");
    if (source == QStringLiteral("text")) {
        startTextSimulation(configuration);
    } else if (source == QStringLiteral("microphone") || pttMode) {
        startMicrophoneSimulation(configuration);
    } else if (source == QStringLiteral("manual-cw")) {
        startManualCw(configuration);
    } else if (source == QStringLiteral("audio-file")) {
        startAudioFile(configuration);
    } else if (source == QStringLiteral("image-sstv") || source == QStringLiteral("image-atv")) {
        startImageTransmission(configuration);
    } else {
        setStatus(text(QStringLiteral("Unsupported transmission source"),
                       QString::fromUtf8(u8"Непідтримуване джерело передачі")), true);
    }
}

void TransmitDialog::startLaboratoryTransmission(
    const SignalLabGenerationResult &result) {
    if (!result.ok()) {
        setStatus(text(QStringLiteral("Signal laboratory has no valid IQ waveform"),
                       QString::fromUtf8(u8"Лабораторія сигналів не має коректного IQ-сигналу")),
                  true);
        return;
    }
    if (activeBackend && activeBackend->isRunning()) {
        setStatus(text(QStringLiteral("Stop the current transmission before starting a laboratory waveform"),
                       QString::fromUtf8(u8"Зупиніть поточну передачу перед запуском лабораторного сигналу")),
                  true);
        return;
    }

    activeBackend = selectedBackend();
    if (activeBackend->isRfCapable()) {
        if (!armCheck->isChecked()) {
            activeBackend = nullptr;
            setStatus(text(QStringLiteral("Arm the RF output in the transmitter window before laboratory TX"),
                           QString::fromUtf8(u8"Перед лабораторною TX-передачею озбройте RF-вихід у вікні передавача")),
                      true);
            return;
        }
        if (deviceCombo->currentData().toInt() < 0) {
            activeBackend = nullptr;
            setStatus(text(QStringLiteral("No HackRF device is available"),
                           QString::fromUtf8(u8"Немає доступного пристрою HackRF")),
                      true);
            return;
        }
        if (selectedHackRfBusyForReception()) {
            activeBackend = nullptr;
            setStatus(text(
                QStringLiteral("This HackRF is already receiving. Select another HackRF or stop RX before transmitting."),
                QString::fromUtf8(u8"Цей HackRF уже працює на прийом. Оберіть інший HackRF або зупиніть RX перед передачею.")),
                true);
            return;
        }
    }

    saveSettings();
    preview->clear();
    progressBar->setValue(0);
    modulatorState = TxModulatorState{};
    bandwidthLimiterState.reset();
    bandwidthLimitedIq.clear();

    TxConfiguration configuration = configurationFromUi();
    configuration.sampleRate = result.sampleRate;
    configuration.level = 1.0f;
    configuration.signalBandwidthHz = result.occupiedBandwidthHz;
    activeConfiguration = configuration;

    QString error;
    if (!activeBackend->start(configuration, &error)) {
        activeBackend->stop();
        activeBackend = nullptr;
        armCheck->setChecked(false);
        setStatus(error, true);
        return;
    }
    // Laboratory IQ is already the final shaped baseband. Bypass the normal
    // modulation bandwidth limiter so OFDM pilots, cyclic prefix and PAPR stay intact.
    if (!activeBackend->writeIq(result.iq.constData(), result.iq.size(), &error)) {
        activeBackend->stop();
        activeBackend = nullptr;
        armCheck->setChecked(false);
        setStatus(error, true);
        return;
    }

    preview->setIq(result.iq);
    plannedDurationSeconds = result.durationSeconds;
    textSimulation = true;
    pttMode = false;
    startButton->setEnabled(false);
    stopButton->setEnabled(true);
    sourceCombo->setEnabled(false);
    exportButton->setEnabled(false);
    statsLabel->setText(result.summary);
    elapsedTimer.restart();
    progressTimer->start();
    setStatus(activeBackend->isRfCapable()
                  ? text(QStringLiteral("Laboratory IQ waveform is being transmitted by HackRF"),
                         QString::fromUtf8(u8"Лабораторний IQ-сигнал передається через HackRF"))
                  : text(QStringLiteral("Laboratory IQ simulation is running"),
                         QString::fromUtf8(u8"Симуляція лабораторного IQ запущена")));
}
void TransmitDialog::startTextSimulation(const TxConfiguration &configuration) {
    const TxGenerationResult generated =
        TransmitWaveformGenerator::generateText(configuration, messageEdit->toPlainText());
    if (!generated.ok()) {
        armCheck->setChecked(false);
        setStatus(generated.error, true);
        return;
    }
    QString error;
    activeConfiguration = configuration;
    if (!activeBackend || !activeBackend->start(configuration, &error)) {
        if (activeBackend) activeBackend->stop();
        armCheck->setChecked(false);
        setStatus(error, true);
        return;
    }
    preview->setIq(generated.iq);
    plannedDurationSeconds = generated.durationSeconds;
    pendingTextIq = generated.iq;
    textSimulation = true;
    startButton->setEnabled(false);
    stopButton->setEnabled(true);
    exportButton->setEnabled(false);
    sourceCombo->setEnabled(false);
    statsLabel->setText(generated.summary);

    if (configuration.modulation == TxModulation::Ft8 && configuration.alignFt8ToUtcSlot) {
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        qint64 targetMs = ((nowMs / 15000) + 1) * 15000;
        if (configuration.ft8SlotParity >= 0) {
            while (((targetMs / 15000) & 1) != configuration.ft8SlotParity) targetMs += 15000;
        }
        const int delayMs = static_cast<int>((std::max<qint64>)(1, targetMs - nowMs));
        ft8StartTimer->start(delayMs);
        progressBar->setValue(0);
        setStatus(text(QStringLiteral("FT8 armed; waiting for UTC slot %1."),
                       QString::fromUtf8(u8"FT8 підготовлено; очікування UTC-слота %1."))
                      .arg(QDateTime::fromMSecsSinceEpoch(targetMs, Qt::UTC)
                               .toString(QStringLiteral("HH:mm:ss"))));
        return;
    }
    beginTextWaveform();
}

void TransmitDialog::beginTextWaveform() {
    if (!activeBackend || pendingTextIq.isEmpty()) return;
    QString error;
    if (!writeActiveIq(pendingTextIq.constData(), pendingTextIq.size(), &error)) {
        armCheck->setChecked(false);
        stopTransmission(error);
        return;
    }
    pendingTextIq.clear();
    pendingTextIq.squeeze();
    elapsedTimer.restart();
    progressTimer->start();
    setStatus(activeBackend->isRfCapable()
                  ? text(QStringLiteral("Text waveform is being transmitted by HackRF."),
                         QString::fromUtf8(u8"Текстовий сигнал передається через HackRF."))
                  : text(QStringLiteral("Text baseband simulation is running."),
                         QString::fromUtf8(u8"Симуляція текстового baseband запущена.")));
}
void TransmitDialog::startMicrophoneSimulation(TxConfiguration configuration) {
#ifdef FOBOSAPP_HAS_QT_MULTIMEDIA
#ifdef _WIN32
    if (windowsMicrophoneCapture) {
        windowsMicrophoneCapture->stop();
        delete windowsMicrophoneCapture;
        windowsMicrophoneCapture = nullptr;
    }
#endif
    if (audioInput) {
        audioInput->disconnect(this);
        audioInput->stop();
        delete audioInput;
        audioInput = nullptr;
        audioDevice = nullptr;
    }
    populateAudioInputs();
    const QString requestedDevice = audioInputCombo->currentData().toString();
    const QList<QAudioDeviceInfo> devices = QAudioDeviceInfo::availableDevices(QAudio::AudioInput);
    const QAudioDeviceInfo defaultDevice = QAudioDeviceInfo::defaultInputDevice();
    QAudioDeviceInfo selected;
    for (const QAudioDeviceInfo &device : devices) {
        if (device.deviceName() == requestedDevice) {
            selected = device;
            break;
        }
    }
    if (selected.isNull() && !defaultDevice.isNull() &&
        (requestedDevice.isEmpty() || requestedDevice == defaultDevice.deviceName())) {
        selected = defaultDevice;
    }
    if (selected.isNull()) {
        setStatus(text(QStringLiteral("No microphone input is currently available. Refresh the device list and check Windows microphone privacy settings."),
                       QString::fromUtf8(u8"Зараз немає доступного мікрофонного входу. Оновіть список пристроїв і перевірте дозволи мікрофона у Windows.")), true);
        return;
    }
    QAudioFormat requestedFormat;
    requestedFormat.setCodec(QStringLiteral("audio/pcm"));
    requestedFormat.setSampleRate(48000);
    requestedFormat.setChannelCount(1);
    requestedFormat.setSampleSize(16);
    requestedFormat.setSampleType(QAudioFormat::SignedInt);
    requestedFormat.setByteOrder(QAudioFormat::LittleEndian);
    // Prefer a well-defined format. Qt 5 can report a legacy 11025 Hz/8-bit
    // endpoint as the Windows native format even though its byte stream uses
    // unsigned PCM semantics, producing a near-full-scale DC offset.
    QAudioFormat format = selected.isFormatSupported(requestedFormat)
                              ? requestedFormat
                              : selected.nearestFormat(requestedFormat);
    if (!supportedAudioCaptureFormat(format)) format = selected.preferredFormat();
    if (!supportedAudioCaptureFormat(format)) {
        setStatus(text(QStringLiteral("The selected microphone format is unsupported (%1 Hz, %2 channel(s), %3-bit PCM). Try another input device."),
                       QString::fromUtf8(u8"Формат обраного мікрофона не підтримується (%1 Гц, %2 канал(и), %3-bit PCM). Спробуйте інший вхід."))
                      .arg(format.sampleRate())
                      .arg(format.channelCount())
                      .arg(format.sampleSize()), true);
        return;
    }
    configuration.sampleRate = format.sampleRate();
    const bool deviationAdjusted = constrainVoiceDeviation(configuration);
    if (!validateSignalBandwidth(configuration)) return;
    configuration.liveSource = true;
    activeConfiguration = configuration;
    QString error;
    if (configuration.modulation == TxModulation::Dmr &&
        !configureDmrEncoder(configuration, &error)) {
        armCheck->setChecked(false);
        setStatus(error, true);
        return;
    }
    if (!activeBackend || !activeBackend->start(configuration, &error)) {
        armCheck->setChecked(false);
        setStatus(error, true);
        return;
    }
    if (pttMode && !activeBackend->setOutputEnabled(false, &error)) {
        activeBackend->stop();
        armCheck->setChecked(false);
        setStatus(error, true);
        return;
    }
    if (configuration.modulation == TxModulation::Dmr && !pttMode && !beginDmrCall(&error)) {
        activeBackend->stop();
        armCheck->setChecked(false);
        setStatus(error, true);
        return;
    }
    audioCaptureChannels = format.channelCount();
    audioCaptureSampleSize = format.sampleSize();
    audioCaptureSampleType = static_cast<int>(format.sampleType());
    audioCaptureLittleEndian = format.byteOrder() == QAudioFormat::LittleEndian;
    audioCapturePendingBytes.clear();
    microphoneDiagnosticFrames = 0;
    microphoneDiagnosticNonZeroFrames = 0;
    microphoneDcPreviousInput = 0.0f;
    microphoneDcPreviousOutput = 0.0f;
    microphoneSilenceWarned = false;
    microphoneDiagnosticTimer.restart();
    microphoneRmsDb = -120.0f;
    microphonePeakDb = -120.0f;
    const int microphoneLevelValue = microphoneLevelSlider->value();
    const float microphoneSystemLevel =
        (std::min)(100, microphoneLevelValue) / 100.0f;
    const int microphoneBoostDb = (std::max)(0, microphoneLevelValue - 100);
    microphoneSoftwareGain =
        static_cast<float>(std::pow(10.0, microphoneBoostDb / 20.0));
    bool nativeCapture = false;
#ifdef _WIN32
    windowsMicrophoneCapture = new WindowsMicrophoneCapture(this);
    connect(windowsMicrophoneCapture, &WindowsMicrophoneCapture::pcmReady,
            this, [this](const QByteArray &pcm) { processMicrophoneBytes(pcm); });
    QString nativeCaptureError;
    nativeCapture = windowsMicrophoneCapture->start(requestedDevice, 48000,
                                                     microphoneSystemLevel,
                                                     &nativeCaptureError);
    if (nativeCapture) {
        audioCaptureChannels = 1;
        audioCaptureSampleSize = 16;
        audioCaptureSampleType = static_cast<int>(QAudioFormat::SignedInt);
        audioCaptureLittleEndian = true;
        qInfo().noquote() << "[TX audio] capture backend"
                          << windowsMicrophoneCapture->backendName()
                          << "requestedDevice" << requestedDevice
                          << "available" << WindowsMicrophoneCapture::deviceNames().join(QStringLiteral(" | "));
    } else {
        qWarning().noquote() << "[TX audio] Native Windows capture unavailable; falling back to Qt"
                             << nativeCaptureError;
        delete windowsMicrophoneCapture;
        windowsMicrophoneCapture = nullptr;
    }
#endif
    if (!nativeCapture) {
        audioInput = new QAudioInput(selected, format, this);
        audioInput->setVolume(microphoneSystemLevel);
        const int captureBytesPerFrame =
            (std::max)(1, format.channelCount()) * (std::max)(1, format.sampleSize() / 8);
        audioInput->setBufferSize(
            (std::max)(4096, format.sampleRate() / 5 * captureBytesPerFrame));
        connect(audioInput, &QAudioInput::stateChanged, this, [this](QAudio::State state) {
            if (state != QAudio::StoppedState || !audioInput ||
                audioInput->error() == QAudio::NoError) {
                return;
            }
            const QString detail = audioInputErrorText(audioInput->error());
            QTimer::singleShot(0, this, [this, detail]() {
                if (!audioInput || !activeBackend || !activeBackend->isRunning()) return;
                stopTransmission(text(QStringLiteral("Microphone capture stopped: %1"),
                                      QString::fromUtf8(u8"Захоплення мікрофона зупинилося: %1"))
                                     .arg(detail));
            });
        });
        audioDevice = audioInput->start();
        if (!audioDevice) {
            const QString detail = audioInputErrorText(audioInput->error());
            activeBackend->stop();
            audioInput->disconnect(this);
            audioInput->stop();
            delete audioInput;
            audioInput = nullptr;
            audioDevice = nullptr;
            setStatus(text(QStringLiteral("Could not start microphone capture from '%1': %2. Refresh the list and check Windows microphone privacy/exclusive-mode settings."),
                           QString::fromUtf8(u8"Не вдалося запустити захоплення мікрофона '%1': %2. Оновіть список і перевірте дозволи Windows та монопольний режим пристрою."))
                          .arg(selected.deviceName(), detail), true);
            return;
        }
        connect(audioDevice, &QIODevice::readyRead, this, [this]() { readMicrophone(); });
        qInfo() << "[TX audio] capture backend Qt Multimedia";
    }
    plannedDurationSeconds = activeBackend->isRfCapable() ? watchdogSpin->value() : 180.0;
    textSimulation = false;
    elapsedTimer.restart();
    progressTimer->start();
    startButton->setEnabled(false);
    stopButton->setEnabled(true);
    exportButton->setEnabled(false);
    sourceCombo->setEnabled(false);
    if (pttMode) {
        liveKeyButton->setEnabled(true);
        liveKeyButton->setFocus(Qt::OtherFocusReason);
        updateTexts();
    }
    statsLabel->setText(signalShapeSummary(configuration));
    qInfo().noquote() << "[TX shaping] start"
                      << "microphone" << selected.deviceName()
                      << "sampleRate" << configuration.sampleRate
                      << "channels" << audioCaptureChannels
                      << "sampleSize" << audioCaptureSampleSize
                      << "sampleType" << audioCaptureSampleType
                      << "byteOrder" << (audioCaptureLittleEndian ? "little" : "big")
                      << "modulation" << TransmitWaveformGenerator::modulationName(configuration.modulation)
                      << "bandwidthHz" << configuration.signalBandwidthHz
                      << "deviationHz" << configuration.deviationHz
                      << "audioLowPassHz"
                      << TransmitWaveformGenerator::audioLowPassCutoffHz(configuration)
                      << "deviationAdjusted" << deviationAdjusted;
    setStatus(activeBackend->isRfCapable()
                  ? (pttMode
                         ? text(QStringLiteral("Microphone PTT is armed. Focus the PTT button and hold Space, "
                                               "hold it with the mouse, or use F12."),
                                QString::fromUtf8(u8"PTT мікрофона озброєно. Передайте фокус кнопці PTT й "
                                                  u8"утримуйте Пробіл, утримуйте її мишею або використовуйте F12."))
                         : text(QStringLiteral("Live microphone transmission is running."),
                                QString::fromUtf8(u8"Передача з мікрофона працює.")))
                  : text(QStringLiteral("Microphone baseband simulation is running; RF remains disabled."),
                         QString::fromUtf8(u8"Симуляція baseband з мікрофона працює; RF залишається вимкненим.")));
#else
    Q_UNUSED(configuration)
    setStatus(text(QStringLiteral("This build has no Qt Multimedia microphone support"),
                   QString::fromUtf8(u8"Ця збірка не має підтримки мікрофона Qt Multimedia")), true);
#endif
}

void TransmitDialog::startManualCw(TxConfiguration configuration) {
    configuration.modulation = TxModulation::Cw;
    // Manual keying is produced incrementally. Wait for a useful reserve
    // instead of starting the HackRF stream from a single 20 ms block.
    configuration.liveSource = true;
    activeConfiguration = configuration;
    QString error;
    if (!activeBackend || !activeBackend->start(configuration, &error) ||
        !activeBackend->setOutputEnabled(false, &error)) {
        if (activeBackend) activeBackend->stop();
        armCheck->setChecked(false);
        setStatus(error, true);
        return;
    }
    manualCwPhase = 0.0;
    plannedDurationSeconds = activeBackend->isRfCapable() ? watchdogSpin->value() : 180.0;
    textSimulation = false;
    elapsedTimer.restart();
    progressTimer->start();
    startButton->setEnabled(false);
    stopButton->setEnabled(true);
    sourceCombo->setEnabled(false);
    liveKeyButton->setEnabled(true);
    liveKeyButton->setFocus(Qt::OtherFocusReason);
    updateTexts();
    setStatus(text(QStringLiteral("Manual CW is armed. Focus the key button and hold Space, "
                                  "hold it with the mouse, or use F12."),
                   QString::fromUtf8(u8"Ручний CW озброєно. Передайте фокус кнопці ключа й утримуйте "
                                     u8"Пробіл, утримуйте її мишею або використовуйте F12.")));
}

void TransmitDialog::startAudioFile(TxConfiguration configuration) {
    if (mediaPath.isEmpty()) chooseMediaFile();
    if (mediaPath.isEmpty()) return;
    // Voice/media audio needs enough Nyquist headroom even when a low-rate
    // digital preset was selected previously. RF shaping remains controlled
    // independently by the requested TX signal bandwidth.
    configuration.sampleRate = (std::clamp)(configuration.sampleRate, 48000, 192000);
    const TxAudioMedia media = TransmitMediaGenerator::loadWav(mediaPath, configuration.sampleRate);
    if (!media.ok()) {
        setStatus(media.error, true);
        return;
    }
    configuration.sampleRate = media.sampleRate;
    constrainVoiceDeviation(configuration);
    if (!validateSignalBandwidth(configuration)) return;
    configuration.liveSource = true;
    const int mediaDurationMilliseconds = static_cast<int>(
        (static_cast<qint64>(media.samples.size()) * 1000) / media.sampleRate);
    configuration.prebufferMilliseconds =
        (std::clamp)(mediaDurationMilliseconds, 20, 400);
    activeConfiguration = configuration;
    QString error;
    if (configuration.modulation == TxModulation::Dmr &&
        !configureDmrEncoder(configuration, &error)) {
        armCheck->setChecked(false);
        setStatus(error, true);
        return;
    }
    if (!activeBackend || !activeBackend->start(configuration, &error)) {
        armCheck->setChecked(false);
        setStatus(error, true);
        return;
    }
    if (configuration.modulation == TxModulation::Dmr && !beginDmrCall(&error)) {
        activeBackend->stop();
        armCheck->setChecked(false);
        setStatus(error, true);
        return;
    }
    mediaAudio = media.samples;
    atvFrame.clear();
    mediaPosition = 0;
    mediaPaused = false;
    plannedDurationSeconds = static_cast<double>(mediaAudio.size()) / configuration.sampleRate;
    textSimulation = false;
    elapsedTimer.restart();
    progressTimer->start();
    sourceTimer->setInterval(10);
    processLiveSource();
    if (!activeBackend || !activeBackend->isRunning()) return;
    sourceTimer->start();
    startButton->setEnabled(false);
    stopButton->setEnabled(true);
    sourceCombo->setEnabled(false);
    statsLabel->setText(media.summary);
    setStatus(text(QStringLiteral("Audio file transmission is running."),
                   QString::fromUtf8(u8"Передача аудіофайлу працює.")));
}

void TransmitDialog::startImageTransmission(TxConfiguration configuration) {
    if (mediaPath.isEmpty() && sourceCombo->currentData().toString() == QStringLiteral("image-sstv")) {
        chooseMediaFile();
    }
    if (preparedMediaImage.isNull() && !mediaPath.isEmpty()) editMediaImage();
    const QImage image = preparedMediaImage.isNull() ? QImage(mediaPath) : preparedMediaImage;
    const QString requestedMode = mediaModeCombo->currentData().toString();
    const bool atv = sourceCombo->currentData().toString() == QStringLiteral("image-atv");
    if (!atv) {
        configuration.sampleRate = (std::clamp)(configuration.sampleRate, 12000, 48000);
        const QString mode = requestedMode.startsWith(QStringLiteral("atv-"))
                                 ? QStringLiteral("robot36") : requestedMode;
        const TxAudioMedia media = TransmitMediaGenerator::generateSstv(
            image, mode, configuration.sampleRate);
        if (!media.ok()) {
            setStatus(media.error, true);
            return;
        }
        if (configuration.modulation == TxModulation::Cw ||
            configuration.modulation == TxModulation::Ft8) {
            configuration.modulation = TxModulation::Nfm;
        }
        configuration.deviationHz = (std::max)(2500.0, configuration.deviationHz);
        if (!validateSignalBandwidth(configuration)) return;
        configuration.liveSource = false;
        const QVector<std::complex<float>> iq =
            TransmitWaveformGenerator::modulateAudio(configuration, media.samples);
        QString error;
        activeConfiguration = configuration;
        if (!activeBackend || !activeBackend->start(configuration, &error) ||
            !writeActiveIq(iq.constData(), iq.size(), &error)) {
            if (activeBackend) activeBackend->stop();
            armCheck->setChecked(false);
            setStatus(error, true);
            return;
        }
        preview->setIq(iq);
        plannedDurationSeconds = static_cast<double>(iq.size()) / configuration.sampleRate;
        textSimulation = true;
        elapsedTimer.restart();
        progressTimer->start();
        startButton->setEnabled(false);
        stopButton->setEnabled(true);
        sourceCombo->setEnabled(false);
        statsLabel->setText(media.summary);
        setStatus(text(QStringLiteral("SSTV image transmission is running."),
                       QString::fromUtf8(u8"Передача зображення SSTV працює.")));
        return;
    }

    configuration.sampleRate = activeBackend && activeBackend->isRfCapable()
                                   ? (std::min)(2000000, configuration.deviceSampleRate)
                                   : 500000;
    configuration.liveSource = true;
    const bool fmVideo = requestedMode == QStringLiteral("atv-fm");
    configuration.modulation = fmVideo ? TxModulation::Nfm : TxModulation::Am;
    // ATV frames are generated ahead of time, so give HackRF a generous
    // reserve before its first USB transfer. A 200 ms voice-style prebuffer
    // can be consumed by a single large transfer before the GUI timer refills
    // the queue.
    configuration.prebufferMilliseconds = 750;
    // A voice/SSTV bandwidth can remain selected when the source is changed to
    // ATV. Applying that filter to a 15.625 kHz raster removes the sync edges
    // and almost all horizontal detail before the samples reach the backend.
    // Preserve deliberate ATV presets, but replace stale narrow-band values.
    constexpr double minimumAtvBandwidthHz = 500000.0;
    constexpr double defaultAtvBandwidthHz = 1500000.0;
    if (configuration.signalBandwidthHz > 0.0 &&
        configuration.signalBandwidthHz < minimumAtvBandwidthHz) {
        const double previousBandwidthHz = configuration.signalBandwidthHz;
        configuration.signalBandwidthHz = (std::min)(
            defaultAtvBandwidthHz,
            static_cast<double>(configuration.sampleRate) * 0.75);
        const int bandwidthIndex = signalBandwidthCombo
                                       ? signalBandwidthCombo->findData(
                                             configuration.signalBandwidthHz)
                                       : -1;
        if (bandwidthIndex >= 0) signalBandwidthCombo->setCurrentIndex(bandwidthIndex);
        qWarning().noquote() << "[ATV TX] replacing narrow stale bandwidth"
                             << previousBandwidthHz << "Hz with"
                             << configuration.signalBandwidthHz << "Hz";
    }
    if (!validateSignalBandwidth(configuration)) return;
    atvFrame = TransmitMediaGenerator::generateAtvFrame(image, configuration, fmVideo);
    if (atvFrame.isEmpty()) {
        setStatus(text(QStringLiteral("Could not generate the analog video frame"),
                       QString::fromUtf8(u8"Не вдалося сформувати кадр аналогового відео")), true);
        return;
    }
    // The raster repeats unchanged. Filter it once instead of running the
    // relatively expensive bandwidth limiter for every 18 ms frame while the
    // HackRF callback is already consuming the source queue.
    QVector<std::complex<float>> filteredAtvFrame;
    TxBandwidthLimiterState atvLimiterState;
    if (TransmitWaveformGenerator::limitSignalBandwidth(
            configuration, atvFrame.constData(), atvFrame.size(),
            &filteredAtvFrame, &atvLimiterState)) {
        atvFrame = std::move(filteredAtvFrame);
    }
    activeConfiguration = configuration;
    qInfo().noquote() << "[ATV TX] frame ready"
                      << "mode" << (fmVideo ? "FM" : "AM")
                      << "sampleRate" << configuration.sampleRate
                      << "signalBandwidthHz" << configuration.signalBandwidthHz
                      << "deviationHz" << configuration.deviationHz
                      << "samples" << atvFrame.size()
                      << "durationMs"
                      << (1000.0 * atvFrame.size() / configuration.sampleRate);
    QString error;
    if (!activeBackend || !activeBackend->start(configuration, &error)) {
        armCheck->setChecked(false);
        setStatus(error, true);
        return;
    }
    mediaAudio.clear();
    mediaPosition = 0;
    mediaPaused = false;
    plannedDurationSeconds = activeBackend->isRfCapable() ? watchdogSpin->value() : 180.0;
    textSimulation = false;
    elapsedTimer.restart();
    progressTimer->start();
    const int frameMs = (std::max)(1, qRound(1000.0 * atvFrame.size() / configuration.sampleRate));
    sourceTimer->setInterval(frameMs);
    for (int i = 0; i < 6 && activeBackend && activeBackend->isRunning(); ++i) processLiveSource();
    if (activeBackend == &hackRf) {
        qInfo().noquote() << "[ATV TX] initial queue"
                          << "bufferedSamples" << hackRf.bufferedSourceSamples()
                          << "streamStarted" << hackRf.streamStarted()
                          << "hardwareAlive" << hackRf.hardwareStreamAlive();
    }
    sourceTimer->start();
    startButton->setEnabled(false);
    stopButton->setEnabled(true);
    sourceCombo->setEnabled(false);
    preview->setIq(atvFrame);
    setStatus(text(QStringLiteral("Analog video test frame is being transmitted continuously."),
                   QString::fromUtf8(u8"Тестовий кадр аналогового відео передається циклічно.")));
}

bool TransmitDialog::configureDmrEncoder(const TxConfiguration &configuration, QString *error) {
    if (!dmrEncoder) dmrEncoder = std::make_unique<DmrTransmitEncoder>();
    DmrTxParameters parameters;
    parameters.outputSampleRate = configuration.sampleRate;
    parameters.level = configuration.level;
    parameters.colorCode = configuration.dmrColorCode;
    parameters.timeslot = configuration.dmrTimeslot;
    parameters.sourceId = configuration.dmrSourceId;
    parameters.destinationId = configuration.dmrDestinationId;
    parameters.privateCall = configuration.dmrPrivateCall;
    parameters.privacyMode = configuration.dmrPrivacyMode;
    parameters.privacyKeyId = configuration.dmrPrivacyKeyId;
    parameters.privacyKey = configuration.dmrPrivacyKey;
    dmrCallActive = false;
    const bool configured = dmrEncoder->configure(parameters, error);
    qInfo().noquote() << "[DMR TX] configure"
                      << "ok" << configured
                      << "sampleRate" << parameters.outputSampleRate
                      << "CC" << parameters.colorCode
                      << "TS" << parameters.timeslot
                      << "source" << parameters.sourceId
                      << "destination" << parameters.destinationId
                      << "call" << (parameters.privateCall ? "private" : "group")
                      << "privacy"
                      << (parameters.privacyMode == DMR_PRIVACY_ARC4
                              ? "ARC4"
                              : (parameters.privacyMode == DMR_PRIVACY_AES256 ? "AES-256" : "none"))
                      << "KID" << parameters.privacyKeyId;
    return configured;
}

bool TransmitDialog::beginDmrCall(QString *error) {
    if (!dmrEncoder) {
        if (error) *error = QStringLiteral("DMR encoder is not configured");
        return false;
    }
    // PTT keeps the RF backend open between calls, so DMR controls remain
    // editable while idle. Re-read them for every call instead of retaining
    // the privacy/key state that was active when the backend first started.
    const TxConfiguration uiConfiguration = configurationFromUi();
    activeConfiguration.dmrColorCode = uiConfiguration.dmrColorCode;
    activeConfiguration.dmrTimeslot = uiConfiguration.dmrTimeslot;
    activeConfiguration.dmrSourceId = uiConfiguration.dmrSourceId;
    activeConfiguration.dmrDestinationId = uiConfiguration.dmrDestinationId;
    activeConfiguration.dmrPrivateCall = uiConfiguration.dmrPrivateCall;
    activeConfiguration.dmrPrivacyMode = uiConfiguration.dmrPrivacyMode;
    activeConfiguration.dmrPrivacyKeyId = uiConfiguration.dmrPrivacyKeyId;
    activeConfiguration.dmrPrivacyKey = uiConfiguration.dmrPrivacyKey;
    if (!configureDmrEncoder(activeConfiguration, error)) return false;
    const QVector<std::complex<float>> iq = dmrEncoder->begin(error);
    if (iq.isEmpty()) return false;
    if (!writeActiveIq(iq.constData(), iq.size(), error)) return false;
    dmrCallActive = true;
    preview->setIq(iq);
    qInfo().noquote() << "[DMR TX] call begin"
                      << "iqSamples" << iq.size()
                      << "MI"
                      << QStringLiteral("0x%1").arg(dmrEncoder->currentPrivacyMiValue(), 8, 16,
                                                    QLatin1Char('0')).toUpper()
                      << "durationMs"
                      << (1000.0 * static_cast<double>(iq.size()) /
                          static_cast<double>(activeConfiguration.sampleRate));
    return true;
}

bool TransmitDialog::finishDmrCall(QString *error) {
    if (!dmrCallActive || !dmrEncoder || dmrFinishing) return true;
    dmrFinishing = true;
    const QVector<std::complex<float>> iq = dmrEncoder->finish(error);
    bool ok = true;
    if (!iq.isEmpty()) {
        ok = writeActiveIq(iq.constData(), iq.size(), error);
        if (ok && activeBackend) ok = activeBackend->drain(2000, error);
        if (ok) preview->setIq(iq);
    } else if (error && !error->isEmpty()) {
        ok = false;
    }
    dmrCallActive = false;
    dmrFinishing = false;
    qInfo().noquote() << "[DMR TX] call end"
                      << "ok" << ok
                      << "iqSamples" << iq.size()
                      << "durationMs"
                      << (1000.0 * static_cast<double>(iq.size()) /
                          static_cast<double>(activeConfiguration.sampleRate));
    return ok;
}

QVector<std::complex<float>> TransmitDialog::modulateActiveAudio(const QVector<float> &audio,
                                                                 QString *error) {
    if (activeConfiguration.modulation == TxModulation::Dmr) {
        if (!dmrCallActive || !dmrEncoder) return {};
        return dmrEncoder->pushAudio(audio, error);
    }
    return TransmitWaveformGenerator::modulateAudio(activeConfiguration, audio, &modulatorState);
}

void TransmitDialog::processLiveSource() {
    if (mediaPaused || !activeBackend || !activeBackend->isRunning()) return;
    const QString source = sourceCombo->currentData().toString();
    QString error;
    if (source == QStringLiteral("manual-cw")) {
        if (!liveKeyPressed) return;
        const int count = (std::max)(1, activeConfiguration.sampleRate / 50);
        const int target = activeBackend == &hackRf ? activeConfiguration.sampleRate / 4 : count;
        int guard = 0;
        while (activeBackend && activeBackend->isRunning() &&
               (activeBackend != &hackRf || hackRf.bufferedSourceSamples() < target) &&
               guard++ < 16) {
            QVector<std::complex<float>> iq(count);
            const double step = 2.0 * 3.14159265358979323846 * activeConfiguration.toneHz /
                                activeConfiguration.sampleRate;
            for (int i = 0; i < count; ++i) {
                iq[i] = std::polar(activeConfiguration.level, static_cast<float>(manualCwPhase));
                manualCwPhase = std::remainder(manualCwPhase + step, 2.0 * 3.14159265358979323846);
            }
            if (!writeActiveIq(iq.constData(), iq.size(), &error)) {
                stopTransmission(error);
                return;
            }
            preview->setIq(iq);
        }
        return;
    }
    if (source == QStringLiteral("audio-file")) {
        if (mediaPosition >= mediaAudio.size()) {
            if (activeBackend == &hackRf && hackRf.bufferedSourceSamples() > 1) return;
            stopTransmission(text(QStringLiteral("Audio file transmission complete"),
                                  QString::fromUtf8(u8"Передачу аудіофайлу завершено")));
            return;
        }
        const int target = activeBackend == &hackRf ? activeConfiguration.sampleRate / 2
                                                     : activeConfiguration.sampleRate / 50;
        int guard = 0;
        while (mediaPosition < mediaAudio.size() && activeBackend && activeBackend->isRunning() &&
               (activeBackend != &hackRf || hackRf.bufferedSourceSamples() < target) &&
               guard++ < 16) {
            const int count = (std::min)(activeConfiguration.sampleRate / 50,
                                         mediaAudio.size() - mediaPosition);
            QVector<float> chunk(count);
            std::copy_n(mediaAudio.constData() + mediaPosition, count, chunk.data());
            mediaPosition += count;
            const QVector<std::complex<float>> iq = modulateActiveAudio(chunk, &error);
            if (!error.isEmpty()) {
                stopTransmission(error);
                return;
            }
            if (!iq.isEmpty() && !writeActiveIq(iq.constData(), iq.size(), &error)) {
                stopTransmission(error);
                return;
            }
            preview->setIq(iq);
        }
        return;
    }
    if (source == QStringLiteral("image-atv") && !atvFrame.isEmpty()) {
        const int target = activeBackend == &hackRf
                               ? activeConfiguration.sampleRate
                               : atvFrame.size();
        int guard = 0;
        do {
            if (!activeBackend->writeIq(atvFrame.constData(), atvFrame.size(), &error)) {
                stopTransmission(error);
                return;
            }
            ++guard;
        } while (activeBackend == &hackRf && activeBackend->isRunning() &&
                 hackRf.bufferedSourceSamples() < target && guard < 128);
    }
}

void TransmitDialog::setLiveKey(bool pressed) {
    if (pressed == liveKeyPressed || !activeBackend || !activeBackend->isRunning()) return;
    QString error;
    const bool dmrMode = activeConfiguration.modulation == TxModulation::Dmr;
    if (pressed) {
        if (!activeBackend->setOutputEnabled(true, &error)) {
            stopTransmission(error);
            return;
        }
        bandwidthLimiterState.reset();
        if (dmrMode && !beginDmrCall(&error)) {
            stopTransmission(error);
            return;
        }
    } else {
        if (dmrMode && !finishDmrCall(&error)) {
            stopTransmission(error);
            return;
        }
        if (!activeBackend->setOutputEnabled(false, &error)) {
            stopTransmission(error);
            return;
        }
    }
    liveKeyPressed = pressed;
    liveKeyButton->setText(pressed
                               ? text(QStringLiteral("TRANSMITTING - release to stop"),
                                      QString::fromUtf8(u8"ПЕРЕДАЧА - відпустіть для зупинки"))
                               : text(QStringLiteral("PTT / CW KEY — hold Space"),
                                      QString::fromUtf8(u8"PTT / КЛЮЧ CW — утримуйте Пробіл")));
    liveKeyButton->setStyleSheet(
        pressed ? QStringLiteral("background:#b3262e;color:white;font-weight:700;")
                : QStringLiteral("background:#218c4c;color:white;font-weight:600;"));
    if (sourceCombo->currentData().toString() == QStringLiteral("manual-cw")) {
        if (pressed) {
            sourceTimer->setInterval(10);
            processLiveSource();
            sourceTimer->start();
        } else {
            sourceTimer->stop();
        }
    }
    setStatus(pressed
                  ? text(QStringLiteral("PTT active"), QString::fromUtf8(u8"PTT активний"))
                  : text(QStringLiteral("PTT released; RF output muted"),
                         QString::fromUtf8(u8"PTT відпущено; RF-вихід заглушено")));
}

void TransmitDialog::chooseMediaFile() {
    const QString source = sourceCombo->currentData().toString();
    const QString filter = source == QStringLiteral("audio-file")
                               ? QStringLiteral("Wave audio (*.wav)")
                               : QStringLiteral("Images (*.png *.jpg *.jpeg *.bmp *.webp);;All files (*)");
    const QString path = QFileDialog::getOpenFileName(
        this,
        text(QStringLiteral("Select transmission media"), QString::fromUtf8(u8"Оберіть медіа для передачі")),
        mediaPath.isEmpty() ? QDir::homePath() : QFileInfo(mediaPath).absolutePath(),
        filter);
    if (path.isEmpty()) return;
    mediaPath = path;
    preparedMediaImage = QImage();
    mediaPathLabel->setText(QFileInfo(mediaPath).fileName());
    mediaEditButton->setEnabled(source != QStringLiteral("audio-file"));
    if (source != QStringLiteral("audio-file")) editMediaImage();
    saveSettings();
}

void TransmitDialog::editMediaImage() {
    const QImage sourceImage(mediaPath);
    if (sourceImage.isNull()) {
        setStatus(text(QStringLiteral("Could not open the selected image"),
                       QString::fromUtf8(u8"Не вдалося відкрити вибране зображення")), true);
        return;
    }
    const bool atv = sourceCombo->currentData().toString() == QStringLiteral("image-atv");
    const QString mode = mediaModeCombo->currentData().toString();
    const QSize target = atv ? QSize(384, 288)
                             : QSize(320, mode.startsWith(QStringLiteral("martin-")) ? 256 : 240);
    SstvImageEditorDialog editor(sourceImage, target, language, this);
    if (editor.exec() == QDialog::Accepted) {
        preparedMediaImage = editor.preparedImage();
        setStatus(text(QStringLiteral("Image prepared for %1 x %2 transmission frame"),
                       QString::fromUtf8(u8"Зображення підготовлено до кадру передачі %1 x %2"))
                      .arg(target.width())
                      .arg(target.height()));
    }
}

void TransmitDialog::readMicrophone() {
#ifdef FOBOSAPP_HAS_QT_MULTIMEDIA
    if (!audioDevice || !activeBackend || !activeBackend->isRunning()) return;
    processMicrophoneBytes(audioDevice->readAll());
#endif
}

void TransmitDialog::processMicrophoneBytes(const QByteArray &newBytes) {
#ifdef FOBOSAPP_HAS_QT_MULTIMEDIA
    if (newBytes.isEmpty() || !activeBackend || !activeBackend->isRunning()) return;
    audioCapturePendingBytes.append(newBytes);
    const int bytesPerValue = audioCaptureSampleSize / 8;
    const int bytesPerFrame = bytesPerValue * audioCaptureChannels;
    const int samples = bytesPerFrame > 0 ? audioCapturePendingBytes.size() / bytesPerFrame : 0;
    if (samples <= 0) return;
    const int completeBytes = samples * bytesPerFrame;
    const QByteArray bytes = audioCapturePendingBytes.left(completeBytes);
    audioCapturePendingBytes.remove(0, completeBytes);
    if (pttMode && !liveKeyPressed) {
        audioCapturePendingBytes.clear();
        return;
    }
    QVector<float> audio(samples);
    const char *data = bytes.constData();
    double sumSquares = 0.0;
    double rawSum = 0.0;
    float peak = 0.0f;
    qint64 nonZeroFrames = 0;
    const float dcAlpha = static_cast<float>(
        std::exp(-2.0 * 3.14159265358979323846 * 35.0 /
                 (std::max)(8000, activeConfiguration.sampleRate)));
    bool decodeEightBitAsUnsigned =
        audioCaptureSampleType == static_cast<int>(QAudioFormat::UnSignedInt);
#ifdef Q_OS_WIN
    // WAVE_FORMAT_PCM defines 8-bit samples as unsigned. Some Qt 5 Windows
    // plugins nevertheless label the endpoint as SignedInt.
    decodeEightBitAsUnsigned = true;
#endif
    for (int frame = 0; frame < samples; ++frame) {
        float mixed = 0.0f;
        for (int channel = 0; channel < audioCaptureChannels; ++channel) {
            const char *value = data + frame * bytesPerFrame + channel * bytesPerValue;
            float sample = 0.0f;
            if (audioCaptureSampleSize == 8 && decodeEightBitAsUnsigned) {
                sample = (static_cast<int>(static_cast<unsigned char>(value[0])) - 128) / 128.0f;
            } else if (audioCaptureSampleSize == 8) {
                sample = static_cast<float>(static_cast<qint8>(value[0])) / 128.0f;
            } else if (audioCaptureSampleSize == 16) {
                const quint16 raw = audioCaptureLittleEndian
                                        ? static_cast<quint16>(static_cast<unsigned char>(value[0])) |
                                              (static_cast<quint16>(static_cast<unsigned char>(value[1])) << 8)
                                        : static_cast<quint16>(static_cast<unsigned char>(value[1])) |
                                              (static_cast<quint16>(static_cast<unsigned char>(value[0])) << 8);
                sample = static_cast<float>(static_cast<qint16>(raw)) / 32768.0f;
            } else if (audioCaptureSampleSize == 24) {
                quint32 raw = audioCaptureLittleEndian
                                  ? static_cast<quint32>(static_cast<unsigned char>(value[0])) |
                                        (static_cast<quint32>(static_cast<unsigned char>(value[1])) << 8) |
                                        (static_cast<quint32>(static_cast<unsigned char>(value[2])) << 16)
                                  : static_cast<quint32>(static_cast<unsigned char>(value[2])) |
                                        (static_cast<quint32>(static_cast<unsigned char>(value[1])) << 8) |
                                        (static_cast<quint32>(static_cast<unsigned char>(value[0])) << 16);
                if (raw & 0x00800000u) raw |= 0xff000000u;
                sample = static_cast<float>(static_cast<qint32>(raw) / 8388608.0);
            } else if (audioCaptureSampleType == static_cast<int>(QAudioFormat::Float)) {
                unsigned char ordered[4];
                for (int byte = 0; byte < 4; ++byte) {
                    ordered[byte] = static_cast<unsigned char>(
                        value[audioCaptureLittleEndian ? byte : 3 - byte]);
                }
                float raw = 0.0f;
                std::memcpy(&raw, ordered, sizeof(raw));
                sample = (std::clamp)(raw, -1.0f, 1.0f);
            } else {
                quint32 raw = 0;
                for (int byte = 0; byte < 4; ++byte) {
                    raw |= static_cast<quint32>(static_cast<unsigned char>(
                               value[audioCaptureLittleEndian ? byte : 3 - byte]))
                           << (8 * byte);
                }
                sample = static_cast<float>(static_cast<qint32>(raw) / 2147483648.0);
            }
            mixed += sample;
        }
        const float rawMixed = mixed / static_cast<float>(audioCaptureChannels);
        rawSum += rawMixed;
        const float dcBlocked = rawMixed - microphoneDcPreviousInput +
                                dcAlpha * microphoneDcPreviousOutput;
        microphoneDcPreviousInput = rawMixed;
        microphoneDcPreviousOutput = dcBlocked;
        audio[frame] = softLimitMicrophone(dcBlocked * microphoneSoftwareGain);
        sumSquares += static_cast<double>(dcBlocked) * dcBlocked;
        peak = (std::max)(peak, std::abs(dcBlocked));
        if (std::abs(dcBlocked) > 1.0e-7f) ++nonZeroFrames;
    }
    const float rms = static_cast<float>(std::sqrt(sumSquares / samples));
    microphoneRmsDb = 20.0f * std::log10((std::max)(rms, 1.0e-6f));
    microphonePeakDb = 20.0f * std::log10((std::max)(peak, 1.0e-6f));
    microphoneDiagnosticFrames += samples;
    microphoneDiagnosticNonZeroFrames += nonZeroFrames;
    if (!microphoneDiagnosticTimer.isValid() || microphoneDiagnosticTimer.elapsed() >= 1000) {
        qDebug().noquote() << "[TX audio] microphone PCM"
                           << "frames" << microphoneDiagnosticFrames
                           << "nonZero" << microphoneDiagnosticNonZeroFrames
                           << "rmsDbfs" << microphoneRmsDb
                           << "peakDbfs" << microphonePeakDb
                           << "softwareBoostDb"
                           << (std::max)(0, microphoneLevelSlider->value() - 100)
                           << "rawDc" << (rawSum / samples)
                           << "pendingBytes" << audioCapturePendingBytes.size();
        if (!microphoneSilenceWarned && microphoneDiagnosticFrames >= 8000 &&
            microphonePeakDb < -80.0f) {
            microphoneSilenceWarned = true;
            const QString warning = text(
                QStringLiteral("Windows is returning digital silence from the selected microphone. Check the hardware microphone-mute key, Windows Privacy & security > Microphone, and the input level in Sound settings."),
                QString::fromUtf8(u8"Windows повертає цифрову тишу з обраного мікрофона. Перевірте апаратну кнопку вимкнення мікрофона, Конфіденційність і безпека > Мікрофон та рівень входу в параметрах звуку."));
            qWarning().noquote() << "[TX audio]" << warning;
            setStatus(warning, true);
        }
        microphoneDiagnosticFrames = 0;
        microphoneDiagnosticNonZeroFrames = 0;
        microphoneDiagnosticTimer.restart();
    }
    QString error;
    const QVector<std::complex<float>> iq = modulateActiveAudio(audio, &error);
    if (!error.isEmpty()) {
        stopTransmission(error);
        return;
    }
    if (!iq.isEmpty() && !writeActiveIq(iq.constData(), iq.size(), &error)) {
        stopTransmission(error);
        return;
    }
    preview->setIq(iq);
    if (activeBackend == &simulator) {
        statsLabel->setText(text(QStringLiteral("Captured %1 IQ samples (%2 s)"),
                                 QString::fromUtf8(u8"Захоплено %1 IQ-семплів (%2 с)"))
                                .arg(simulator.capturedIq().size())
                                .arg(static_cast<double>(simulator.capturedIq().size()) /
                                         simulator.configuration().sampleRate,
                                     0, 'f', 2));
    }
#endif
}

void TransmitDialog::updateProgress() {
    if (!activeBackend || !activeBackend->isRunning()) {
        progressTimer->stop();
        return;
    }
    const double elapsed = elapsedTimer.elapsed() / 1000.0;
    const double denominator = (std::max)(0.001, plannedDurationSeconds);
    const bool audioFile = sourceCombo->currentData().toString() == QStringLiteral("audio-file") &&
                           !mediaAudio.isEmpty();
    const double progress = audioFile
                                ? static_cast<double>(mediaPosition) / mediaAudio.size()
                                : elapsed / denominator;
    progressBar->setValue((std::min)(1000, qRound(1000.0 * progress)));
    if (activeBackend == &hackRf) {
        if (hackRf.streamStarted() && !hackRf.hardwareStreamAlive()) {
            ++hackRfStoppedStreamPolls;
            if (hackRfStoppedStreamPolls >= 3) {
                qWarning().noquote()
                    << "[HackRFTX] USB stream stopped unexpectedly"
                    << "outputSamples" << hackRf.outputSamples()
                    << "bufferedSourceSamples" << hackRf.bufferedSourceSamples();
                armCheck->setChecked(false);
                stopTransmission(text(
                    QStringLiteral("HackRF USB stream stopped unexpectedly. Reduce the TX device sample rate or move the receiver to another USB controller."),
                    QString::fromUtf8(u8"USB-потік HackRF несподівано зупинився. Зменште семплрейт передавача або перенесіть приймач на інший USB-контролер.")));
                return;
            }
        } else {
            hackRfStoppedStreamPolls = 0;
        }
        const double seconds = hackRf.configuration().deviceSampleRate > 0
                                   ? static_cast<double>(hackRf.outputSamples()) /
                                         hackRf.configuration().deviceSampleRate
                                   : 0.0;
        statsLabel->setText(
            text(QStringLiteral("RF: %1 M samples | source buffer: %2 | zero-fill: %3"),
                 QString::fromUtf8(u8"RF: %1 млн семплів | буфер джерела: %2 | нульове заповнення: %3"))
                .arg(hackRf.outputSamples() / 1000000.0, 0, 'f', 2)
                .arg(hackRf.bufferedSourceSamples())
                .arg(hackRf.sourceUnderrunSamples()) +
            QStringLiteral(" | ") + signalShapeSummary(activeConfiguration) +
            ((sourceCombo->currentData().toString() == QStringLiteral("microphone") ||
              sourceCombo->currentData().toString() == QStringLiteral("microphone-ptt"))
                 ? text(QStringLiteral(" | mic RMS %1 dBFS / peak %2 dBFS"),
                        QString::fromUtf8(u8" | мікр. RMS %1 dBFS / пік %2 dBFS"))
                       .arg(microphoneRmsDb, 0, 'f', 1)
                       .arg(microphonePeakDb, 0, 'f', 1)
                 : QString()));
        Q_UNUSED(seconds)
    }
    double safetyLimit = activeBackend->isRfCapable() ? watchdogSpin->value() : 180.0;
    if (textSimulation) {
        // Finite generated signals (notably SSTV) must not be truncated by a
        // shorter generic RF watchdog. Keep a small drain margin after the
        // waveform's measured duration while retaining the watchdog for live TX.
        safetyLimit = (std::max)(safetyLimit, plannedDurationSeconds + 5.0);
    }
    if ((textSimulation && elapsed >= plannedDurationSeconds) || elapsed >= safetyLimit) {
        stopTransmission(textSimulation
                             ? text(activeBackend->isRfCapable() ? QStringLiteral("Transmission complete")
                                                                : QStringLiteral("Simulation complete"),
                                    activeBackend->isRfCapable() ? QString::fromUtf8(u8"Передачу завершено")
                                                                : QString::fromUtf8(u8"Симуляцію завершено"))
                             : text(QStringLiteral("RF safety timer reached"),
                                    QString::fromUtf8(u8"Спрацював таймер безпеки RF")));
    }
}

void TransmitDialog::stopTransmission(const QString &reason) {
    progressTimer->stop();
    hackRfStoppedStreamPolls = 0;
    if (ft8StartTimer) ft8StartTimer->stop();
    pendingTextIq.clear();
    pendingTextIq.squeeze();
    sourceTimer->stop();
#ifdef FOBOSAPP_HAS_QT_MULTIMEDIA
    if (audioInput) {
        audioInput->disconnect(this);
        audioInput->stop();
        delete audioInput;
        audioInput = nullptr;
        audioDevice = nullptr;
    }
    audioCapturePendingBytes.clear();
    microphoneDiagnosticFrames = 0;
    microphoneDiagnosticNonZeroFrames = 0;
    microphoneDcPreviousInput = 0.0f;
    microphoneDcPreviousOutput = 0.0f;
    microphoneSilenceWarned = false;
#endif
#ifdef _WIN32
    if (windowsMicrophoneCapture) {
        windowsMicrophoneCapture->stop();
        delete windowsMicrophoneCapture;
        windowsMicrophoneCapture = nullptr;
    }
#endif
    const bool hadSamples = activeBackend == &simulator && !simulator.capturedIq().isEmpty();
    QString dmrError;
    if (activeBackend && activeBackend->isRunning() && dmrCallActive) {
        finishDmrCall(&dmrError);
    }
    if (activeBackend) activeBackend->stop();
    activeBackend = nullptr;
    if (dmrEncoder) dmrEncoder->reset();
    dmrCallActive = false;
    dmrFinishing = false;
    liveKeyPressed = false;
    pttMode = false;
    microphoneRmsDb = -120.0f;
    microphonePeakDb = -120.0f;
    automaticFt8Active = false;
    bandwidthLimiterState.reset();
    bandwidthLimitedIq.clear();
    mediaPaused = false;
    mediaAudio.clear();
    atvFrame.clear();
    mediaPosition = 0;
    startButton->setEnabled(true);
    stopButton->setEnabled(false);
    sourceCombo->setEnabled(true);
    liveKeyButton->setEnabled(false);
    liveKeyButton->setText(text(QStringLiteral("Start transmission"),
                                QString::fromUtf8(u8"Почати передачу")));
    liveKeyButton->setStyleSheet(
        QStringLiteral("background:#6b7078;color:white;font-weight:600;"));
    exportButton->setEnabled(hadSamples);
    if (!reason.isEmpty()) setStatus(reason);
    else if (!dmrError.isEmpty()) setStatus(dmrError, true);
}

void TransmitDialog::exportIq() {
    const QVector<std::complex<float>> &iq = simulator.capturedIq();
    if (iq.isEmpty()) return;
    QSettings settings(persistentSettingsFilePath(), QSettings::IniFormat);
    const QString initial = settings.value(QStringLiteral("transmit/lastExportDirectory"),
                                           QDir::homePath()).toString();
    const QString suggested = QStringLiteral("tx_%1_%2.cf32")
                                  .arg(TransmitWaveformGenerator::modulationName(simulator.configuration().modulation)
                                           .toLower().replace('/', '_').replace(' ', '_'))
                                  .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd_HHmmss")));
    const QString path = QFileDialog::getSaveFileName(
        this,
        text(QStringLiteral("Export complex float IQ"), QString::fromUtf8(u8"Експорт комплексного float IQ")),
        QDir(initial).filePath(suggested),
        QStringLiteral("Complex float IQ (*.cf32)"));
    if (path.isEmpty()) return;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        setStatus(file.errorString(), true);
        return;
    }
    QByteArray buffer;
    buffer.resize(iq.size() * static_cast<int>(sizeof(float)) * 2);
    float *output = reinterpret_cast<float *>(buffer.data());
    for (int i = 0; i < iq.size(); ++i) {
        output[2 * i] = iq[i].real();
        output[2 * i + 1] = iq[i].imag();
    }
    if (file.write(buffer) != buffer.size() || !file.commit()) {
        setStatus(file.errorString(), true);
        return;
    }
    const QFileInfo info(path);
    settings.setValue(QStringLiteral("transmit/lastExportDirectory"), info.absolutePath());
    QJsonObject metadata;
    metadata.insert(QStringLiteral("format"), QStringLiteral("cf32_le"));
    metadata.insert(QStringLiteral("sample_rate"), simulator.configuration().sampleRate);
    metadata.insert(QStringLiteral("frequency_hz"), simulator.configuration().frequencyHz);
    metadata.insert(QStringLiteral("signal_bandwidth_hz"),
                    simulator.configuration().signalBandwidthHz);
    metadata.insert(QStringLiteral("modulation"),
                    TransmitWaveformGenerator::modulationName(simulator.configuration().modulation));
    metadata.insert(QStringLiteral("sample_count"), iq.size());
    metadata.insert(QStringLiteral("duration_seconds"),
                    static_cast<double>(iq.size()) / simulator.configuration().sampleRate);
    metadata.insert(QStringLiteral("rf_output"), false);
    QSaveFile sidecar(info.absolutePath() + QLatin1Char('/') + info.completeBaseName() + QStringLiteral(".json"));
    if (sidecar.open(QIODevice::WriteOnly)) {
        sidecar.write(QJsonDocument(metadata).toJson(QJsonDocument::Indented));
        sidecar.commit();
    }
    setStatus(text(QStringLiteral("IQ exported: %1"), QString::fromUtf8(u8"IQ експортовано: %1")).arg(path));
}

void TransmitDialog::setStatus(const QString &message, bool error) {
    statusLabel->setText(message);
    statusLabel->setStyleSheet(error ? QStringLiteral("color: #d9534f;") : QString());
}

void TransmitDialog::loadSettings() {
    QSettings settings(persistentSettingsFilePath(), QSettings::IniFormat);
    amDepthPercent = (std::clamp)(
        settings.value(QStringLiteral("transmit/amDepthPercent"), 90).toInt(), 0, 100);
    askDepthPercent = (std::clamp)(
        settings.value(QStringLiteral("transmit/askDepthPercent"), 70).toInt(), 0, 100);
    frequencySpin->setValue(settings.value(QStringLiteral("transmit/frequencyMhz"), 144.300).toDouble());
    const int mode = settings.value(QStringLiteral("transmit/modulation"),
                                    static_cast<int>(TxModulation::Nfm)).toInt();
    const int modeIndex = modulationCombo->findData(mode);
    if (modeIndex >= 0) modulationCombo->setCurrentIndex(modeIndex);
    const QString savedAudioInput = settings.value(QStringLiteral("transmit/audioInputDevice")).toString();
    const int savedAudioInputIndex = audioInputCombo->findData(savedAudioInput);
    if (savedAudioInputIndex >= 0) audioInputCombo->setCurrentIndex(savedAudioInputIndex);
    microphoneLevelSlider->setValue(
        settings.value(QStringLiteral("transmit/microphoneLevelBoost"), 100).toInt());
    updateMicrophoneLevelText();
    const int sampleRate = settings.value(QStringLiteral("transmit/sampleRate"), 48000).toInt();
    int rateIndex = sampleRateCombo->findData(sampleRate);
    if (rateIndex < 0) {
        sampleRateCombo->addItem(signalunits::sampleRateText(sampleRate), sampleRate);
        rateIndex = sampleRateCombo->count() - 1;
    }
    sampleRateCombo->setCurrentIndex(rateIndex);
    levelSpin->setValue(settings.value(QStringLiteral("transmit/levelPercent"), 50).toInt());
    deviationSpin->setValue(settings.value(QStringLiteral("transmit/deviationHz"), 2500.0).toDouble());
    const double savedSignalBandwidth =
        settings.value(QStringLiteral("transmit/signalBandwidthHz"), 0.0).toDouble();
    int signalBandwidthIndex = -1;
    for (int index = 0; index < signalBandwidthCombo->count(); ++index) {
        if (std::abs(signalBandwidthCombo->itemData(index).toDouble() - savedSignalBandwidth) < 0.5) {
            signalBandwidthIndex = index;
            break;
        }
    }
    if (signalBandwidthIndex >= 0) {
        signalBandwidthCombo->setCurrentIndex(signalBandwidthIndex);
    } else {
        signalBandwidthCombo->setCurrentIndex(-1);
        signalBandwidthCombo->setEditText(QStringLiteral("%1 Hz").arg(savedSignalBandwidth, 0, 'f', 0));
    }
    toneSpin->setValue(settings.value(QStringLiteral("transmit/toneHz"), 700.0).toDouble());
    cwWpmSpin->setValue(settings.value(QStringLiteral("transmit/cwWpm"), 18).toInt());
    symbolRateSpin->setValue(settings.value(QStringLiteral("transmit/symbolRate"),
                                            symbolRateSpin->value()).toDouble());
    gaussianBtSpin->setValue(settings.value(QStringLiteral("transmit/gaussianBt"),
                                             gaussianBtSpin->value()).toDouble());
    modulationDepthSpin->setValue(selectedModulation() == TxModulation::Ask
                                      ? askDepthPercent
                                      : amDepthPercent);
    invertDataCheck->setChecked(settings.value(QStringLiteral("transmit/invertData"), false).toBool());
    ft8UtcCheck->setChecked(settings.value(QStringLiteral("transmit/alignFt8Utc"), true).toBool());
    const int ft8SlotIndex = ft8SlotCombo->findData(settings.value(QStringLiteral("transmit/ft8SlotParity"), -1).toInt());
    if (ft8SlotIndex >= 0) ft8SlotCombo->setCurrentIndex(ft8SlotIndex);
    dmrColorCodeSpin->setValue(settings.value(QStringLiteral("transmit/dmrColorCode"), 1).toInt());
    const int dmrTimeslot = settings.value(QStringLiteral("transmit/dmrTimeslot"), 1).toInt();
    const int dmrTimeslotIndex = dmrTimeslotCombo->findData(dmrTimeslot);
    if (dmrTimeslotIndex >= 0) dmrTimeslotCombo->setCurrentIndex(dmrTimeslotIndex);
    dmrSourceIdSpin->setValue(settings.value(QStringLiteral("transmit/dmrSourceId"), 1).toInt());
    dmrDestinationIdSpin->setValue(settings.value(QStringLiteral("transmit/dmrDestinationId"), 9).toInt());
    const bool dmrPrivateCall = settings.value(QStringLiteral("transmit/dmrPrivateCall"), false).toBool();
    const int dmrCallTypeIndex = dmrCallTypeCombo->findData(dmrPrivateCall);
    if (dmrCallTypeIndex >= 0) dmrCallTypeCombo->setCurrentIndex(dmrCallTypeIndex);
    const int dmrPrivacyMode = settings.value(QStringLiteral("transmit/dmrPrivacyMode"),
                                              DMR_PRIVACY_NONE).toInt();
    const int dmrPrivacyIndex = dmrPrivacyCombo->findData(dmrPrivacyMode);
    if (dmrPrivacyIndex >= 0) dmrPrivacyCombo->setCurrentIndex(dmrPrivacyIndex);
    dmrPrivacyKeyIdSpin->setValue(settings.value(QStringLiteral("transmit/dmrPrivacyKeyId"), 0).toInt());
    dmrPrivacyKeyEdit->setText(settings.value(QStringLiteral("transmit/dmrPrivacyKeyHex")).toString());
    const QString backend = settings.value(QStringLiteral("transmit/backend"),
                                           QStringLiteral("simulator-iq-file")).toString();
    const int backendIndex = backendCombo->findData(backend);
    if (backendIndex >= 0) backendCombo->setCurrentIndex(backendIndex);
    const int deviceIndex = settings.value(QStringLiteral("transmit/hackRfDeviceIndex"), 0).toInt();
    const int deviceComboIndex = deviceCombo->findData(deviceIndex);
    if (deviceComboIndex >= 0) deviceCombo->setCurrentIndex(deviceComboIndex);
    const int deviceRate = settings.value(QStringLiteral("transmit/deviceSampleRate"), 2000000).toInt();
    const int deviceRateIndex = deviceSampleRateCombo->findData(deviceRate);
    if (deviceRateIndex >= 0) deviceSampleRateCombo->setCurrentIndex(deviceRateIndex);
    const uint bandwidth = settings.value(QStringLiteral("transmit/bandwidthHz"), 0).toUInt();
    const int bandwidthIndex = bandwidthCombo->findData(bandwidth);
    if (bandwidthIndex >= 0) bandwidthCombo->setCurrentIndex(bandwidthIndex);
    txGainSpin->setValue(settings.value(QStringLiteral("transmit/txVgaGainDb"), 0).toInt());
    rfAmpCheck->setChecked(settings.value(QStringLiteral("transmit/rfAmp"), false).toBool());
    watchdogSpin->setValue(settings.value(QStringLiteral("transmit/watchdogSeconds"), 30).toInt());
    armCheck->setChecked(false);
    messageEdit->setPlainText(settings.value(QStringLiteral("transmit/message"),
                                             QStringLiteral("CQ TEST KN34")).toString());
    const QString source = settings.value(QStringLiteral("transmit/source"),
                                          QStringLiteral("microphone")).toString();
    const int sourceIndex = sourceCombo->findData(source);
    if (sourceIndex >= 0) sourceCombo->setCurrentIndex(sourceIndex);
    mediaPath = settings.value(QStringLiteral("transmit/mediaPath")).toString();
    const QString mediaMode = settings.value(QStringLiteral("transmit/mediaMode"),
                                             QStringLiteral("robot36")).toString();
    const int mediaModeIndex = mediaModeCombo->findData(mediaMode);
    if (mediaModeIndex >= 0) mediaModeCombo->setCurrentIndex(mediaModeIndex);
    restoreGeometry(settings.value(QStringLiteral("transmit/dialogGeometry")).toByteArray());
}

void TransmitDialog::saveSettings() const {
    QSettings settings(persistentSettingsFilePath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("transmit/frequencyMhz"), frequencySpin->value());
    settings.setValue(QStringLiteral("transmit/modulation"), static_cast<int>(selectedModulation()));
    settings.setValue(QStringLiteral("transmit/sampleRate"), configurationFromUi().sampleRate);
    settings.setValue(QStringLiteral("transmit/audioInputDevice"),
                      audioInputCombo->currentData().toString());
    settings.setValue(QStringLiteral("transmit/microphoneLevelBoost"),
                      microphoneLevelSlider->value());
    settings.setValue(QStringLiteral("transmit/levelPercent"), levelSpin->value());
    settings.setValue(QStringLiteral("transmit/deviationHz"), deviationSpin->value());
    settings.setValue(QStringLiteral("transmit/signalBandwidthHz"), signalBandwidthFromUi());
    settings.setValue(QStringLiteral("transmit/toneHz"), toneSpin->value());
    settings.setValue(QStringLiteral("transmit/cwWpm"), cwWpmSpin->value());
    settings.setValue(QStringLiteral("transmit/symbolRate"), symbolRateSpin->value());
    settings.setValue(QStringLiteral("transmit/gaussianBt"), gaussianBtSpin->value());
    settings.setValue(QStringLiteral("transmit/amDepthPercent"), amDepthPercent);
    settings.setValue(QStringLiteral("transmit/askDepthPercent"), askDepthPercent);
    settings.setValue(QStringLiteral("transmit/invertData"), invertDataCheck->isChecked());
    settings.setValue(QStringLiteral("transmit/alignFt8Utc"), ft8UtcCheck->isChecked());
    settings.setValue(QStringLiteral("transmit/ft8SlotParity"), ft8SlotCombo->currentData().toInt());
    settings.setValue(QStringLiteral("transmit/dmrColorCode"), dmrColorCodeSpin->value());
    settings.setValue(QStringLiteral("transmit/dmrTimeslot"), dmrTimeslotCombo->currentData().toInt());
    settings.setValue(QStringLiteral("transmit/dmrSourceId"), dmrSourceIdSpin->value());
    settings.setValue(QStringLiteral("transmit/dmrDestinationId"), dmrDestinationIdSpin->value());
    settings.setValue(QStringLiteral("transmit/dmrPrivateCall"), dmrCallTypeCombo->currentData().toBool());
    settings.setValue(QStringLiteral("transmit/dmrPrivacyMode"), dmrPrivacyCombo->currentData().toInt());
    settings.setValue(QStringLiteral("transmit/dmrPrivacyKeyId"), dmrPrivacyKeyIdSpin->value());
    settings.setValue(QStringLiteral("transmit/dmrPrivacyKeyHex"), dmrPrivacyKeyEdit->text().trimmed());
    settings.setValue(QStringLiteral("transmit/backend"), backendCombo->currentData().toString());
    settings.setValue(QStringLiteral("transmit/hackRfDeviceIndex"), deviceCombo->currentData().toInt());
    settings.setValue(QStringLiteral("transmit/deviceSampleRate"), deviceSampleRateCombo->currentData().toInt());
    settings.setValue(QStringLiteral("transmit/bandwidthHz"), bandwidthCombo->currentData().toUInt());
    settings.setValue(QStringLiteral("transmit/txVgaGainDb"), txGainSpin->value());
    settings.setValue(QStringLiteral("transmit/rfAmp"), rfAmpCheck->isChecked());
    settings.setValue(QStringLiteral("transmit/watchdogSeconds"), watchdogSpin->value());
    settings.setValue(QStringLiteral("transmit/message"), messageEdit->toPlainText());
    settings.setValue(QStringLiteral("transmit/source"), sourceCombo->currentData().toString());
    settings.setValue(QStringLiteral("transmit/mediaPath"), mediaPath);
    settings.setValue(QStringLiteral("transmit/mediaMode"), mediaModeCombo->currentData().toString());
    settings.setValue(QStringLiteral("transmit/dialogGeometry"), saveGeometry());
}

void TransmitDialog::closeEvent(QCloseEvent *event) {
    armCheck->setChecked(false);
    stopTransmission();
    saveSettings();
    QDialog::closeEvent(event);
}

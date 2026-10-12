#include "signallabdialog.h"
#include "signalunits.h"

#include "appsettingsutils.h"
#include "signallabpreview.h"
#include "signallabconstellation3d.h"
#include "waterfall3dview.h"

#include <QApplication>
#include <QCheckBox>
#include <QHideEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

SignalLabDialog::SignalLabDialog(const QString &language,
                                 TransmitHandler transmitHandler,
                                 StopHandler stopHandler,
                                 QWidget *parent)
    : QDialog(parent),
      language(language),
      transmitHandler(std::move(transmitHandler)),
      stopHandler(std::move(stopHandler)) {
    setAttribute(Qt::WA_DeleteOnClose, false);
    resize(980, 690);
    buildUi();
    loadSettings();
    updateTexts();
    updateModeUi();
}

QString SignalLabDialog::text(const QString &english, const QString &ukrainian) const {
    return language.toLower().startsWith(QStringLiteral("uk")) ? ukrainian : english;
}

void SignalLabDialog::buildUi() {
    auto makeLabel = [this](const QString &english, const QString &ukrainian,
                            QWidget *parent) {
        QLabel *label = new QLabel(text(english, ukrainian), parent);
        label->setProperty("englishText", english);
        label->setProperty("ukrainianText", ukrainian);
        return label;
    };

    QVBoxLayout *root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(8);
    QSplitter *splitter = new QSplitter(Qt::Horizontal, this);

    QScrollArea *scroll = new QScrollArea(splitter);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    QWidget *parameters = new QWidget(scroll);
    QVBoxLayout *left = new QVBoxLayout(parameters);
    left->setContentsMargins(0, 0, 6, 0);

    QGroupBox *commonBox = new QGroupBox(parameters);
    commonBox->setObjectName(QStringLiteral("signalLabCommonBox"));
    QFormLayout *common = new QFormLayout(commonBox);
    waveformCombo = new QComboBox(commonBox);
    auto addWaveform = [this](const QString &name, SignalLabWaveform waveform) {
        waveformCombo->addItem(name, static_cast<int>(waveform));
    };
    addWaveform(QStringLiteral("Carrier"), SignalLabWaveform::Carrier);
    addWaveform(QStringLiteral("Sine / DSB"), SignalLabWaveform::Sine);
    addWaveform(QStringLiteral("Square"), SignalLabWaveform::Square);
    addWaveform(QStringLiteral("Triangle"), SignalLabWaveform::Triangle);
    addWaveform(QStringLiteral("Sawtooth"), SignalLabWaveform::Sawtooth);
    addWaveform(QStringLiteral("Pulse train"), SignalLabWaveform::Pulse);
    addWaveform(QStringLiteral("Multi-tone"), SignalLabWaveform::MultiTone);
    addWaveform(QStringLiteral("Linear chirp"), SignalLabWaveform::LinearChirp);
    addWaveform(QStringLiteral("Complex white noise"), SignalLabWaveform::WhiteNoise);
    addWaveform(QStringLiteral("BPSK"), SignalLabWaveform::Bpsk);
    addWaveform(QStringLiteral("QPSK"), SignalLabWaveform::Qpsk);
    addWaveform(QStringLiteral("8PSK"), SignalLabWaveform::Psk8);
    addWaveform(QStringLiteral("pi/4-DQPSK"), SignalLabWaveform::Pi4Dqpsk);
    addWaveform(QStringLiteral("16-QAM"), SignalLabWaveform::Qam16);
    addWaveform(QStringLiteral("64-QAM"), SignalLabWaveform::Qam64);
    addWaveform(QStringLiteral("256-QAM"), SignalLabWaveform::Qam256);
    addWaveform(QStringLiteral("2-FSK"), SignalLabWaveform::Fsk2);
    addWaveform(QStringLiteral("2-GFSK"), SignalLabWaveform::Gfsk2);
    addWaveform(QStringLiteral("OFDM"), SignalLabWaveform::Ofdm);
    sampleRateCombo = new QComboBox(commonBox);
    sampleRateCombo->setEditable(true);
    sampleRateCombo->setInsertPolicy(QComboBox::NoInsert);
    sampleRateCombo->setToolTip(text(QStringLiteral("Complex IQ samples per second: 48 ksps, 20 Msps, 1 Gsps. Custom input is supported; bare numbers mean sps. Maximum buffer: 20 million samples, independently of hardware capabilities."),
        QString::fromUtf8(u8"Комплексні IQ-семпли за секунду: 48 ksps, 20 Msps, 1 Gsps. Можна вводити вручну; число без одиниць означає sps. Буфер до 20 млн семплів; можливості апаратури окремі.")));
    for (int rate : {48000, 96000, 192000, 500000, 1000000, 2000000, 5000000,
                     10000000, 20000000, 50000000, 100000000, 200000000, 500000000, 1000000000}) {
        sampleRateCombo->addItem(signalunits::sampleRateText(rate), rate);
    }
    durationSpin = new QDoubleSpinBox(commonBox);
    durationSpin->setRange(0.001, 30.0);
    durationSpin->setDecimals(6);
    durationSpin->setSingleStep(0.1);
    durationSpin->setSuffix(QStringLiteral(" s"));
    amplitudeSpin = new QSpinBox(commonBox);
    amplitudeSpin->setRange(1, 100);
    amplitudeSpin->setSuffix(QStringLiteral(" %"));
    centerOffsetSpin = new QDoubleSpinBox(commonBox);
    centerOffsetSpin->setDecimals(1);
    centerOffsetSpin->setSingleStep(100.0);
    centerOffsetSpin->setSuffix(QStringLiteral(" Hz"));
    common->addRow(makeLabel(QStringLiteral("Waveform:"),
                             QString::fromUtf8(u8"Форма сигналу:"), commonBox),
                   waveformCombo);
    common->addRow(makeLabel(QStringLiteral("Generator rate:"),
                             QString::fromUtf8(u8"Частота генератора:"), commonBox),
                   sampleRateCombo);
    common->addRow(makeLabel(QStringLiteral("Duration:"),
                             QString::fromUtf8(u8"Тривалість:"), commonBox),
                   durationSpin);
    common->addRow(makeLabel(QStringLiteral("Peak amplitude:"),
                             QString::fromUtf8(u8"Пікова амплітуда:"), commonBox),
                   amplitudeSpin);
    common->addRow(makeLabel(QStringLiteral("Center offset:"),
                             QString::fromUtf8(u8"Зсув від центра:"), commonBox),
                   centerOffsetSpin);

    basicBox = new QGroupBox(parameters);
    basicBox->setObjectName(QStringLiteral("signalLabBasicBox"));
    QFormLayout *basic = new QFormLayout(basicBox);
    toneSpin = new QDoubleSpinBox(basicBox);
    toneSpin->setRange(-10000000.0, 10000000.0);
    toneSpin->setDecimals(1);
    toneSpin->setSuffix(QStringLiteral(" Hz"));
    secondToneSpin = new QDoubleSpinBox(basicBox);
    secondToneSpin->setRange(-10000000.0, 10000000.0);
    secondToneSpin->setDecimals(1);
    secondToneSpin->setSuffix(QStringLiteral(" Hz"));
    dutySpin = new QDoubleSpinBox(basicBox);
    dutySpin->setRange(0.1, 99.9);
    dutySpin->setDecimals(1);
    dutySpin->setSuffix(QStringLiteral(" %"));
    multiToneEdit = new QLineEdit(basicBox);
    basic->addRow(makeLabel(QStringLiteral("Tone / start:"),
                            QString::fromUtf8(u8"Тон / початок:"), basicBox), toneSpin);
    basic->addRow(makeLabel(QStringLiteral("Second / stop:"),
                            QString::fromUtf8(u8"Другий / кінець:"), basicBox), secondToneSpin);
    basic->addRow(makeLabel(QStringLiteral("Duty cycle:"),
                            QString::fromUtf8(u8"Коефіцієнт заповнення:"), basicBox), dutySpin);
    basic->addRow(makeLabel(QStringLiteral("Tone list (Hz):"),
                            QString::fromUtf8(u8"Список тонів (Гц):"), basicBox), multiToneEdit);

    digitalBox = new QGroupBox(parameters);
    digitalBox->setObjectName(QStringLiteral("signalLabDigitalBox"));
    QFormLayout *digital = new QFormLayout(digitalBox);
    symbolRateSpin = new QDoubleSpinBox(digitalBox);
    symbolRateSpin->setRange(1.0, 5000000.0);
    symbolRateSpin->setDecimals(1);
    symbolRateSpin->setSuffix(QStringLiteral(" sym/s"));
    rolloffSpin = new QDoubleSpinBox(digitalBox);
    rolloffSpin->setRange(0.01, 1.0);
    rolloffSpin->setDecimals(2);
    rolloffSpin->setSingleStep(0.05);
    fskDeviationSpin = new QDoubleSpinBox(digitalBox);
    fskDeviationSpin->setRange(1.0, 10000000.0);
    fskDeviationSpin->setDecimals(1);
    fskDeviationSpin->setSuffix(QStringLiteral(" Hz"));
    seedSpin = new QSpinBox(digitalBox);
    seedSpin->setRange(0, 2147483647);
    digital->addRow(makeLabel(QStringLiteral("Symbol rate:"),
                              QString::fromUtf8(u8"Символьна швидкість:"), digitalBox),
                    symbolRateSpin);
    digital->addRow(makeLabel(QStringLiteral("RRC / Gaussian factor:"),
                              QString::fromUtf8(u8"Коефіцієнт RRC / Gaussian:"), digitalBox),
                    rolloffSpin);
    digital->addRow(makeLabel(QStringLiteral("FSK deviation:"),
                              QString::fromUtf8(u8"Девіація FSK:"), digitalBox),
                    fskDeviationSpin);
    digital->addRow(makeLabel(QStringLiteral("Random seed:"),
                              QString::fromUtf8(u8"Початкове число RNG:"), digitalBox),
                    seedSpin);

    ofdmBox = new QGroupBox(parameters);
    ofdmBox->setObjectName(QStringLiteral("signalLabOfdmBox"));
    QFormLayout *ofdm = new QFormLayout(ofdmBox);
    ofdmFftCombo = new QComboBox(ofdmBox);
    for (int size : {64, 128, 256, 512, 1024, 2048, 4096}) {
        ofdmFftCombo->addItem(QString::number(size), size);
    }
    ofdmActiveSpin = new QSpinBox(ofdmBox);
    ofdmActiveSpin->setRange(2, 4094);
    ofdmCpSpin = new QSpinBox(ofdmBox);
    ofdmCpSpin->setRange(0, 2048);
    ofdmSymbolsSpin = new QSpinBox(ofdmBox);
    ofdmSymbolsSpin->setRange(1, 10000);
    ofdmPilotSpin = new QSpinBox(ofdmBox);
    ofdmPilotSpin->setRange(0, 1024);
    ofdmPilotSpin->setSpecialValueText(QStringLiteral("Off"));
    ofdmConstellationCombo = new QComboBox(ofdmBox);
    for (SignalLabConstellation constellation :
         {SignalLabConstellation::Bpsk, SignalLabConstellation::Qpsk,
          SignalLabConstellation::Psk8, SignalLabConstellation::Qam16,
          SignalLabConstellation::Qam64, SignalLabConstellation::Qam256}) {
        ofdmConstellationCombo->addItem(
            SignalLabGenerator::constellationName(constellation),
            static_cast<int>(constellation));
    }
    ofdmDcNullCheck = new QCheckBox(ofdmBox);
    ofdmWindowSpin = new QDoubleSpinBox(ofdmBox);
    ofdmWindowSpin->setRange(0.0, 20.0);
    ofdmWindowSpin->setDecimals(1);
    ofdmWindowSpin->setSuffix(QStringLiteral(" %"));
    ofdm->addRow(makeLabel(QStringLiteral("IFFT size:"),
                           QString::fromUtf8(u8"Розмір IFFT:"), ofdmBox), ofdmFftCombo);
    ofdm->addRow(makeLabel(QStringLiteral("Active subcarriers:"),
                           QString::fromUtf8(u8"Активні піднесучі:"), ofdmBox), ofdmActiveSpin);
    ofdm->addRow(makeLabel(QStringLiteral("Cyclic prefix:"),
                           QString::fromUtf8(u8"Циклічний префікс:"), ofdmBox), ofdmCpSpin);
    ofdm->addRow(makeLabel(QStringLiteral("OFDM symbols:"),
                           QString::fromUtf8(u8"Символи OFDM:"), ofdmBox), ofdmSymbolsSpin);
    ofdm->addRow(makeLabel(QStringLiteral("Constellation:"),
                           QString::fromUtf8(u8"Сузір'я:"), ofdmBox), ofdmConstellationCombo);
    ofdm->addRow(makeLabel(QStringLiteral("Pilot spacing:"),
                           QString::fromUtf8(u8"Крок пілотів:"), ofdmBox), ofdmPilotSpin);
    ofdm->addRow(makeLabel(QStringLiteral("Null DC carrier:"),
                           QString::fromUtf8(u8"Нульова DC-піднесуча:"), ofdmBox), ofdmDcNullCheck);
    ofdm->addRow(makeLabel(QStringLiteral("Symbol edge window:"),
                           QString::fromUtf8(u8"Вікно країв символу:"), ofdmBox), ofdmWindowSpin);

    left->addWidget(commonBox);
    left->addWidget(basicBox);
    left->addWidget(digitalBox);
    left->addWidget(ofdmBox);
    left->addStretch(1);
    scroll->setWidget(parameters);

    QWidget *rightPane = new QWidget(splitter);
    QVBoxLayout *right = new QVBoxLayout(rightPane);
    right->setContentsMargins(6, 0, 0, 0);
    QHBoxLayout *previewControls = new QHBoxLayout();
    previewControls->setContentsMargins(0, 0, 0, 0);
    previewModeCombo = new QComboBox(rightPane);
    previewModeCombo->addItem(QStringLiteral("I/Q + constellation"), 0);
    previewModeCombo->addItem(QStringLiteral("2D waterfall"), 1);
    previewModeCombo->addItem(QStringLiteral("3D waterfall"), 2);
    previewModeCombo->addItem(QStringLiteral("3D constellation"), 3);
    previewFftCombo = new QComboBox(rightPane);
    for (int size : {256, 512, 1024, 2048, 4096, 8192, 16384}) {
        previewFftCombo->addItem(QString::number(size), size);
    }
    previewFftCombo->setCurrentIndex(previewFftCombo->findData(2048));
    previewRowsSpin = new QSpinBox(rightPane);
    previewRowsSpin->setRange(8, 512);
    previewRowsSpin->setValue(160);
    previewControls->addWidget(makeLabel(QStringLiteral("Preview:"),
                                         QString::fromUtf8(u8"Перегляд:"), rightPane));
    previewControls->addWidget(previewModeCombo, 1);
    previewControls->addWidget(makeLabel(QStringLiteral("FFT:"),
                                         QStringLiteral("FFT:"), rightPane));
    previewControls->addWidget(previewFftCombo);
    previewControls->addWidget(makeLabel(QStringLiteral("Rows:"),
                                         QString::fromUtf8(u8"Рядки:"), rightPane));
    previewControls->addWidget(previewRowsSpin);
    constellationSurfaceCheckbox = new QCheckBox(rightPane);

    previewStack = new QStackedWidget(rightPane);
    preview = new SignalLabPreviewWidget(previewStack);
    waterfall2D = new SignalLabWaterfall2DWidget(previewStack);
    waterfall3D = new Waterfall3DView(previewStack);
    waterfall3D->setMinimumSize(430, 330);
    waterfall3D->setResolutionDivisor(1);
    waterfall3D->setSurfaceStyle(1);
    waterfall3D->setSurfaceSmoothing(1);
    waterfall3D->setSurfaceLighting(1);
    previewStack->addWidget(preview);
    previewStack->addWidget(waterfall2D);
    previewStack->addWidget(waterfall3D);
    constellation3D = new SignalLabConstellation3DWidget(previewStack);
    previewStack->addWidget(constellation3D);
    summaryLabel = new QLabel(rightPane);
    summaryLabel->setWordWrap(true);
    summaryLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    statusLabel = new QLabel(rightPane);
    statusLabel->setWordWrap(true);
    right->addLayout(previewControls);
    right->addWidget(constellationSurfaceCheckbox);
    right->addWidget(previewStack, 1);
    right->addWidget(summaryLabel);
    right->addWidget(statusLabel);
    splitter->addWidget(scroll);
    splitter->addWidget(rightPane);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({360, 600});
    root->addWidget(splitter, 1);

    QHBoxLayout *actions = new QHBoxLayout();
    generateButton = new QPushButton(this);
    transmitButton = new QPushButton(this);
    stopButton = new QPushButton(this);
    exportButton = new QPushButton(this);
    closeButton = new QPushButton(this);
    exportButton->setEnabled(false);
    actions->addWidget(generateButton);
    actions->addWidget(transmitButton);
    actions->addWidget(stopButton);
    actions->addWidget(exportButton);
    actions->addStretch(1);
    actions->addWidget(closeButton);
    root->addLayout(actions);

    connect(waveformCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this]() { updateModeUi(); });
    connect(sampleRateCombo, &QComboBox::currentTextChanged,
            this, [this]() { updateSampleRateLimits(); });
    connect(ofdmFftCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this]() {
                const int size = ofdmFftCombo->currentData().toInt();
                ofdmActiveSpin->setMaximum((std::max)(2, size - 2));
                ofdmCpSpin->setMaximum(size / 2);
                if (ofdmActiveSpin->value() > size - 2) ofdmActiveSpin->setValue(size / 2);
                if (ofdmCpSpin->value() > size / 2) ofdmCpSpin->setValue(size / 8);
            });
    connect(generateButton, &QPushButton::clicked, this,
            [this]() { generateCurrent(); });
    connect(transmitButton, &QPushButton::clicked, this, [this]() {
        if (generateCurrent() && transmitHandler) transmitHandler(lastResult);
    });
    connect(stopButton, &QPushButton::clicked, this, [this]() {
        if (stopHandler) stopHandler();
    });
    connect(exportButton, &QPushButton::clicked, this,
            [this]() { exportCurrent(); });
    connect(previewModeCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this]() { updatePreviewMode(); });
    connect(constellationSurfaceCheckbox, &QCheckBox::toggled, this,
            [this](bool enabled) { constellation3D->setSurfaceEnabled(enabled); });
    connect(previewFftCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this]() {
                const int mode = previewModeCombo->currentData().toInt();
                if (lastResult.ok() && (mode == 1 || mode == 2)) rebuildSpectralPreview();
            });
    connect(previewRowsSpin, qOverload<int>(&QSpinBox::valueChanged),
            this, [this]() {
                const int mode = previewModeCombo->currentData().toInt();
                if (lastResult.ok() && (mode == 1 || mode == 2)) rebuildSpectralPreview();
            });
    connect(closeButton, &QPushButton::clicked, this, &QDialog::hide);
}
void SignalLabDialog::setLanguage(const QString &nextLanguage) {
    language = nextLanguage;
    updateTexts();
}

void SignalLabDialog::setInitialSampleRate(int sampleRate) {
    sampleRate = (std::clamp)(sampleRate, 8000, signalunits::MaximumSampleRate);
    int index = sampleRateCombo->findData(sampleRate);
    if (index < 0) {
        sampleRateCombo->addItem(signalunits::sampleRateText(sampleRate), sampleRate);
        index = sampleRateCombo->count() - 1;
    }
    sampleRateCombo->setCurrentIndex(index);
    updateSampleRateLimits();
}

void SignalLabDialog::updateTexts() {
    setWindowTitle(text(QStringLiteral("Signal laboratory"),
                        QString::fromUtf8(u8"Лабораторія сигналів")));
    preview->setLanguage(language);
    waterfall2D->setLanguage(language);
    constellation3D->setLanguage(language);
    constellationSurfaceCheckbox->setText(text(QStringLiteral("Radial surface"), QString::fromUtf8(u8"Радіальна поверхня")));
    constellationSurfaceCheckbox->setToolTip(text(
        QStringLiteral("Fill from I=Q=0 to displayed samples. Visual interpolation only, not IQ reconstruction."),
        QString::fromUtf8(u8"Заповнення від I=Q=0 до відображених точок. Інтерполяція лише візуальна, це не відновлення IQ.")));
    previewModeCombo->setItemText(0, text(QStringLiteral("I/Q + constellation"),
                                              QString::fromUtf8(u8"I/Q + сузір'я")));
    previewModeCombo->setItemText(1, text(QStringLiteral("2D waterfall"),
                                              QString::fromUtf8(u8"2D-водоспад")));
    previewModeCombo->setItemText(2, text(QStringLiteral("3D waterfall"),
                                              QString::fromUtf8(u8"3D-водоспад")));
    previewModeCombo->setItemText(3, text(QStringLiteral("3D constellation"),
                                              QString::fromUtf8(u8"3D-сузір'я")));
    if (QGroupBox *box = findChild<QGroupBox *>(QStringLiteral("signalLabCommonBox"))) {
        box->setTitle(text(QStringLiteral("Generator"),
                           QString::fromUtf8(u8"Генератор")));
    }
    basicBox->setTitle(text(QStringLiteral("Basic and composite forms"),
                            QString::fromUtf8(u8"Прості та складені форми")));
    digitalBox->setTitle(text(QStringLiteral("Quadrature modulation"),
                              QString::fromUtf8(u8"Квадратурна модуляція")));
    ofdmBox->setTitle(QStringLiteral("OFDM"));

    for (QLabel *label : findChildren<QLabel *>()) {
        const QString english = label->property("englishText").toString();
        const QString ukrainian = label->property("ukrainianText").toString();
        if (!english.isEmpty()) label->setText(text(english, ukrainian));
    }

    const QStringList englishWaveforms = {
        QStringLiteral("Carrier"), QStringLiteral("Sine / DSB"), QStringLiteral("Square"),
        QStringLiteral("Triangle"), QStringLiteral("Sawtooth"), QStringLiteral("Pulse train"),
        QStringLiteral("Multi-tone"), QStringLiteral("Linear chirp"),
        QStringLiteral("Complex white noise"), QStringLiteral("BPSK"), QStringLiteral("QPSK"),
        QStringLiteral("8PSK"), QStringLiteral("pi/4-DQPSK"), QStringLiteral("16-QAM"),
        QStringLiteral("64-QAM"), QStringLiteral("256-QAM"), QStringLiteral("2-FSK"),
        QStringLiteral("2-GFSK"), QStringLiteral("OFDM")};
    const QStringList ukrainianWaveforms = {
        QString::fromUtf8(u8"Несуча"), QString::fromUtf8(u8"Синус / DSB"),
        QString::fromUtf8(u8"Прямокутний"), QString::fromUtf8(u8"Трикутний"),
        QString::fromUtf8(u8"Пилкоподібний"), QString::fromUtf8(u8"Послідовність імпульсів"),
        QString::fromUtf8(u8"Багатотоновий"), QString::fromUtf8(u8"Лінійний chirp"),
        QString::fromUtf8(u8"Комплексний білий шум"), QStringLiteral("BPSK"),
        QStringLiteral("QPSK"), QStringLiteral("8PSK"), QStringLiteral("pi/4-DQPSK"),
        QStringLiteral("16-QAM"), QStringLiteral("64-QAM"), QStringLiteral("256-QAM"),
        QStringLiteral("2-FSK"), QStringLiteral("2-GFSK"), QStringLiteral("OFDM")};
    for (int i = 0; i < waveformCombo->count() && i < englishWaveforms.size(); ++i) {
        waveformCombo->setItemText(i, text(englishWaveforms[i], ukrainianWaveforms[i]));
    }

    generateButton->setText(text(QStringLiteral("Generate / preview"),
                                 QString::fromUtf8(u8"Згенерувати / перегляд")));
    transmitButton->setText(text(QStringLiteral("Transmit with selected backend"),
                                 QString::fromUtf8(u8"Передати вибраним backend")));
    stopButton->setText(text(QStringLiteral("Stop TX"),
                             QString::fromUtf8(u8"Зупинити TX")));
    exportButton->setText(text(QStringLiteral("Export IQ..."),
                               QString::fromUtf8(u8"Експорт IQ...")));
    closeButton->setText(text(QStringLiteral("Close"),
                              QString::fromUtf8(u8"Закрити")));
    ofdmDcNullCheck->setText(text(QStringLiteral("Enabled"),
                                  QString::fromUtf8(u8"Увімкнено")));
    if (lastResult.iq.isEmpty()) {
        statusLabel->setText(text(
            QStringLiteral("Generate locally first. RF transmission still requires ARM in the parent TX window."),
            QString::fromUtf8(u8"Спочатку згенеруйте сигнал локально. Для RF-передачі все одно потрібне ARM у головному вікні TX.")));
    }
}

void SignalLabDialog::updateModeUi() {
    const SignalLabWaveform waveform =
        static_cast<SignalLabWaveform>(waveformCombo->currentData().toInt());
    const bool basic = static_cast<int>(waveform) <=
                       static_cast<int>(SignalLabWaveform::WhiteNoise);
    const bool ofdm = waveform == SignalLabWaveform::Ofdm;
    const bool digital = !basic;
    const bool fsk = waveform == SignalLabWaveform::Fsk2 ||
                     waveform == SignalLabWaveform::Gfsk2;
    basicBox->setVisible(basic);
    digitalBox->setVisible(digital);
    ofdmBox->setVisible(ofdm);
    durationSpin->setEnabled(!ofdm);
    symbolRateSpin->setEnabled(digital && !ofdm);
    rolloffSpin->setEnabled(digital && !ofdm);
    fskDeviationSpin->setEnabled(fsk);
    seedSpin->setEnabled(digital || waveform == SignalLabWaveform::WhiteNoise);

    toneSpin->setEnabled(waveform == SignalLabWaveform::Sine ||
                         waveform == SignalLabWaveform::Square ||
                         waveform == SignalLabWaveform::Triangle ||
                         waveform == SignalLabWaveform::Sawtooth ||
                         waveform == SignalLabWaveform::Pulse ||
                         waveform == SignalLabWaveform::LinearChirp);
    secondToneSpin->setEnabled(waveform == SignalLabWaveform::LinearChirp);
    dutySpin->setEnabled(waveform == SignalLabWaveform::Pulse);
    multiToneEdit->setEnabled(waveform == SignalLabWaveform::MultiTone);
}

void SignalLabDialog::updateSampleRateLimits() {
    const int sampleRate = signalunits::sampleRateValue(sampleRateCombo);
    if (sampleRate == 0) return;
    durationSpin->setMaximum((std::min)(30.0, 20000000.0 / sampleRate));
    const double maximum = sampleRate * 0.49;
    centerOffsetSpin->setRange(-maximum, maximum);
    toneSpin->setRange(-maximum, maximum);
    secondToneSpin->setRange(-maximum, maximum);
    fskDeviationSpin->setMaximum(maximum);
    symbolRateSpin->setMaximum(sampleRate * 0.45);
}

SignalLabConfiguration SignalLabDialog::configurationFromUi() const {
    SignalLabConfiguration configuration;
    configuration.waveform =
        static_cast<SignalLabWaveform>(waveformCombo->currentData().toInt());
    configuration.sampleRate = signalunits::sampleRateValue(sampleRateCombo);
    configuration.durationSeconds = durationSpin->value();
    configuration.amplitude = amplitudeSpin->value() / 100.0;
    configuration.centerOffsetHz = centerOffsetSpin->value();
    configuration.toneFrequencyHz = toneSpin->value();
    configuration.secondFrequencyHz = secondToneSpin->value();
    configuration.dutyCycle = dutySpin->value() / 100.0;
    configuration.multiToneFrequencies = multiToneEdit->text();
    configuration.symbolRate = symbolRateSpin->value();
    configuration.rolloff = rolloffSpin->value();
    configuration.fskDeviationHz = fskDeviationSpin->value();
    configuration.randomSeed = static_cast<quint32>(seedSpin->value());
    configuration.ofdmFftSize = ofdmFftCombo->currentData().toInt();
    configuration.ofdmActiveCarriers = ofdmActiveSpin->value();
    configuration.ofdmCyclicPrefix = ofdmCpSpin->value();
    configuration.ofdmSymbolCount = ofdmSymbolsSpin->value();
    configuration.ofdmPilotSpacing = ofdmPilotSpin->value();
    configuration.ofdmConstellation = static_cast<SignalLabConstellation>(
        ofdmConstellationCombo->currentData().toInt());
    configuration.ofdmNullDc = ofdmDcNullCheck->isChecked();
    configuration.ofdmEdgeWindowPercent = ofdmWindowSpin->value();
    return configuration;
}

bool SignalLabDialog::generateCurrent() {
    saveSettings();
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const SignalLabConfiguration configuration = configurationFromUi();
    const SignalLabGenerationResult generated =
        SignalLabGenerator::generate(configuration);
    QApplication::restoreOverrideCursor();
    if (!generated.ok()) {
        lastResult = SignalLabGenerationResult{};
        preview->clear();
        constellation3D->clear();
        waterfall2D->clear();
        waterfall3D->clearHistory();
        summaryLabel->clear();
        exportButton->setEnabled(false);
        statusLabel->setStyleSheet(QStringLiteral("color:#d9534f;"));
        statusLabel->setText(text(QStringLiteral("Generation failed: %1"),
                                  QString::fromUtf8(u8"Помилка генерації: %1"))
                                 .arg(generated.error));
        return false;
    }
    lastResult = generated;
    lastConfiguration = configuration;
    const auto iqPreview = buildSignalLabIqPreview(lastResult);
    preview->setData(iqPreview);
    constellation3D->setData(iqPreview);
    summaryLabel->setText(lastResult.summary);
    exportButton->setEnabled(true);
    statusLabel->setStyleSheet(QString());
    statusLabel->setText(text(
        QStringLiteral("Generated locally. Preview and export are RF-safe."),
        QString::fromUtf8(u8"Згенеровано локально. Перегляд та експорт не випромінюють RF.")));
    const int mode = previewModeCombo->currentData().toInt();
    if (mode == 1 || mode == 2) {
        rebuildSpectralPreview();
    }
    return true;
}

void SignalLabDialog::updatePreviewMode() {
    const int mode = previewModeCombo->currentData().toInt();
    previewStack->setCurrentIndex(mode);
    constellationSurfaceCheckbox->setVisible(mode == 3);
    const bool spectral = mode == 1 || mode == 2;
    previewFftCombo->setEnabled(spectral);
    previewRowsSpin->setEnabled(spectral);
    if (spectral && lastResult.ok()) rebuildSpectralPreview();
}

void SignalLabDialog::rebuildSpectralPreview() {
    if (!lastResult.ok()) {
        waterfall2D->clear();
        waterfall3D->clearHistory();
        return;
    }
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const SignalLabSpectralPreviewData spectral = buildSignalLabSpectralPreview(
        lastResult,
        previewFftCombo->currentData().toInt(),
        previewRowsSpin->value());
    QApplication::restoreOverrideCursor();
    if (!spectral.ok()) {
        waterfall2D->clear();
        waterfall3D->clearHistory();
        statusLabel->setStyleSheet(QStringLiteral("color:#d9534f;"));
        statusLabel->setText(text(QStringLiteral("Preview failed: %1"),
                                  QString::fromUtf8(u8"Помилка перегляду: %1"))
                                 .arg(spectral.error));
        return;
    }

    waterfall2D->setData(spectral);
    waterfall3D->clearHistory();
    waterfall3D->setHistoryCapacity((std::max)(8, spectral.rowCount));
    for (std::size_t row = 0; row < spectral.levels.size(); ++row) {
        waterfall3D->appendHistoryRow(spectral.levels[row],
                                      spectral.colors[row],
                                      spectral.minimumDb,
                                      spectral.maximumDb);
    }
}

void SignalLabDialog::exportCurrent() {
    if (!lastResult.ok() && !generateCurrent()) return;
    QSettings settings(persistentSettingsFilePath(), QSettings::IniFormat);
    const QString initial = settings.value(QStringLiteral("transmit/signalLabExportDirectory"),
                                           QDir::homePath()).toString();
    const QString suggested = QStringLiteral("signal_lab_%1.cf32")
        .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd_HHmmss")));
    const QString path = QFileDialog::getSaveFileName(
        this,
        text(QStringLiteral("Export laboratory IQ"),
             QString::fromUtf8(u8"Експорт лабораторного IQ")),
        QDir(initial).filePath(suggested),
        QStringLiteral("Complex float IQ (*.cf32)"));
    if (path.isEmpty()) return;

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        statusLabel->setStyleSheet(QStringLiteral("color:#d9534f;"));
        statusLabel->setText(file.errorString());
        return;
    }
    constexpr int chunkSamples = 8192;
    QByteArray buffer(chunkSamples * static_cast<int>(sizeof(float)) * 2, Qt::Uninitialized);
    int writtenSamples = 0;
    while (writtenSamples < lastResult.iq.size()) {
        const int count = (std::min)(chunkSamples, lastResult.iq.size() - writtenSamples);
        float *output = reinterpret_cast<float *>(buffer.data());
        for (int i = 0; i < count; ++i) {
            output[2 * i] = lastResult.iq[writtenSamples + i].real();
            output[2 * i + 1] = lastResult.iq[writtenSamples + i].imag();
        }
        const qint64 bytes = static_cast<qint64>(count) * sizeof(float) * 2;
        if (file.write(buffer.constData(), bytes) != bytes) {
            file.cancelWriting();
            statusLabel->setStyleSheet(QStringLiteral("color:#d9534f;"));
            statusLabel->setText(file.errorString());
            return;
        }
        writtenSamples += count;
    }
    if (!file.commit()) {
        statusLabel->setStyleSheet(QStringLiteral("color:#d9534f;"));
        statusLabel->setText(file.errorString());
        return;
    }

    const QFileInfo info(path);
    settings.setValue(QStringLiteral("transmit/signalLabExportDirectory"),
                      info.absolutePath());
    QJsonObject metadata;
    const SignalLabConfiguration &configuration = lastConfiguration;
    metadata.insert(QStringLiteral("format"), QStringLiteral("cf32_le"));
    metadata.insert(QStringLiteral("waveform"),
                    SignalLabGenerator::waveformName(configuration.waveform));
    metadata.insert(QStringLiteral("sample_rate"), lastResult.sampleRate);
    metadata.insert(QStringLiteral("sample_count"), lastResult.iq.size());
    metadata.insert(QStringLiteral("duration_seconds"), lastResult.durationSeconds);
    metadata.insert(QStringLiteral("occupied_bandwidth_hz"),
                    lastResult.occupiedBandwidthHz);
    metadata.insert(QStringLiteral("center_offset_hz"), configuration.centerOffsetHz);
    metadata.insert(QStringLiteral("peak"), lastResult.peak);
    metadata.insert(QStringLiteral("rms"), lastResult.rms);
    metadata.insert(QStringLiteral("papr_db"), lastResult.paprDb);
    metadata.insert(QStringLiteral("random_seed"),
                    static_cast<double>(configuration.randomSeed));
    if (lastResult.samplesPerSymbol > 0) {
        metadata.insert(QStringLiteral("samples_per_symbol"), lastResult.samplesPerSymbol);
        metadata.insert(QStringLiteral("symbol_rate_actual"),
                        double(lastResult.sampleRate) / lastResult.samplesPerSymbol);
        metadata.insert(QStringLiteral("rrc_rolloff"), lastResult.pulseRolloff);
    }
    metadata.insert(QStringLiteral("rf_output"), false);
    if (configuration.waveform == SignalLabWaveform::Ofdm) {
        metadata.insert(QStringLiteral("ofdm_fft_size"), configuration.ofdmFftSize);
        metadata.insert(QStringLiteral("ofdm_active_subcarriers"),
                        configuration.ofdmActiveCarriers);
        metadata.insert(QStringLiteral("ofdm_cyclic_prefix"),
                        configuration.ofdmCyclicPrefix);
        metadata.insert(QStringLiteral("ofdm_constellation"),
                        SignalLabGenerator::constellationName(
                            configuration.ofdmConstellation));
        metadata.insert(QStringLiteral("ofdm_pilot_spacing"),
                        configuration.ofdmPilotSpacing);
        metadata.insert(QStringLiteral("ofdm_null_dc"), configuration.ofdmNullDc);
    }
    QSaveFile sidecar(info.absolutePath() + QLatin1Char('/') +
                      info.completeBaseName() + QStringLiteral(".json"));
    if (sidecar.open(QIODevice::WriteOnly)) {
        sidecar.write(QJsonDocument(metadata).toJson(QJsonDocument::Indented));
        sidecar.commit();
    }
    statusLabel->setStyleSheet(QString());
    statusLabel->setText(text(QStringLiteral("IQ exported: %1"),
                              QString::fromUtf8(u8"IQ експортовано: %1"))
                             .arg(path));
}

void SignalLabDialog::loadSettings() {
    QSettings settings(persistentSettingsFilePath(), QSettings::IniFormat);
    const int waveform = settings.value(
        QStringLiteral("transmit/signalLab/waveform"),
        static_cast<int>(SignalLabWaveform::Carrier)).toInt();
    int index = waveformCombo->findData(waveform);
    if (index >= 0) waveformCombo->setCurrentIndex(index);
    setInitialSampleRate(settings.value(QStringLiteral("transmit/signalLab/sampleRate"),
                                        48000).toInt());
    durationSpin->setValue(settings.value(QStringLiteral("transmit/signalLab/duration"),
                                          0.5).toDouble());
    amplitudeSpin->setValue(settings.value(QStringLiteral("transmit/signalLab/amplitude"),
                                           70).toInt());
    centerOffsetSpin->setValue(settings.value(QStringLiteral("transmit/signalLab/centerOffset"),
                                              0.0).toDouble());
    toneSpin->setValue(settings.value(QStringLiteral("transmit/signalLab/tone"),
                                      1000.0).toDouble());
    secondToneSpin->setValue(settings.value(QStringLiteral("transmit/signalLab/secondTone"),
                                            3000.0).toDouble());
    dutySpin->setValue(settings.value(QStringLiteral("transmit/signalLab/duty"),
                                      50.0).toDouble());
    multiToneEdit->setText(settings.value(
        QStringLiteral("transmit/signalLab/multiTone"),
        QStringLiteral("-3000, -1000, 1000, 3000")).toString());
    symbolRateSpin->setValue(settings.value(QStringLiteral("transmit/signalLab/symbolRate"),
                                            1200.0).toDouble());
    rolloffSpin->setValue(settings.value(QStringLiteral("transmit/signalLab/rolloff"),
                                         0.35).toDouble());
    fskDeviationSpin->setValue(settings.value(
        QStringLiteral("transmit/signalLab/fskDeviation"), 1200.0).toDouble());
    seedSpin->setValue(settings.value(QStringLiteral("transmit/signalLab/seed"), 1).toInt());
    index = ofdmFftCombo->findData(settings.value(
        QStringLiteral("transmit/signalLab/ofdmFft"), 256).toInt());
    if (index >= 0) ofdmFftCombo->setCurrentIndex(index);
    ofdmActiveSpin->setValue(settings.value(
        QStringLiteral("transmit/signalLab/ofdmActive"), 128).toInt());
    ofdmCpSpin->setValue(settings.value(
        QStringLiteral("transmit/signalLab/ofdmCp"), 32).toInt());
    ofdmSymbolsSpin->setValue(settings.value(
        QStringLiteral("transmit/signalLab/ofdmSymbols"), 40).toInt());
    ofdmPilotSpin->setValue(settings.value(
        QStringLiteral("transmit/signalLab/ofdmPilots"), 16).toInt());
    index = ofdmConstellationCombo->findData(settings.value(
        QStringLiteral("transmit/signalLab/ofdmConstellation"),
        static_cast<int>(SignalLabConstellation::Qpsk)).toInt());
    if (index >= 0) ofdmConstellationCombo->setCurrentIndex(index);
    ofdmDcNullCheck->setChecked(settings.value(
        QStringLiteral("transmit/signalLab/ofdmNullDc"), true).toBool());
    ofdmWindowSpin->setValue(settings.value(
        QStringLiteral("transmit/signalLab/ofdmWindow"), 2.0).toDouble());
    index = previewModeCombo->findData(settings.value(
        QStringLiteral("transmit/signalLab/previewMode"), 0).toInt());
    if (index >= 0) previewModeCombo->setCurrentIndex(index);
    index = previewFftCombo->findData(settings.value(
        QStringLiteral("transmit/signalLab/previewFft"), 2048).toInt());
    if (index >= 0) previewFftCombo->setCurrentIndex(index);
    previewRowsSpin->setValue(settings.value(
        QStringLiteral("transmit/signalLab/previewRows"), 160).toInt());
    constellation3D->setCameraState(QJsonDocument::fromJson(settings.value(
        QStringLiteral("transmit/signalLab/constellation3DCamera")).toByteArray()).object());
    constellationSurfaceCheckbox->setChecked(settings.value(QStringLiteral("transmit/signalLab/constellationSurface"), false).toBool());
    updatePreviewMode();
    restoreGeometry(settings.value(
        QStringLiteral("transmit/signalLab/dialogGeometry")).toByteArray());
}

void SignalLabDialog::saveSettings() const {
    QSettings settings(persistentSettingsFilePath(), QSettings::IniFormat);
    const SignalLabConfiguration configuration = configurationFromUi();
    settings.setValue(QStringLiteral("transmit/signalLab/waveform"),
                      static_cast<int>(configuration.waveform));
    settings.setValue(QStringLiteral("transmit/signalLab/constellation3DCamera"),
                      QJsonDocument(constellation3D->cameraState()).toJson(QJsonDocument::Compact));
    settings.setValue(QStringLiteral("transmit/signalLab/constellationSurface"), constellationSurfaceCheckbox->isChecked());
    settings.setValue(QStringLiteral("transmit/signalLab/sampleRate"),
                      configuration.sampleRate);
    settings.setValue(QStringLiteral("transmit/signalLab/duration"),
                      configuration.durationSeconds);
    settings.setValue(QStringLiteral("transmit/signalLab/amplitude"),
                      amplitudeSpin->value());
    settings.setValue(QStringLiteral("transmit/signalLab/centerOffset"),
                      configuration.centerOffsetHz);
    settings.setValue(QStringLiteral("transmit/signalLab/tone"),
                      configuration.toneFrequencyHz);
    settings.setValue(QStringLiteral("transmit/signalLab/secondTone"),
                      configuration.secondFrequencyHz);
    settings.setValue(QStringLiteral("transmit/signalLab/duty"), dutySpin->value());
    settings.setValue(QStringLiteral("transmit/signalLab/multiTone"),
                      configuration.multiToneFrequencies);
    settings.setValue(QStringLiteral("transmit/signalLab/symbolRate"),
                      configuration.symbolRate);
    settings.setValue(QStringLiteral("transmit/signalLab/rolloff"),
                      configuration.rolloff);
    settings.setValue(QStringLiteral("transmit/signalLab/fskDeviation"),
                      configuration.fskDeviationHz);
    settings.setValue(QStringLiteral("transmit/signalLab/seed"), seedSpin->value());
    settings.setValue(QStringLiteral("transmit/signalLab/ofdmFft"),
                      configuration.ofdmFftSize);
    settings.setValue(QStringLiteral("transmit/signalLab/ofdmActive"),
                      configuration.ofdmActiveCarriers);
    settings.setValue(QStringLiteral("transmit/signalLab/ofdmCp"),
                      configuration.ofdmCyclicPrefix);
    settings.setValue(QStringLiteral("transmit/signalLab/ofdmSymbols"),
                      configuration.ofdmSymbolCount);
    settings.setValue(QStringLiteral("transmit/signalLab/ofdmPilots"),
                      configuration.ofdmPilotSpacing);
    settings.setValue(QStringLiteral("transmit/signalLab/ofdmConstellation"),
                      static_cast<int>(configuration.ofdmConstellation));
    settings.setValue(QStringLiteral("transmit/signalLab/ofdmNullDc"),
                      configuration.ofdmNullDc);
    settings.setValue(QStringLiteral("transmit/signalLab/ofdmWindow"),
                      configuration.ofdmEdgeWindowPercent);
    settings.setValue(QStringLiteral("transmit/signalLab/previewMode"),
                      previewModeCombo->currentData().toInt());
    settings.setValue(QStringLiteral("transmit/signalLab/previewFft"),
                      previewFftCombo->currentData().toInt());
    settings.setValue(QStringLiteral("transmit/signalLab/previewRows"),
                      previewRowsSpin->value());
    settings.setValue(QStringLiteral("transmit/signalLab/dialogGeometry"),
                      saveGeometry());
}

void SignalLabDialog::hideEvent(QHideEvent *event) {
    saveSettings();
    QDialog::hideEvent(event);
}

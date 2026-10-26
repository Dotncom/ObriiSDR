#include "multisdrdialog.h"

#include "analyzerspectrumwidget.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QImage>
#include <QPainter>
#include <QPaintEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <limits>
#include <cstring>
#include <thread>

class AnalyzerWaterfallPreview final : public QWidget {
public:
    explicit AnalyzerWaterfallPreview(QWidget *parent = nullptr) : QWidget(parent) {
        setMinimumHeight(150);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        history = QImage(1024, 256, QImage::Format_ARGB32);
        history.fill(QColor(3, 6, 8));
    }

    void appendFrame(const std::vector<float> &levels, double topDb, double bottomDb) {
        if (levels.empty() || !(topDb > bottomDb)) return;
        const int stride = history.bytesPerLine();
        std::memmove(history.bits() + stride, history.bits(),
                     static_cast<std::size_t>(stride) * (history.height() - 1));
        auto *row = reinterpret_cast<QRgb *>(history.scanLine(0));
        for (int x = 0; x < history.width(); ++x) {
            const std::size_t index = std::min<std::size_t>(
                levels.size() - 1,
                static_cast<std::size_t>((static_cast<quint64>(x) * levels.size()) /
                                         static_cast<quint64>(history.width())));
            const float level = levels[index];
            if (!std::isfinite(level)) {
                row[x] = qRgb(2, 4, 5);
                continue;
            }
            const double normalized = std::clamp((level - bottomDb) / (topDb - bottomDb), 0.0, 1.0);
            const QColor color = QColor::fromHsvF((1.0 - normalized) * 0.70, 0.96,
                                                  0.12 + normalized * 0.88);
            row[x] = color.rgb();
        }
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(3, 6, 8));
        painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
        painter.drawImage(rect(), history);
    }

private:
    QImage history;
};

struct MultiSdrDialog::SourceUi {
    bool active = false;
    AnalyzerReceiver *receiver = nullptr;
    QGroupBox *group = nullptr;
    QComboBox *device = nullptr;
    QComboBox *input = nullptr;
    QDoubleSpinBox *centerMhz = nullptr;
    QComboBox *sampleRate = nullptr;
    QComboBox *fft = nullptr;
    QSpinBox *lna = nullptr;
    QSpinBox *vga = nullptr;
    QDoubleSpinBox *edgeTrim = nullptr;
    QDoubleSpinBox *levelOffset = nullptr;
    QCheckBox *enabled = nullptr;
    QPushButton *startStop = nullptr;
    QPushButton *remove = nullptr;
    QLabel *status = nullptr;
    AnalyzerSpectrumFrame lastFrame;
    bool hasFrame = false;
};

MultiSdrDialog::MultiSdrDialog(bool ukrainianUi, QWidget *parent, bool primaryFromHost)
    : QDialog(parent), ukrainian(ukrainianUi), hostPrimary(primaryFromHost) {
    for (int index = 0; index < static_cast<int>(sources.size()); ++index) {
        sources[static_cast<std::size_t>(index)] = std::make_unique<SourceUi>();
        sources[static_cast<std::size_t>(index)]->receiver = new AnalyzerReceiver(this);
    }
    previewMosaicEngine.setMaximumOutputBins(65536);
    mainMosaicEngine.setMaximumOutputBins(65536);
    buildUi();
    restoreSettings();
    refreshDevices();
    if (hostPrimary) {
        SourceUi &primary = *sources[0];
        primary.group->setTitle(text("RX 1 - main Obrii receiver", "RX 1 - основний приймач Obrii"));
        primary.device->clear();
        primary.device->addItem(text("Main Obrii SDR stream", "Основний потік Obrii SDR"),
                                QStringLiteral("host-primary"));
        QWidget *primaryControls[] = {
            static_cast<QWidget *>(primary.device), static_cast<QWidget *>(primary.input),
            static_cast<QWidget *>(primary.sampleRate),
            static_cast<QWidget *>(primary.fft), static_cast<QWidget *>(primary.lna),
            static_cast<QWidget *>(primary.vga)};
        for (QWidget *control : primaryControls)
            control->setEnabled(false);
        primary.startStop->setEnabled(true);
        primary.centerMhz->setEnabled(true);
        primary.startStop->setToolTip(text(
            "Starts or stops the main Obrii receiver. Its hardware settings remain in the main Receiver and Frequencies sections.",
            "Запускає або зупиняє основний приймач Obrii. Його апаратні параметри залишаються в головних розділах «Приймач» і «Частоти»."));
        setPrimaryRunning(false);
    }
}

MultiSdrDialog::~MultiSdrDialog() {
    if (audioSourceIndex.exchange(0, std::memory_order_acq_rel) != 0)
        emit audioSourceChanged(0);
    saveSettings();
    for (int index = hostPrimary ? 1 : 0; index < static_cast<int>(sources.size()); ++index)
        stopSource(index);
}

void MultiSdrDialog::closeEvent(QCloseEvent *event) {
    saveSettings();
    QDialog::closeEvent(event);
}

QString MultiSdrDialog::text(const char *english, const char *ukrainianText) const {
    return QString::fromUtf8(ukrainian ? ukrainianText : english);
}

QString MultiSdrDialog::frequencyText(double hz) const {
    if (std::abs(hz) >= 1.0e9) return QStringLiteral("%1 GHz").arg(hz / 1.0e9, 0, 'f', 6);
    if (std::abs(hz) >= 1.0e6) return QStringLiteral("%1 MHz").arg(hz / 1.0e6, 0, 'f', 6);
    if (std::abs(hz) >= 1.0e3) return QStringLiteral("%1 kHz").arg(hz / 1.0e3, 0, 'f', 3);
    return QStringLiteral("%1 Hz").arg(hz, 0, 'f', 1);
}

void MultiSdrDialog::buildUi() {
    setWindowTitle(text("Multi-SDR spectrum mosaic", "Мозаїка спектра Multi-SDR"));
    resize(1420, 820);
    setMinimumSize(980, 620);
    setStyleSheet(QStringLiteral(
        "QDialog,QWidget{background:#0d1114;color:#cbd6d6;}"
        "QGroupBox{border:1px solid #2a4142;margin-top:10px;padding-top:8px;font-weight:600;}"
        "QGroupBox::title{subcontrol-origin:margin;left:8px;padding:0 4px;color:#8fe0c2;}"
        "QPushButton,QComboBox,QSpinBox,QDoubleSpinBox{background:#172024;border:1px solid #365153;padding:4px;}"
        "QPushButton:checked{background:#275746;border-color:#62c99f;}"));

    auto *root = new QVBoxLayout(this);
    auto *toolbar = new QHBoxLayout();
    refreshButton = new QPushButton(text("Refresh devices", "Оновити пристрої"), this);
    startAllButton = new QPushButton(text("Start both", "Запустити обидва"), this);
    stopAllButton = new QPushButton(text("Stop all", "Зупинити всі"), this);
    addReceiverButton = new QPushButton(text("Add receiver", "Додати приймач"), this);
    removeReceiverButton = new QPushButton(text("Remove last", "Видалити останній"), this);
    panTargetCombo = new QComboBox(this);
    panTargetCombo->addItem(text("Pan: synchronized group", "Рух: синхронна група"), -1);
    panTargetCombo->addItem(text("Pan: RX 1 only", "Рух: тільки RX 1"), 0);
    panTargetCombo->addItem(text("Pan: RX 2 only", "Рух: тільки RX 2"), 1);
    synchronizedPanCheckbox = new QCheckBox(
        text("Always pan RX together", "Завжди рухати RX разом"), this);
    synchronizedPanCheckbox->setChecked(true);
    synchronizedPanCheckbox->setToolTip(text(
        "Apply the same center-frequency delta to every receiver. When disabled, overview still pans the group, while focused view pans only the focused RX.",
        "Застосовувати однаковий зсув центральної частоти до всіх приймачів. Коли вимкнено, огляд усе одно рухає групу, а фокусний режим - лише вибраний RX."));
    frameAgeSpin = new QSpinBox(this);
    frameAgeSpin->setRange(20, 10000);
    frameAgeSpin->setSuffix(QStringLiteral(" ms"));
    frameAgeSpin->setValue(750);
    autoAlignCheckbox = new QCheckBox(text("Auto-align spans", "Автовирівнювання смуг"), this);
    autoAlignCheckbox->setToolTip(text(
        "Keep enabled receivers adjacent to RX 1 using their usable bandwidth after edge discard.",
        "Розташовувати активні приймачі поряд із RX 1 за корисною смугою після відкидання країв."));
    alignmentOverlapSpin = new QDoubleSpinBox(this);
    alignmentOverlapSpin->setRange(0.0, 50.0);
    alignmentOverlapSpin->setDecimals(1);
    alignmentOverlapSpin->setSingleStep(1.0);
    alignmentOverlapSpin->setSuffix(QStringLiteral(" %"));
    alignmentOverlapSpin->setValue(5.0);
    alignmentOverlapSpin->setToolTip(text(
        "Overlap between neighboring usable spans.",
        "Перекриття між сусідніми корисними смугами."));
    toolbar->addWidget(refreshButton);
    toolbar->addWidget(startAllButton);
    toolbar->addWidget(stopAllButton);
    toolbar->addWidget(addReceiverButton);
    toolbar->addWidget(removeReceiverButton);
    toolbar->addSpacing(12);
    toolbar->addWidget(panTargetCombo);
    toolbar->addWidget(synchronizedPanCheckbox);
    toolbar->addWidget(autoAlignCheckbox);
    toolbar->addWidget(alignmentOverlapSpin);
    toolbar->addWidget(new QLabel(text("Max frame skew:", "Макс. розсинхрон кадрів:"), this));
    toolbar->addWidget(frameAgeSpin);
    toolbar->addStretch(1);
    root->addLayout(toolbar);

    auto *splitter = new QSplitter(Qt::Horizontal, this);
    auto *controlsScroll = new QScrollArea(splitter);
    controlsScroll->setWidgetResizable(true);
    controlsScroll->setFrameShape(QFrame::NoFrame);
    auto *controls = new QWidget(controlsScroll);
    auto *controlsLayout = new QVBoxLayout(controls);

    for (int sourceIndex = 0; sourceIndex < static_cast<int>(sources.size()); ++sourceIndex) {
        SourceUi &source = *sources[static_cast<std::size_t>(sourceIndex)];
        source.active = sourceIndex < activeSourceCount;
        source.group = new QGroupBox(QStringLiteral("RX %1").arg(sourceIndex + 1), controls);
        auto *form = new QFormLayout(source.group);
        source.device = new QComboBox(source.group);
        source.input = new QComboBox(source.group);
        source.centerMhz = new QDoubleSpinBox(source.group);
        source.centerMhz->setDecimals(6);
        source.centerMhz->setRange(0.001, 8000.0);
        source.centerMhz->setSuffix(QStringLiteral(" MHz"));
        source.centerMhz->setSingleStep(0.1);
        source.sampleRate = new QComboBox(source.group);
        source.fft = new QComboBox(source.group);
        for (int length : {2048, 4096, 8192, 16384, 32768, 65536,
                           131072, 262144, 524288, 1048576})
            source.fft->addItem(QString::number(length), length);
        source.lna = new QSpinBox(source.group);
        source.lna->setRange(0, 40);
        source.lna->setSuffix(QStringLiteral(" dB"));
        source.vga = new QSpinBox(source.group);
        source.vga->setRange(0, 62);
        source.vga->setSuffix(QStringLiteral(" dB"));
        auto *gainRow = new QWidget(source.group);
        auto *gainLayout = new QHBoxLayout(gainRow);
        gainLayout->setContentsMargins(0, 0, 0, 0);
        gainLayout->addWidget(source.lna);
        gainLayout->addWidget(source.vga);
        source.edgeTrim = new QDoubleSpinBox(source.group);
        source.edgeTrim->setRange(0.0, 45.0);
        source.edgeTrim->setDecimals(1);
        source.edgeTrim->setSuffix(QStringLiteral(" %"));
        source.edgeTrim->setValue(5.0);
        source.levelOffset = new QDoubleSpinBox(source.group);
        source.levelOffset->setRange(-120.0, 120.0);
        source.levelOffset->setDecimals(2);
        source.levelOffset->setSuffix(QStringLiteral(" dB"));
        source.enabled = new QCheckBox(text("Include in main window", "Додати до основного вікна"), source.group);
        source.enabled->setChecked(true);
        source.startStop = new QPushButton(text("Start RX", "Запустити RX"), source.group);
        source.remove = new QPushButton(text("Remove this RX", "Прибрати цей RX"), source.group);
        source.remove->setVisible(sourceIndex > 0);
        source.status = new QLabel(text("Stopped", "Зупинено"), source.group);
        source.status->setWordWrap(true);
        source.status->setStyleSheet(QStringLiteral("color:#91a3a5;"));

        form->addRow(text("Device:", "Пристрій:"), source.device);
        form->addRow(text("Input / channel:", "Вхід / канал:"), source.input);
        form->addRow(text("Center:", "Центр:"), source.centerMhz);
        form->addRow(text("Sample rate:", "Частота семплів:"), source.sampleRate);
        form->addRow(QStringLiteral("FFT:"), source.fft);
        form->addRow(QStringLiteral("LNA / VGA:"), gainRow);
        form->addRow(text("Discard each edge:", "Відкинути кожен край:"), source.edgeTrim);
        form->addRow(text("Level correction:", "Поправка рівня:"), source.levelOffset);
        form->addRow(source.enabled);
        form->addRow(source.startStop);
        if (sourceIndex > 0) form->addRow(source.remove);
        form->addRow(source.status);
        controlsLayout->addWidget(source.group);
        source.group->setVisible(source.active);

        connect(source.device, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this, sourceIndex](int) {
            populateInputs(sourceIndex);
            populateSampleRates(sourceIndex);
        });
        connect(source.sampleRate, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this](int) {
            if (autoAlignCheckbox && autoAlignCheckbox->isChecked()) alignEnabledSources();
        });
        connect(source.startStop, &QPushButton::clicked,
                this, [this, sourceIndex]() {
            if (hostPrimary && sourceIndex == 0) {
                if (primaryRunning) emit primaryStopRequested();
                else emit primaryStartRequested();
                return;
            }
            SourceUi &item = *sources[static_cast<std::size_t>(sourceIndex)];
            if (item.receiver->isRunning()) stopSource(sourceIndex);
            else startSource(sourceIndex);
        });
        if (sourceIndex > 0) {
            connect(source.remove, &QPushButton::clicked,
                    this, [this, sourceIndex]() { removeReceiverSlot(sourceIndex); });
        }
        connect(source.centerMhz, &QDoubleSpinBox::editingFinished,
                this, [this, sourceIndex]() {
            SourceUi &item = *sources[static_cast<std::size_t>(sourceIndex)];
            if (hostPrimary && sourceIndex == 0) {
                const double previousHz = item.hasFrame
                    ? item.lastFrame.centerHz
                    : item.centerMhz->property("lastCenterHz").toDouble();
                const double requestedHz = item.centerMhz->value() * 1.0e6;
                if (std::isfinite(previousHz) && previousHz > 0.0)
                    emit primaryRetuneRequested(requestedHz - previousHz);
                item.centerMhz->setProperty("lastCenterHz", requestedHz);
                if (autoAlignCheckbox->isChecked()) alignEnabledSources(sourceIndex);
            } else if (item.receiver->isRunning()) {
                retuneSource(sourceIndex, item.centerMhz->value() * 1.0e6);
                if (autoAlignCheckbox->isChecked()) alignEnabledSources(sourceIndex);
            } else if (autoAlignCheckbox->isChecked()) {
                alignEnabledSources(sourceIndex);
            }
        });
        connect(source.lna, QOverload<int>::of(&QSpinBox::valueChanged),
                this, [this, sourceIndex](int) {
            SourceUi &item = *sources[static_cast<std::size_t>(sourceIndex)];
            if (item.receiver->isRunning())
                item.receiver->setGains(item.lna->value(), item.vga->value());
        });
        connect(source.vga, QOverload<int>::of(&QSpinBox::valueChanged),
                this, [this, sourceIndex](int) {
            SourceUi &item = *sources[static_cast<std::size_t>(sourceIndex)];
            if (item.receiver->isRunning())
                item.receiver->setGains(item.lna->value(), item.vga->value());
        });
        const auto refreshSettings = [this, sourceIndex]() {
            previewMosaicEngine.setSourceSettings(sourceIndex, previewMosaicSettings(sourceIndex));
            mainMosaicEngine.setSourceSettings(sourceIndex, mainMosaicSettings(sourceIndex));
            refreshMosaic(true);
        };
        connect(source.edgeTrim, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [this, refreshSettings](double) {
            refreshSettings();
            if (autoAlignCheckbox->isChecked()) alignEnabledSources();
        });
        connect(source.levelOffset, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [refreshSettings](double) { refreshSettings(); });
        connect(source.enabled, &QCheckBox::toggled,
                this, [this, refreshSettings](bool) {
            refreshSettings();
            if (autoAlignCheckbox->isChecked()) alignEnabledSources();
        });
        connect(source.receiver, &AnalyzerReceiver::frameReady,
                this, [this, sourceIndex](const AnalyzerSpectrumFrame &frame) {
            consumeFrame(sourceIndex, frame);
        });
        connect(source.receiver, &AnalyzerReceiver::iqBlockReady,
                this,
                [this, sourceIndex](const float *samples, quint64 floatCount,
                                    double sampleRateHz, double centerHz) {
            if (sourceIndex > 0 &&
                audioSourceIndex.load(std::memory_order_acquire) == sourceIndex) {
                emit auxiliaryIqReady(samples, floatCount, sampleRateHz, centerHz);
            }
        }, Qt::DirectConnection);
        connect(source.receiver, &AnalyzerReceiver::runningChanged,
                this, [this, sourceIndex](bool running) {
            SourceUi &item = *sources[static_cast<std::size_t>(sourceIndex)];
            item.startStop->setText(running ? text("Stop RX", "Зупинити RX")
                                            : text("Start RX", "Запустити RX"));
            item.device->setEnabled(!running);
            item.input->setEnabled(!running && item.input->count() > 1);
            item.sampleRate->setEnabled(!running);
            item.fft->setEnabled(!running);
            int expectedAudioSource = sourceIndex;
            if (!running && sourceIndex > 0 &&
                audioSourceIndex.compare_exchange_strong(
                    expectedAudioSource, 0, std::memory_order_acq_rel))
                emit audioSourceChanged(0);
        });
        connect(source.receiver, &AnalyzerReceiver::receiverError,
                this, [this, sourceIndex](const QString &message) {
            sources[static_cast<std::size_t>(sourceIndex)]->status->setText(message);
        });

        const auto persistSourceSettings = [this]() { saveSettings(); };
        connect(source.device, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, persistSourceSettings);
        connect(source.input, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, persistSourceSettings);
        connect(source.sampleRate, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, persistSourceSettings);
        connect(source.fft, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, persistSourceSettings);
        connect(source.centerMhz, &QDoubleSpinBox::editingFinished,
                this, persistSourceSettings);
        connect(source.lna, QOverload<int>::of(&QSpinBox::valueChanged),
                this, persistSourceSettings);
        connect(source.vga, QOverload<int>::of(&QSpinBox::valueChanged),
                this, persistSourceSettings);
        connect(source.edgeTrim, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, persistSourceSettings);
        connect(source.levelOffset, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, persistSourceSettings);
        connect(source.enabled, &QCheckBox::toggled, this, persistSourceSettings);
    }
    controlsLayout->addStretch(1);

    auto *displayGroup = new QGroupBox(text("Mosaic display", "Відображення мозаїки"), controls);
    auto *displayForm = new QFormLayout(displayGroup);
    topDbSpin = new QDoubleSpinBox(displayGroup);
    topDbSpin->setRange(-199.0, 100.0);
    topDbSpin->setValue(0.0);
    topDbSpin->setSuffix(QStringLiteral(" dBFS"));
    bottomDbSpin = new QDoubleSpinBox(displayGroup);
    bottomDbSpin->setRange(-200.0, 99.0);
    bottomDbSpin->setValue(-140.0);
    bottomDbSpin->setSuffix(QStringLiteral(" dBFS"));
    displayForm->addRow(text("Top:", "Верх:"), topDbSpin);
    displayForm->addRow(text("Bottom:", "Низ:"), bottomDbSpin);
    controlsLayout->insertWidget(2, displayGroup);
    controlsScroll->setWidget(controls);

    auto *display = new QWidget(splitter);
    auto *displayLayout = new QVBoxLayout(display);
    spectrum = new AnalyzerSpectrumWidget(display);
    spectrum->setAmplitudeBounds(topDbSpin->value(), bottomDbSpin->value());
    previewWaterfall = new AnalyzerWaterfallPreview(display);
    statusLabel = new QLabel(text("Select two receivers and start the mosaic.",
                                  "Оберіть два приймачі та запустіть мозаїку."), display);
    statusLabel->setWordWrap(true);
    displayLayout->addWidget(spectrum, 3);
    displayLayout->addWidget(previewWaterfall, 2);
    displayLayout->addWidget(statusLabel);
    splitter->addWidget(controlsScroll);
    splitter->addWidget(display);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({360, 1060});
    root->addWidget(splitter, 1);

    connect(refreshButton, &QPushButton::clicked, this, &MultiSdrDialog::refreshDevices);
    connect(startAllButton, &QPushButton::clicked, this, &MultiSdrDialog::startAll);
    connect(stopAllButton, &QPushButton::clicked, this, &MultiSdrDialog::stopAll);
    connect(addReceiverButton, &QPushButton::clicked, this, &MultiSdrDialog::addReceiverSlot);
    connect(removeReceiverButton, &QPushButton::clicked, this,
            [this]() { removeReceiverSlot(); });
    connect(autoAlignCheckbox, &QCheckBox::toggled, this, [this](bool enabled) {
        alignmentOverlapSpin->setEnabled(enabled);
        if (enabled) alignEnabledSources();
    });
    connect(alignmentOverlapSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double) {
        if (autoAlignCheckbox->isChecked()) alignEnabledSources();
    });
    connect(frameAgeSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int value) {
        previewMosaicEngine.setMaximumFrameAgeMs(value);
        mainMosaicEngine.setMaximumFrameAgeMs(value);
        refreshMosaic(true);
    });
    const auto updateAmplitude = [this](double) {
        spectrum->setAmplitudeBounds(topDbSpin->value(), bottomDbSpin->value());
    };
    connect(topDbSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, updateAmplitude);
    connect(bottomDbSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, updateAmplitude);
    const auto persistDialogSettings = [this]() { saveSettings(); };
    connect(panTargetCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, persistDialogSettings);
    connect(synchronizedPanCheckbox, &QCheckBox::toggled, this, persistDialogSettings);
    connect(autoAlignCheckbox, &QCheckBox::toggled, this, persistDialogSettings);
    connect(alignmentOverlapSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, persistDialogSettings);
    connect(frameAgeSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, persistDialogSettings);
    connect(topDbSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, persistDialogSettings);
    connect(bottomDbSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, persistDialogSettings);
    connect(spectrum, &AnalyzerSpectrumWidget::frequencyPanRequested,
            this, &MultiSdrDialog::panMosaic);
    connect(spectrum, &AnalyzerSpectrumWidget::frequencyRangeSelected,
            this, [this](double first, double second) {
        if (first > second) std::swap(first, second);
        spectrum->setMeasurementDetails(
            text("Selected range", "Виділений діапазон") + QStringLiteral(":\n") +
            frequencyText(first) + QStringLiteral(" .. ") + frequencyText(second) +
            QStringLiteral("\n") + text("Span", "Ширина") + QStringLiteral(": ") +
            frequencyText(second - first));
    });
}

void MultiSdrDialog::refreshDevices() {
    for (int index = hostPrimary ? 1 : 0; index < static_cast<int>(sources.size()); ++index) {
        const auto &source = sources[static_cast<std::size_t>(index)];
        if (source->active && source->receiver->isRunning()) {
            QMessageBox::information(this, windowTitle(),
                                     text("Stop all receivers before refreshing USB devices.",
                                          "Зупиніть усі приймачі перед оновленням USB-пристроїв."));
            return;
        }
    }
    setCursor(Qt::WaitCursor);
    devices = AnalyzerReceiver::enumerateDevices();
    unsetCursor();
    for (int index = hostPrimary ? 1 : 0; index < static_cast<int>(sources.size()); ++index) {
        if (sources[static_cast<std::size_t>(index)]->active) populateDeviceCombo(index);
    }
    statusLabel->setText(text("Detected devices: ", "Знайдено пристроїв: ") +
                         QString::number(devices.size()));
}

void MultiSdrDialog::populateDeviceCombo(int sourceIndex) {
    SourceUi &source = *sources[static_cast<std::size_t>(sourceIndex)];
    QString previousBackend = source.device->currentData(Qt::UserRole).toString();
    int previousIndex = source.device->currentData(Qt::UserRole + 1).toInt();
    if (previousBackend.isEmpty()) {
        previousBackend = source.device->property("savedBackend").toString();
        previousIndex = source.device->property("savedNativeIndex").toInt();
    }
    const QSignalBlocker blocker(source.device);
    source.device->clear();
    for (const AnalyzerReceiverDevice &device : devices) {
        source.device->addItem(device.label, device.backendId);
        source.device->setItemData(source.device->count() - 1, device.nativeIndex, Qt::UserRole + 1);
    }
    int wanted = -1;
    for (int index = 0; index < source.device->count(); ++index) {
        if (source.device->itemData(index, Qt::UserRole).toString() == previousBackend &&
            source.device->itemData(index, Qt::UserRole + 1).toInt() == previousIndex) {
            wanted = index;
            break;
        }
    }
    if (wanted < 0 && sourceIndex < source.device->count()) wanted = sourceIndex;
    source.device->setCurrentIndex(wanted >= 0 ? wanted : 0);
    populateInputs(sourceIndex);
    populateSampleRates(sourceIndex);
}

void MultiSdrDialog::populateInputs(int sourceIndex) {
    SourceUi &source = *sources[static_cast<std::size_t>(sourceIndex)];
    int previous = source.input->currentData().toInt();
    if (source.input->count() == 0)
        previous = source.input->property("savedInput").toInt();
    const QString backend = source.device->currentData(Qt::UserRole).toString();
    const QSignalBlocker blocker(source.input);
    source.input->clear();
    if (backend.startsWith(QStringLiteral("fobos-"))) {
        source.input->addItem(text("RF / tuner", "RF / тюнер"), 0);
        source.input->addItem(QStringLiteral("HF1 direct"), 1);
        source.input->addItem(QStringLiteral("HF2 direct"), 2);
        source.input->setCurrentIndex(std::max(0, source.input->findData(previous)));
    } else if (backend == QStringLiteral("soapy-sdr")) {
        int channels = 1;
        const int nativeIndex = source.device->currentData(Qt::UserRole + 1).toInt();
        for (const AnalyzerReceiverDevice &device : devices) {
            if (device.backendId == backend && device.nativeIndex == nativeIndex) {
                channels = std::max(1, device.rxChannels);
                break;
            }
        }
        for (int channel = 0; channel < channels; ++channel)
            source.input->addItem(QStringLiteral("RX %1").arg(channel), channel);
        source.input->setCurrentIndex(std::max(0, source.input->findData(previous)));
    } else {
        source.input->addItem(text("RF / tuner", "RF / тюнер"), 0);
    }
    source.input->setEnabled(!source.receiver->isRunning() && source.input->count() > 1);
}

void MultiSdrDialog::populateSampleRates(int sourceIndex) {
    SourceUi &source = *sources[static_cast<std::size_t>(sourceIndex)];
    double previous = source.sampleRate->currentData().toDouble();
    if (!(previous > 0.0)) previous = source.sampleRate->property("savedRate").toDouble();
    const QString backend = source.device->currentData(Qt::UserRole).toString();
    QVector<double> rates;
    if (backend == QStringLiteral("hackrf"))
        rates = {2.0e6, 4.0e6, 8.0e6, 10.0e6, 12.5e6, 16.0e6, 20.0e6};
    else if (backend == QStringLiteral("rtl-sdr"))
        rates = {1.024e6, 1.536e6, 2.048e6, 2.4e6, 2.56e6, 3.2e6};
    else if (backend == QStringLiteral("soapy-sdr")) {
        const int nativeIndex = source.device->currentData(Qt::UserRole + 1).toInt();
        for (const AnalyzerReceiverDevice &device : devices) {
            if (device.backendId == backend && device.nativeIndex == nativeIndex) {
                rates = device.sampleRatesHz;
                break;
            }
        }
        if (rates.isEmpty()) rates = {1.0e6, 2.0e6, 5.0e6, 10.0e6, 20.0e6, 30.72e6};
    } else {
        rates = {8.0e6, 10.0e6, 12.5e6, 16.0e6, 20.0e6,
                 25.0e6, 32.0e6, 40.0e6, 50.0e6};
    }
    const QSignalBlocker blocker(source.sampleRate);
    source.sampleRate->clear();
    for (double rate : rates)
        source.sampleRate->addItem(QStringLiteral("%1 MS/s").arg(rate / 1.0e6, 0, 'f', 3), rate);
    int selected = -1;
    double nearest = std::numeric_limits<double>::max();
    const double wanted = previous > 0.0 ? previous : (backend == QStringLiteral("hackrf") ? 20.0e6 : 50.0e6);
    for (int index = 0; index < source.sampleRate->count(); ++index) {
        const double distance = std::abs(source.sampleRate->itemData(index).toDouble() - wanted);
        if (distance < nearest) { nearest = distance; selected = index; }
    }
    source.sampleRate->setCurrentIndex(std::max(0, selected));
}

AnalyzerReceiverSettings MultiSdrDialog::receiverSettings(int sourceIndex) const {
    const SourceUi &source = *sources[static_cast<std::size_t>(sourceIndex)];
    AnalyzerReceiverSettings settings;
    settings.backendId = source.device->currentData(Qt::UserRole).toString();
    settings.nativeIndex = source.device->currentData(Qt::UserRole + 1).toInt();
    settings.centerHz = source.centerMhz->value() * 1.0e6;
    settings.sampleRateHz = source.sampleRate->currentData().toDouble();
    settings.fftLength = source.fft->currentData().toInt();
    settings.displayReductionRatio = 0;
    settings.displayReductionMode = 0;
    settings.lnaGainDb = source.lna->value();
    settings.vgaGainDb = source.vga->value();
    int enabledReceivers = hostPrimary ? 1 : 0;
    for (int index = hostPrimary ? 1 : 0; index < static_cast<int>(sources.size()); ++index) {
        if (sources[static_cast<std::size_t>(index)]->active) ++enabledReceivers;
    }
    const unsigned int hardwareThreads = std::max(1U, std::thread::hardware_concurrency());
    settings.fftWorkerThreads = std::clamp(
        static_cast<int>(hardwareThreads /
                         static_cast<unsigned int>(std::max(1, enabledReceivers))),
        1, 4);
    settings.directChannel = source.input->currentData().toInt();
    settings.directSampling = settings.backendId.startsWith(QStringLiteral("fobos-")) &&
                              settings.directChannel != 0;
    if (settings.backendId == QStringLiteral("soapy-sdr")) {
        settings.rxChannel = std::max(0, source.input->currentData().toInt());
        settings.directChannel = 0;
        settings.directSampling = false;
    }
    return settings;
}

SpectrumMosaicSourceSettings MultiSdrDialog::previewMosaicSettings(int sourceIndex) const {
    const SourceUi &source = *sources[static_cast<std::size_t>(sourceIndex)];
    SpectrumMosaicSourceSettings settings;
    settings.enabled = source.active;
    settings.edgeDiscardFraction = source.edgeTrim->value() / 100.0;
    settings.levelOffsetDb = source.levelOffset->value();
    return settings;
}

SpectrumMosaicSourceSettings MultiSdrDialog::mainMosaicSettings(int sourceIndex) const {
    const SourceUi &source = *sources[static_cast<std::size_t>(sourceIndex)];
    SpectrumMosaicSourceSettings settings;
    settings.enabled = source.active && source.enabled->isChecked();
    settings.edgeDiscardFraction = source.edgeTrim->value() / 100.0;
    settings.levelOffsetDb = source.levelOffset->value();
    return settings;
}

bool MultiSdrDialog::startSource(int sourceIndex) {
    SourceUi &source = *sources[static_cast<std::size_t>(sourceIndex)];
    if (!source.active) return false;
    if (source.receiver->isRunning()) return true;
    if (source.device->currentIndex() < 0 ||
        source.device->currentData(Qt::UserRole).toString().isEmpty()) {
        source.status->setText(text("No receiver selected", "Приймач не вибрано"));
        return false;
    }
    for (int otherIndex = 0; otherIndex < static_cast<int>(sources.size()); ++otherIndex) {
        if (otherIndex == sourceIndex) continue;
        const SourceUi &other = *sources[static_cast<std::size_t>(otherIndex)];
        if (!other.active) continue;
        const QString otherBackend = other.device->currentData(Qt::UserRole).toString();
        const QString sourceBackend = source.device->currentData(Qt::UserRole).toString();
        const bool sameFobosDevice = otherBackend.startsWith(QStringLiteral("fobos-")) &&
                                     sourceBackend.startsWith(QStringLiteral("fobos-")) &&
                                     other.device->currentData(Qt::UserRole + 1).toInt() ==
                                         source.device->currentData(Qt::UserRole + 1).toInt();
        if (other.receiver->isRunning() && (sameFobosDevice ||
            other.device->currentData(Qt::UserRole).toString() ==
                source.device->currentData(Qt::UserRole).toString() &&
            other.device->currentData(Qt::UserRole + 1).toInt() ==
                source.device->currentData(Qt::UserRole + 1).toInt())) {
            source.status->setText(text("This physical device is already used by the other RX.",
                                        "Цей фізичний пристрій уже використовує інший RX."));
            return false;
        }
    }
    previewMosaicEngine.setSourceSettings(sourceIndex, previewMosaicSettings(sourceIndex));
    mainMosaicEngine.setSourceSettings(sourceIndex, mainMosaicSettings(sourceIndex));
    source.hasFrame = false;
    if (!source.receiver->start(receiverSettings(sourceIndex))) {
        source.status->setText(source.receiver->errorString());
        return false;
    }
    source.status->setText(text("Starting...", "Запуск..."));
    return true;
}

void MultiSdrDialog::stopSource(int sourceIndex) {
    SourceUi &source = *sources[static_cast<std::size_t>(sourceIndex)];
    source.receiver->stop();
    source.hasFrame = false;
    SpectrumMosaicSourceSettings disabledPreview = previewMosaicSettings(sourceIndex);
    disabledPreview.enabled = false;
    SpectrumMosaicSourceSettings disabledMain = mainMosaicSettings(sourceIndex);
    disabledMain.enabled = false;
    previewMosaicEngine.setSourceSettings(sourceIndex, disabledPreview);
    mainMosaicEngine.setSourceSettings(sourceIndex, disabledMain);
    refreshMosaic(true);
    source.status->setText(text("Stopped", "Зупинено"));
    if (audioSourceIndex.load(std::memory_order_acquire) == sourceIndex) {
        audioSourceIndex.store(0, std::memory_order_release);
        emit audioSourceChanged(0);
    }
}

void MultiSdrDialog::startAll() {
    previewMosaicEngine.clear();
    mainMosaicEngine.clear();
    latestMainMosaicValid = false;
    if (autoAlignCheckbox->isChecked()) alignEnabledSources();
    if (hostPrimary && sources[0]->active && !primaryRunning)
        emit primaryStartRequested();
    for (int index = hostPrimary ? 1 : 0; index < static_cast<int>(sources.size()); ++index) {
        if (sources[static_cast<std::size_t>(index)]->active) startSource(index);
    }
}

void MultiSdrDialog::stopAll() {
    if (hostPrimary && primaryRunning)
        emit primaryStopRequested();
    for (int index = hostPrimary ? 1 : 0; index < static_cast<int>(sources.size()); ++index)
        stopSource(index);
}

void MultiSdrDialog::addReceiverSlot() {
    if (activeSourceCount >= static_cast<int>(sources.size())) return;
    int index = -1;
    for (int candidate = 1; candidate < static_cast<int>(sources.size()); ++candidate) {
        if (!sources[static_cast<std::size_t>(candidate)]->active) {
            index = candidate;
            break;
        }
    }
    if (index < 0) return;
    ++activeSourceCount;
    SourceUi &source = *sources[static_cast<std::size_t>(index)];
    source.active = true;
    source.group->setVisible(true);
    source.enabled->setChecked(true);
    populateDeviceCombo(index);
    if (panTargetCombo->findData(index) < 0)
        panTargetCombo->addItem(text("Pan: selected RX", "Рух: вибраний RX") +
                                    QStringLiteral(" %1").arg(index + 1), index);
    addReceiverButton->setEnabled(activeSourceCount < static_cast<int>(sources.size()));
    removeReceiverButton->setEnabled(activeSourceCount > 1);
    startAllButton->setText(text("Start all", "Запустити всі"));
    if (autoAlignCheckbox->isChecked()) alignEnabledSources();
    saveSettings();
}

void MultiSdrDialog::removeReceiverSlot() {
    if (activeSourceCount <= 1) return;
    for (int index = static_cast<int>(sources.size()) - 1; index >= 1; --index) {
        if (sources[static_cast<std::size_t>(index)]->active) {
            removeReceiverSlot(index);
            return;
        }
    }
}

void MultiSdrDialog::removeReceiverSlot(int index) {
    if (index <= 0 || index >= static_cast<int>(sources.size()) || activeSourceCount <= 1) return;
    SourceUi &source = *sources[static_cast<std::size_t>(index)];
    if (!source.active) return;
    stopSource(index);
    source.active = false;
    --activeSourceCount;
    source.enabled->setChecked(false);
    source.group->setVisible(false);
    const int panIndex = panTargetCombo->findData(index);
    if (panIndex >= 0) panTargetCombo->removeItem(panIndex);
    previewMosaicEngine.setSourceSettings(index, previewMosaicSettings(index));
    mainMosaicEngine.setSourceSettings(index, mainMosaicSettings(index));
    refreshMosaic(true);
    addReceiverButton->setEnabled(true);
    removeReceiverButton->setEnabled(activeSourceCount > 1);
    startAllButton->setText(activeSourceCount == 2
                                ? text("Start both", "Запустити обидва")
                                : text("Start all", "Запустити всі"));
    saveSettings();
}

void MultiSdrDialog::consumePrimaryFrame(const AnalyzerSpectrumFrame &frame) {
    if (!hostPrimary) return;
    if (!primaryRunning) setPrimaryRunning(true);
    SourceUi &source = *sources[0];
    const bool firstFrame = !source.hasFrame;
    const double previousCenterHz = source.hasFrame ? source.lastFrame.centerHz : frame.centerHz;
    {
        const QSignalBlocker blocker(source.centerMhz);
        source.centerMhz->setValue(frame.centerHz / 1.0e6);
    }
    source.centerMhz->setProperty("lastCenterHz", frame.centerHz);
    consumeFrame(0, frame);
    const bool centerChanged = std::abs(frame.centerHz - previousCenterHz) > 0.5;
    bool synchronizedPanFrame = false;
    if (centerChanged && std::isfinite(pendingSynchronizedPrimaryCenterHz)) {
        synchronizedPanFrame = std::abs(frame.centerHz - pendingSynchronizedPrimaryCenterHz) <= 1.0;
        pendingSynchronizedPrimaryCenterHz = std::numeric_limits<double>::quiet_NaN();
    }
    if ((firstFrame || centerChanged) && !synchronizedPanFrame &&
        autoAlignCheckbox->isChecked()) {
        alignEnabledSources();
    }
}

void MultiSdrDialog::setPrimaryRunning(bool running) {
    if (!hostPrimary) return;
    const bool changed = primaryRunning != running;
    primaryRunning = running;
    SourceUi &primary = *sources[0];
    primary.startStop->setText(running ? text("Stop RX", "Зупинити RX")
                                       : text("Start RX", "Запустити RX"));
    if (!running) {
        primary.hasFrame = false;
        previewMosaicEngine.clear();
        mainMosaicEngine.clear();
        latestMainMosaicValid = false;
        primary.status->setText(text(
            "Main receiver is stopped. Start it here or in the Receiver section.",
            "Основний приймач зупинено. Запустіть його тут або в розділі «Приймач»."));
    } else if (!primary.hasFrame) {
        primary.status->setText(text("Waiting for the main spectrum",
                                     "Очікування основного спектра"));
    }
    if (!changed) return;
    for (int index = 1; index < static_cast<int>(sources.size()); ++index) {
        SourceUi &source = *sources[static_cast<std::size_t>(index)];
        if (!source.active) continue;
        if (running) startSource(index);
        else stopSource(index);
    }
}

void MultiSdrDialog::setPrimaryConfiguration(double centerHz, double sampleRateHz, int fftLength) {
    if (!hostPrimary) return;
    SourceUi &primary = *sources[0];
    {
        const QSignalBlocker blocker(primary.centerMhz);
        primary.centerMhz->setValue(centerHz / 1.0e6);
        primary.centerMhz->setProperty("lastCenterHz", centerHz);
    }
    {
        const QSignalBlocker blocker(primary.sampleRate);
        primary.sampleRate->clear();
        primary.sampleRate->addItem(
            QStringLiteral("%1 MS/s").arg(sampleRateHz / 1.0e6, 0, 'f', 3), sampleRateHz);
    }
    {
        const QSignalBlocker blocker(primary.fft);
        int index = primary.fft->findData(fftLength);
        if (index < 0) {
            primary.fft->addItem(QString::number(fftLength), fftLength);
            index = primary.fft->count() - 1;
        }
        primary.fft->setCurrentIndex(index);
    }
}

double MultiSdrDialog::sourceUsefulBandwidthHz(int sourceIndex) const {
    const SourceUi &source = *sources[static_cast<std::size_t>(sourceIndex)];
    double sampleRateHz = source.sampleRate->currentData().toDouble();
    if (hostPrimary && sourceIndex == 0 && source.hasFrame) {
        const double frameSpan = source.lastFrame.maximumHz - source.lastFrame.minimumHz;
        if (std::isfinite(frameSpan) && frameSpan > 0.0) sampleRateHz = frameSpan;
    }
    const double retainedFraction = std::clamp(1.0 - 2.0 * source.edgeTrim->value() / 100.0,
                                               0.02, 1.0);
    return std::max(1.0, sampleRateHz * retainedFraction);
}

void MultiSdrDialog::alignEnabledSources(int anchorSourceIndex) {
    if (!autoAlignCheckbox || !autoAlignCheckbox->isChecked() || activeSourceCount < 2) return;
    std::vector<int> enabledIndices;
    for (int index = 0; index < static_cast<int>(sources.size()); ++index) {
        if (sources[static_cast<std::size_t>(index)]->active) enabledIndices.push_back(index);
    }
    if (enabledIndices.size() < 2) return;
    auto anchorIt = std::find(enabledIndices.begin(), enabledIndices.end(), anchorSourceIndex);
    if (anchorIt == enabledIndices.end()) anchorIt = enabledIndices.begin();
    const int anchorPosition = static_cast<int>(std::distance(enabledIndices.begin(), anchorIt));
    const int anchorIndex = *anchorIt;

    auto applyCenter = [this, anchorIndex](int index, double centerHz) {
        if (index == anchorIndex) return;
        SourceUi &source = *sources[static_cast<std::size_t>(index)];
        const double oldCenterHz = source.centerMhz->value() * 1.0e6;
        if (hostPrimary && index == 0) {
            emit primaryRetuneRequested(centerHz - oldCenterHz);
            const QSignalBlocker blocker(source.centerMhz);
            source.centerMhz->setValue(centerHz / 1.0e6);
            source.centerMhz->setProperty("lastCenterHz", centerHz);
        } else if (source.receiver->isRunning()) {
            retuneSource(index, centerHz);
        } else {
            const QSignalBlocker blocker(source.centerMhz);
            source.centerMhz->setValue(centerHz / 1.0e6);
        }
    };

    double adjacentCenterHz = sources[static_cast<std::size_t>(anchorIndex)]->centerMhz->value() * 1.0e6;
    double adjacentWidthHz = sourceUsefulBandwidthHz(anchorIndex);
    for (int position = anchorPosition + 1; position < static_cast<int>(enabledIndices.size()); ++position) {
        const int index = enabledIndices[static_cast<std::size_t>(position)];
        const double widthHz = sourceUsefulBandwidthHz(index);
        const double overlapHz = std::min(adjacentWidthHz, widthHz) *
                                 alignmentOverlapSpin->value() / 100.0;
        const double centerHz = adjacentCenterHz + 0.5 * (adjacentWidthHz + widthHz) - overlapHz;
        applyCenter(index, centerHz);
        adjacentCenterHz = centerHz;
        adjacentWidthHz = widthHz;
    }

    adjacentCenterHz = sources[static_cast<std::size_t>(anchorIndex)]->centerMhz->value() * 1.0e6;
    adjacentWidthHz = sourceUsefulBandwidthHz(anchorIndex);
    for (int position = anchorPosition - 1; position >= 0; --position) {
        const int index = enabledIndices[static_cast<std::size_t>(position)];
        const double widthHz = sourceUsefulBandwidthHz(index);
        const double overlapHz = std::min(adjacentWidthHz, widthHz) *
                                 alignmentOverlapSpin->value() / 100.0;
        const double centerHz = adjacentCenterHz - 0.5 * (adjacentWidthHz + widthHz) + overlapHz;
        applyCenter(index, centerHz);
        adjacentCenterHz = centerHz;
        adjacentWidthHz = widthHz;
    }
}

void MultiSdrDialog::consumeFrame(int sourceIndex, const AnalyzerSpectrumFrame &frame) {
    SourceUi &source = *sources[static_cast<std::size_t>(sourceIndex)];
    source.lastFrame = frame;
    source.hasFrame = true;
    source.status->setText(
        frequencyText(frame.minimumHz) + QStringLiteral(" .. ") + frequencyText(frame.maximumHz) +
        QStringLiteral(" | ") + QString::number(frame.levelsDb.size()) + text(" bins", " бінів"));
    previewMosaicEngine.setSourceSettings(sourceIndex, previewMosaicSettings(sourceIndex));
    previewMosaicEngine.updateFrame(sourceIndex, frame);
    mainMosaicEngine.setSourceSettings(sourceIndex, mainMosaicSettings(sourceIndex));
    mainMosaicEngine.updateFrame(sourceIndex, frame);
    refreshMosaic();
}

void MultiSdrDialog::refreshMosaic(bool force) {
    const int minimumIntervalMs = activeSourceCount > 2 ? 33 : 16;
    if (!force && mosaicRefreshClock.isValid() &&
        mosaicRefreshClock.elapsed() < minimumIntervalMs)
        return;
    mosaicRefreshClock.restart();
    previewMosaicEngine.setMaximumFrameAgeMs(frameAgeSpin->value());
    mainMosaicEngine.setMaximumFrameAgeMs(frameAgeSpin->value());
    const auto mainResult = mainMosaicEngine.build();
    latestMainMosaicValid = mainResult.has_value() &&
                            mainResult->contributingSources.size() >= 2;
    if (latestMainMosaicValid) latestMainMosaicFrame = mainResult->frame;
    if (!isVisible()) return;
    const auto result = previewMosaicEngine.build();
    if (!result.has_value()) return;
    const AnalyzerSpectrumFrame &frame = result->frame;
    mosaicLevels = frame.levelsDb;
    mosaicFrequencies.resize(frame.levelsDb.size());
    const double denominator = static_cast<double>(std::max<std::size_t>(1, frame.levelsDb.size() - 1));
    for (std::size_t index = 0; index < mosaicFrequencies.size(); ++index) {
        mosaicFrequencies[index] = static_cast<float>(
            frame.minimumHz + (frame.maximumHz - frame.minimumHz) * index / denominator);
    }
    spectrum->setFrame(mosaicFrequencies, mosaicLevels);
    if (previewWaterfall)
        previewWaterfall->appendFrame(mosaicLevels, topDbSpin->value(), bottomDbSpin->value());
    statusLabel->setText(
        text("Mosaic", "Мозаїка") + QStringLiteral(": ") +
        frequencyText(frame.minimumHz) + QStringLiteral(" .. ") + frequencyText(frame.maximumHz) +
        QStringLiteral(" | ") + text("sources", "джерел") + QStringLiteral(": ") +
        QString::number(result->contributingSources.size()) + QStringLiteral(" | ") +
        text("overlap bins", "біни перекриття") + QStringLiteral(": ") +
        QString::number(result->overlapBins) + QStringLiteral(" | ") +
        text("gap bins", "біни прогалин") + QStringLiteral(": ") +
        QString::number(result->uncoveredBins));
}

void MultiSdrDialog::panMosaic(double deltaHz) {
    const int target = panTargetCombo->currentData().toInt();
    if (target >= 0) {
        SourceUi &source = *sources[static_cast<std::size_t>(target)];
        if (hostPrimary && target == 0) {
            emit primaryRetuneRequested(deltaHz);
        } else if (source.receiver->isRunning()) {
            retuneSource(target, source.centerMhz->value() * 1.0e6 + deltaHz);
        }
        return;
    }
    for (int index = 0; index < static_cast<int>(sources.size()); ++index) {
        SourceUi &source = *sources[static_cast<std::size_t>(index)];
        if (!source.active) continue;
        if (hostPrimary && index == 0) {
            emit primaryRetuneRequested(deltaHz);
        } else if (source.receiver->isRunning()) {
            retuneSource(index, source.centerMhz->value() * 1.0e6 + deltaHz);
        }
    }
}

void MultiSdrDialog::retuneSource(int sourceIndex, double centerHz) {
    SourceUi &source = *sources[static_cast<std::size_t>(sourceIndex)];
    if (hostPrimary && sourceIndex == 0) {
        emit primaryRetuneRequested(centerHz - source.centerMhz->value() * 1.0e6);
        return;
    }
    const AnalyzerReceiverSettings settings = receiverSettings(sourceIndex);
    if (!settings.directSampling)
        centerHz = std::max(centerHz, settings.sampleRateHz * 0.5);
    centerHz = std::clamp(centerHz, 0.0, 8.0e9);
    if (source.receiver->retune(centerHz)) {
        const QSignalBlocker blocker(source.centerMhz);
        source.centerMhz->setValue(centerHz / 1.0e6);
    }
}

bool MultiSdrDialog::mainMosaicFrame(AnalyzerSpectrumFrame &frame) const {
    if (mainIncludedReceiverCount() < 2) return false;
    if (mainViewMode == 0) {
        if (!latestMainMosaicValid) return false;
        frame = latestMainMosaicFrame;
        return true;
    }

    int sourceIndex = focusedMainSource;
    if (sourceIndex < 0 || sourceIndex >= static_cast<int>(sources.size()) ||
        !sources[static_cast<std::size_t>(sourceIndex)]->active ||
        !sources[static_cast<std::size_t>(sourceIndex)]->enabled->isChecked()) {
        sourceIndex = -1;
        for (int index = 0; index < static_cast<int>(sources.size()); ++index) {
            const SourceUi &candidate = *sources[static_cast<std::size_t>(index)];
            if (candidate.active && candidate.enabled->isChecked()) {
                sourceIndex = index;
                break;
            }
        }
    }
    if (sourceIndex < 0) return false;
    const SourceUi &source = *sources[static_cast<std::size_t>(sourceIndex)];
    if (!source.hasFrame || source.lastFrame.levelsDb.size() < 2 ||
        !(source.lastFrame.maximumHz > source.lastFrame.minimumHz)) return false;

    const std::size_t count = source.lastFrame.levelsDb.size();
    const std::size_t trim = std::min<std::size_t>(
        count / 2 - 1,
        static_cast<std::size_t>(std::floor(count * source.edgeTrim->value() / 100.0)));
    const std::size_t first = trim;
    const std::size_t last = count - trim;
    if (last <= first + 1) return false;
    const double binWidthHz = (source.lastFrame.maximumHz - source.lastFrame.minimumHz) /
                              static_cast<double>(count - 1);
    frame = source.lastFrame;
    frame.minimumHz = source.lastFrame.minimumHz + binWidthHz * static_cast<double>(first);
    frame.maximumHz = source.lastFrame.minimumHz + binWidthHz * static_cast<double>(last - 1);
    frame.centerHz = 0.5 * (frame.minimumHz + frame.maximumHz);
    frame.levelsDb.assign(source.lastFrame.levelsDb.begin() + static_cast<std::ptrdiff_t>(first),
                          source.lastFrame.levelsDb.begin() + static_cast<std::ptrdiff_t>(last));
    const float levelOffset = static_cast<float>(source.levelOffset->value());
    if (std::abs(levelOffset) > 0.000001f) {
        for (float &level : frame.levelsDb) {
            if (std::isfinite(level)) level += levelOffset;
        }
    }
    frame.sourceFftLength = static_cast<int>(frame.levelsDb.size());
    return true;
}

int MultiSdrDialog::selectAudioSourceForFrequency(double frequencyHz) {
    if (!std::isfinite(frequencyHz)) return 0;

    std::vector<int> candidates;
    for (int index = 0; index < static_cast<int>(sources.size()); ++index) {
        const SourceUi &source = *sources[static_cast<std::size_t>(index)];
        const bool running = hostPrimary && index == 0
                                 ? primaryRunning
                                 : source.receiver->isRunning();
        if (!source.active || !source.enabled->isChecked() || !running || !source.hasFrame)
            continue;
        if (index > 0 && receiverSettings(index).directSampling)
            continue;
        const double spanHz = source.lastFrame.maximumHz - source.lastFrame.minimumHz;
        const double trimHz = std::max(0.0, spanHz) * source.edgeTrim->value() / 100.0;
        const double usableMinimumHz = source.lastFrame.minimumHz + trimHz;
        const double usableMaximumHz = source.lastFrame.maximumHz - trimHz;
        if (frequencyHz >= usableMinimumHz && frequencyHz <= usableMaximumHz)
            candidates.push_back(index);
    }

    int selected = 0;
    const int current = audioSourceIndex.load(std::memory_order_acquire);
    if (std::find(candidates.begin(), candidates.end(), current) != candidates.end()) {
        selected = current;
    } else if (!candidates.empty()) {
        selected = *std::min_element(candidates.begin(), candidates.end(),
            [this, frequencyHz](int left, int right) {
                const double leftDistance = std::abs(
                    sources[static_cast<std::size_t>(left)]->lastFrame.centerHz - frequencyHz);
                const double rightDistance = std::abs(
                    sources[static_cast<std::size_t>(right)]->lastFrame.centerHz - frequencyHz);
                return leftDistance < rightDistance;
            });
    }

    if (audioSourceIndex.exchange(selected, std::memory_order_acq_rel) != selected)
        emit audioSourceChanged(selected);
    return selected;
}

void MultiSdrDialog::setMainViewMode(int mode) {
    mainViewMode = mode == 1 ? 1 : 0;
}

int MultiSdrDialog::mainIncludedReceiverCount() const {
    int count = 0;
    for (const auto &source : sources) {
        if (source->active && source->enabled->isChecked()) ++count;
    }
    return count;
}

int MultiSdrDialog::focusedMainReceiverNumber() const {
    return focusedMainSource + 1;
}

void MultiSdrDialog::focusAdjacentMainReceiver(int direction) {
    std::vector<int> included;
    for (int index = 0; index < static_cast<int>(sources.size()); ++index) {
        const SourceUi &source = *sources[static_cast<std::size_t>(index)];
        if (source.active && source.enabled->isChecked()) included.push_back(index);
    }
    if (included.size() < 2) return;
    auto current = std::find(included.begin(), included.end(), focusedMainSource);
    int position = current == included.end()
                       ? 0
                       : static_cast<int>(std::distance(included.begin(), current));
    const int count = static_cast<int>(included.size());
    position = (position + (direction < 0 ? -1 : 1) + count) % count;
    focusedMainSource = included[static_cast<std::size_t>(position)];
}

void MultiSdrDialog::panMainDisplay(double deltaHz) {
    if (!std::isfinite(deltaHz) || std::abs(deltaHz) < 0.01) return;

    std::vector<int> included;
    included.reserve(sources.size());
    for (int index = 0; index < static_cast<int>(sources.size()); ++index) {
        const SourceUi &source = *sources[static_cast<std::size_t>(index)];
        if (source.active && source.enabled->isChecked()) included.push_back(index);
    }
    if (included.empty()) return;

    const bool panAsGroup = synchronizedPanCheckbox->isChecked() || mainViewMode == 0;
    if (!panAsGroup) {
        int sourceIndex = focusedMainSource;
        if (std::find(included.begin(), included.end(), sourceIndex) == included.end())
            sourceIndex = included.front();
        SourceUi &source = *sources[static_cast<std::size_t>(sourceIndex)];
        const double targetHz = source.centerMhz->value() * 1.0e6 + deltaHz;
        if (hostPrimary && sourceIndex == 0) {
            emit primaryRetuneRequested(deltaHz);
        } else if (source.receiver->isRunning()) {
            retuneSource(sourceIndex, targetHz);
        } else {
            const QSignalBlocker blocker(source.centerMhz);
            source.centerMhz->setValue(targetHz / 1.0e6);
        }
        saveSettings();
        return;
    }

    // Use one bounded delta for the whole main mosaic. Receiver spacing is
    // therefore invariant even when the displayed bands have a large gap.
    double minimumDeltaHz = -std::numeric_limits<double>::infinity();
    double maximumDeltaHz = std::numeric_limits<double>::infinity();
    for (int index : included) {
        const SourceUi &source = *sources[static_cast<std::size_t>(index)];
        const double currentHz = source.centerMhz->value() * 1.0e6;
        double minimumHz = source.centerMhz->minimum() * 1.0e6;
        if (!(hostPrimary && index == 0)) {
            const AnalyzerReceiverSettings settings = receiverSettings(index);
            if (!settings.directSampling)
                minimumHz = std::max(minimumHz, settings.sampleRateHz * 0.5);
        }
        const double maximumHz = source.centerMhz->maximum() * 1.0e6;
        minimumDeltaHz = std::max(minimumDeltaHz, minimumHz - currentHz);
        maximumDeltaHz = std::min(maximumDeltaHz, maximumHz - currentHz);
    }
    const double synchronizedDeltaHz = std::clamp(deltaHz, minimumDeltaHz, maximumDeltaHz);
    if (!std::isfinite(synchronizedDeltaHz) || std::abs(synchronizedDeltaHz) < 0.01) return;

    for (int index : included) {
        SourceUi &source = *sources[static_cast<std::size_t>(index)];
        const double targetHz = source.centerMhz->value() * 1.0e6 + synchronizedDeltaHz;
        if (hostPrimary && index == 0) {
            pendingSynchronizedPrimaryCenterHz = targetHz;
            {
                const QSignalBlocker blocker(source.centerMhz);
                source.centerMhz->setValue(targetHz / 1.0e6);
            }
            source.centerMhz->setProperty("lastCenterHz", targetHz);
            emit primaryRetuneRequested(synchronizedDeltaHz);
        } else if (source.receiver->isRunning()) {
            retuneSource(index, targetHz);
        } else {
            const QSignalBlocker blocker(source.centerMhz);
            source.centerMhz->setValue(targetHz / 1.0e6);
        }
    }
    saveSettings();
}

void MultiSdrDialog::saveSettings() const {
    if (restoringSettings) return;
    QSettings settings;
    settings.beginGroup(QStringLiteral("multiSdr"));
    settings.setValue(QStringLiteral("geometry"), saveGeometry());
    settings.setValue(QStringLiteral("panTarget"), panTargetCombo->currentData().toInt());
    settings.setValue(QStringLiteral("synchronizedPan"), synchronizedPanCheckbox->isChecked());
    settings.setValue(QStringLiteral("frameAgeMs"), frameAgeSpin->value());
    settings.setValue(QStringLiteral("topDb"), topDbSpin->value());
    settings.setValue(QStringLiteral("bottomDb"), bottomDbSpin->value());
    settings.setValue(QStringLiteral("autoAlign"), autoAlignCheckbox->isChecked());
    settings.setValue(QStringLiteral("alignmentOverlap"), alignmentOverlapSpin->value());
    settings.setValue(QStringLiteral("sourceCount"), activeSourceCount);
    for (int index = 0; index < static_cast<int>(sources.size()); ++index) {
        const SourceUi &source = *sources[static_cast<std::size_t>(index)];
        settings.beginGroup(QStringLiteral("rx%1").arg(index + 1));
        settings.setValue(QStringLiteral("backend"), source.device->currentData(Qt::UserRole));
        settings.setValue(QStringLiteral("nativeIndex"), source.device->currentData(Qt::UserRole + 1));
        settings.setValue(QStringLiteral("input"), source.input->currentData());
        settings.setValue(QStringLiteral("centerMhz"), source.centerMhz->value());
        settings.setValue(QStringLiteral("sampleRate"), source.sampleRate->currentData());
        settings.setValue(QStringLiteral("fft"), source.fft->currentData());
        settings.setValue(QStringLiteral("lna"), source.lna->value());
        settings.setValue(QStringLiteral("vga"), source.vga->value());
        settings.setValue(QStringLiteral("edgeTrim"), source.edgeTrim->value());
        settings.setValue(QStringLiteral("levelOffset"), source.levelOffset->value());
        settings.setValue(QStringLiteral("enabled"), source.enabled->isChecked());
        settings.setValue(QStringLiteral("active"), source.active);
        settings.endGroup();
    }
    settings.endGroup();
    settings.sync();
}

void MultiSdrDialog::restoreSettings() {
    restoringSettings = true;
    QSettings settings;
    settings.beginGroup(QStringLiteral("multiSdr"));
    restoreGeometry(settings.value(QStringLiteral("geometry")).toByteArray());
    const int panTarget = settings.value(QStringLiteral("panTarget"), -1).toInt();
    synchronizedPanCheckbox->setChecked(
        settings.value(QStringLiteral("synchronizedPan"), true).toBool());
    panTargetCombo->setCurrentIndex(std::max(0, panTargetCombo->findData(panTarget)));
    frameAgeSpin->setValue(settings.value(QStringLiteral("frameAgeMs"), 750).toInt());
    topDbSpin->setValue(settings.value(QStringLiteral("topDb"), 0.0).toDouble());
    bottomDbSpin->setValue(settings.value(QStringLiteral("bottomDb"), -140.0).toDouble());
    {
        const QSignalBlocker blocker(autoAlignCheckbox);
        autoAlignCheckbox->setChecked(settings.value(QStringLiteral("autoAlign"), false).toBool());
    }
    alignmentOverlapSpin->setValue(settings.value(QStringLiteral("alignmentOverlap"), 5.0).toDouble());
    alignmentOverlapSpin->setEnabled(autoAlignCheckbox->isChecked());
    const int legacySourceCount = std::clamp(
        settings.value(QStringLiteral("sourceCount"), 2).toInt(), 1,
        static_cast<int>(sources.size()));
    activeSourceCount = 0;
    for (int index = 0; index < static_cast<int>(sources.size()); ++index) {
        SourceUi &source = *sources[static_cast<std::size_t>(index)];
        settings.beginGroup(QStringLiteral("rx%1").arg(index + 1));
        source.active = index == 0 || settings.value(
            QStringLiteral("active"), index < legacySourceCount).toBool();
        if (source.active) ++activeSourceCount;
        source.device->setProperty("savedBackend", settings.value(QStringLiteral("backend")));
        source.device->setProperty("savedNativeIndex", settings.value(QStringLiteral("nativeIndex"), 0));
        source.input->setProperty("savedInput", settings.value(QStringLiteral("input"), 0));
        source.centerMhz->setValue(settings.value(
            QStringLiteral("centerMhz"), index == 0 ? 90.0 : 125.0).toDouble());
        source.sampleRate->setProperty("savedRate", settings.value(
            QStringLiteral("sampleRate"), index == 0 ? 50.0e6 : 20.0e6));
        const int fft = settings.value(QStringLiteral("fft"), 65536).toInt();
        source.fft->setCurrentIndex(std::max(0, source.fft->findData(fft)));
        source.lna->setValue(settings.value(QStringLiteral("lna"), 0).toInt());
        source.vga->setValue(settings.value(QStringLiteral("vga"), 0).toInt());
        source.edgeTrim->setValue(settings.value(QStringLiteral("edgeTrim"), 5.0).toDouble());
        source.levelOffset->setValue(settings.value(QStringLiteral("levelOffset"), 0.0).toDouble());
        source.enabled->setChecked(settings.value(QStringLiteral("enabled"), true).toBool());
        settings.endGroup();
    }
    {
        const QSignalBlocker blocker(panTargetCombo);
        panTargetCombo->clear();
        panTargetCombo->addItem(text("Pan: synchronized group", "Рух: синхронна група"), -1);
        panTargetCombo->addItem(text("Pan: RX 1 only", "Рух: тільки RX 1"), 0);
    }
    for (int index = 0; index < static_cast<int>(sources.size()); ++index) {
        SourceUi &source = *sources[static_cast<std::size_t>(index)];
        source.group->setVisible(source.active);
        if (index > 0 && source.active && panTargetCombo->findData(index) < 0) {
            panTargetCombo->addItem(text("Pan: selected RX", "Рух: вибраний RX") +
                                        QStringLiteral(" %1").arg(index + 1), index);
        }
    }
    panTargetCombo->setCurrentIndex(std::max(0, panTargetCombo->findData(panTarget)));
    addReceiverButton->setEnabled(activeSourceCount < static_cast<int>(sources.size()));
    removeReceiverButton->setEnabled(activeSourceCount > 1);
    startAllButton->setText(activeSourceCount == 2
                                ? text("Start both", "Запустити обидва")
                                : text("Start all", "Запустити всі"));
    settings.endGroup();
    restoringSettings = false;
}

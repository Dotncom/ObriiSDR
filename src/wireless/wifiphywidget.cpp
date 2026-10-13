#include "wifiphywidget.h"
#include "wifiliveiqworker.h"
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QSettings>
#include <QSaveFile>
#include <QStandardPaths>
#include <QThread>
#include <QVBoxLayout>
#include <cmath>

QString WifiPhyWidget::text(const char *en, const char *uk) const { return QString::fromUtf8(ukrainian ? uk : en); }
WifiPhyWidget::WifiPhyWidget(bool uk, QWidget *parent) : QWidget(parent), ukrainian(uk), process(new QProcess(this)) {
    auto *layout = new QVBoxLayout(this); auto *form = new QFormLayout;
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    QSettings settings("ObriiSDR", "WirelessLab");
    QString program;
#ifdef Q_OS_WIN
    program = QDir(QCoreApplication::applicationDirPath()).filePath("wifi-phy/python.exe");
#else
    program = QDir(QCoreApplication::applicationDirPath()).filePath("wireless/python-phy");
    if (!QFileInfo::exists(program)) program = QStandardPaths::findExecutable("python3");
#endif
    const auto row = [this](QLineEdit *edit, bool folder) {
        auto *box = new QHBoxLayout; auto *browse = new QPushButton(text("Open", "Відкрити"), this);
        box->addWidget(edit, 1); box->addWidget(browse);
        connect(browse, &QPushButton::clicked, this, [this, edit, folder] {
            const QString initial = edit == input && edit->text().isEmpty()
                ? QDir(QCoreApplication::applicationDirPath()).filePath("recordings/spectrum") : edit->text();
            const QString path = folder ? QFileDialog::getExistingDirectory(this, text("Output folder", "Папка результатів"), initial)
                : QFileDialog::getOpenFileName(this, text("Select file", "Оберіть файл"), initial,
                    edit == input ? QStringLiteral("IQ recordings (*.iq8 *.cf32 *.cs16 *.cs8 *.wav);;All files (*)") : QString());
            if (path.isEmpty()) return;
            edit->setText(path);
            if (edit == input) {
                const QFileInfo info(path);
                if (info.suffix().compare(QStringLiteral("iq8"), Qt::CaseInsensitive) == 0)
                    format->setCurrentText(QStringLiteral("cs8"));
                for (const auto &sidecar : {path + ".json", info.dir().filePath(info.completeBaseName() + ".json")}) {
                    QFile file(sidecar); if (!file.open(QIODevice::ReadOnly) || file.size() > 262144) continue;
                    const auto object = QJsonDocument::fromJson(file.readAll()).object();
                    if (object.value("format").toString() == QStringLiteral("full_iq_raw_s8_interleaved"))
                        format->setCurrentText(QStringLiteral("cs8"));
                    const double rate = object.value("sampleRate").toDouble(object.value("sampleRateHz").toDouble());
                    const double frequency = object.value("centerFrequency").toDouble(object.value("centerFrequencyHz").toDouble());
                    if (rate > 0) sampleRate->setValue(rate / 1e6);
                    if (frequency > 0) { center->setValue(frequency / 1e6); channel->setValue(frequency / 1e6); }
                    break;
                }
            }
        }); return box;
    };
    const QString saved = settings.value("phy/python").toString();
    legacyPythonPath = QFileInfo::exists(saved) ? saved : program;
    const QString extendedDefault = QDir(QCoreApplication::applicationDirPath()).filePath("wifi-ht/python.exe");
    const QString extendedSaved = settings.value("phy/extendedPython", extendedDefault).toString();
    extendedPythonPath = QFileInfo(extendedSaved).isFile() ? extendedSaved : extendedDefault;
    phyMode = new QComboBox(this);
    phyMode->addItem(text("Legacy a/g (stable)", "Legacy a/g (стабільний)"));
    phyMode->addItem(text("HT20 (experimental)", "HT20 (експериментальний)"));
    phyMode->setCurrentIndex(settings.value("phy/mode", 0).toInt() == 1 ? 1 : 0);
    python = new QLineEdit(phyMode->currentIndex() == 1 ? extendedPythonPath : legacyPythonPath, this);
    connect(phyMode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (index == 1) { legacyPythonPath = python->text().trimmed(); python->setText(extendedPythonPath); }
        else { extendedPythonPath = python->text().trimmed(); python->setText(legacyPythonPath); }
    });
    input = new QLineEdit(this);
    directory = new QLineEdit(settings.value("phy/output", QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/ObriiSDR/Wireless").toString(), this);
    form->addRow(text("PHY mode", "Режим PHY"), phyMode);
    form->addRow(text("PHY Python", "Python рушія PHY"), row(python, false));
    form->addRow(text("IQ recording", "Запис IQ"), row(input, false));
    form->addRow(text("Output folder", "Папка результатів"), row(directory, true));
    format = new QComboBox(this); format->addItems({"cf32", "cs16", "cs8"});
    format->setCurrentText(settings.value("phy/format", "cf32").toString());
    form->addRow(text("Raw format / I+jQ", "Сирий формат / I+jQ"), format);
    sampleRate = new QDoubleSpinBox(this); sampleRate->setDecimals(6); sampleRate->setRange(0.001, 1000); sampleRate->setSuffix(" MS/s"); sampleRate->setValue(settings.value("phy/rate", 20).toDouble());
    sampleRate->setToolTip(text("Recording sample rate. Stereo PCM16 WAV uses its header. The whole selected 20 MHz channel must be captured.", "Частота семплів запису. Stereo PCM16 WAV використовує заголовок. У записі має вміщатися весь обраний канал 20 МГц."));
    center = new QDoubleSpinBox(this); center->setDecimals(6); center->setRange(1, 60000); center->setSuffix(" MHz"); center->setValue(settings.value("phy/center", 2412).toDouble());
    channel = new QDoubleSpinBox(this); channel->setDecimals(6); channel->setRange(1, 60000); channel->setSuffix(" MHz"); channel->setValue(settings.value("phy/channel", 2412).toDouble());
    threshold = new QDoubleSpinBox(this); threshold->setDecimals(2); threshold->setRange(0.1, 0.95); threshold->setSingleStep(0.01); threshold->setValue(settings.value("phy/threshold", 0.56).toDouble());
    form->addRow(text("Recording sample rate", "Частота семплів запису"), sampleRate);
    form->addRow(text("Recording center", "Центр запису"), center);
    form->addRow(text("Wi-Fi channel center", "Центр каналу Wi-Fi"), channel);
    form->addRow(text("Sync threshold", "Поріг синхронізації"), threshold);
    layout->addLayout(form);
    auto *buttons = new QHBoxLayout;
    startButton = new QPushButton(text("Decode IQ", "Декодувати IQ"), this); stopButton = new QPushButton(text("Stop", "Стоп"), this);
    openButton = new QPushButton(text("Inspect packets", "Переглянути пакети"), this);
    stopButton->setEnabled(false); openButton->setEnabled(false);
    liveStartButton = new QPushButton(text("Decode live IQ", "Декодувати живий IQ"), this);
    liveStopButton = new QPushButton(text("Stop live", "Стоп live"), this);
    buttons->addWidget(startButton); buttons->addWidget(stopButton); buttons->addWidget(openButton);
    buttons->addWidget(liveStartButton); buttons->addWidget(liveStopButton); layout->addLayout(buttons);
    liveSourceStatus = new QLabel(this);
    liveSourceStatus->setWordWrap(true);
    layout->addWidget(liveSourceStatus);
    updateLiveButtons();
    status = new QLabel(this); status->setWordWrap(true); layout->addWidget(status);
    phyStatus = new QLabel(this);
    phyStatus->setWordWrap(true);
    phyStatus->setToolTip(text("Synchronization candidates are not confirmed packets. HT/VHT-SIG CRC OK confirms a header, not its payload. Data CRC pass/fail counts completed decode attempts. VHT bandwidth is read from decoded headers; the current data decoder supports only a subset of VHT20. Live IQ queue drops are saved in the JSON sidecar.",
                                 "Кандидати синхронізації ще не підтверджені пакети. HT/VHT-SIG CRC OK підтверджує заголовок, але не його дані. CRC даних рахує завершені спроби декодування. Ширина VHT прочитана із заголовків; декодер даних підтримує лише частину VHT20. Втрати черги живого IQ зберігаються у JSON-файлі."));
    layout->addWidget(phyStatus);
    const auto showMode = [this] {
        phyStatus->clear();
        status->setText(phyMode->currentIndex() == 1
            ? text("IEEE 802.11 HT20 experimental. Idle.", "IEEE 802.11 HT20 експериментальний. Очікування.")
            : text("IEEE 802.11 a/g OFDM, 20 MHz. Idle.", "IEEE 802.11 a/g OFDM, 20 МГц. Очікування."));
    };
    connect(phyMode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, showMode);
    showMode();
    log = new QPlainTextEdit(this); log->setReadOnly(true); log->setMaximumBlockCount(150); layout->addWidget(log, 1);
    process->setProcessChannelMode(QProcess::MergedChannels);
#ifdef Q_OS_WIN
    process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) { args->flags |= 0x08000000 | 0x00004000; });
#endif
    connect(startButton, &QPushButton::clicked, this, &WifiPhyWidget::start);
    connect(stopButton, &QPushButton::clicked, this, &WifiPhyWidget::stop);
    connect(liveStartButton, &QPushButton::clicked, this, &WifiPhyWidget::startLive);
    connect(liveStopButton, &QPushButton::clicked, this, &WifiPhyWidget::stopLive);
    connect(openButton, &QPushButton::clicked, this, [this] { emit captureReady(result); });
    connect(process, &QProcess::readyReadStandardOutput, this, &WifiPhyWidget::readProcessOutput);
    connect(process, &QProcess::errorOccurred, this, [this] {
        status->setText(process->errorString());
        stopButton->setEnabled(false);
        updateLiveButtons();
    });
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, [this](int code, QProcess::ExitStatus exit) {
        processOutputBuffer += process->readAllStandardOutput();
        if (!processOutputBuffer.isEmpty() && !processOutputBuffer.endsWith('\n')) processOutputBuffer += '\n';
        readProcessOutput();
        if (phyMode->currentIndex() == 1) {
            QJsonObject phy;
            QFile metadata(result + QStringLiteral(".json"));
            if (metadata.exists() && metadata.size() <= 256 * 1024 && metadata.open(QIODevice::ReadOnly)) {
                const QJsonDocument document = QJsonDocument::fromJson(metadata.readAll());
                metadata.close();
                if (document.isObject()) {
                    QJsonObject root = document.object();
                    phy = root.value(QStringLiteral("phy")).toObject();
                    phy.insert(QStringLiteral("ht_diagnostics"), filePhyDiagnostics.htJson());
                    phy.insert(QStringLiteral("vht_diagnostics"), filePhyDiagnostics.vhtJson());
                    root.insert(QStringLiteral("phy"), phy);
                    QSaveFile saved(result + QStringLiteral(".json"));
                    if (saved.open(QIODevice::WriteOnly)) {
                        saved.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
                        if (!saved.commit()) log->appendPlainText(text("Could not save PHY diagnostics.", "Не вдалося зберегти діагностику PHY."));
                    }
                }
            }
            if (phy.isEmpty()) {
                phy.insert(QStringLiteral("ht_diagnostics"), filePhyDiagnostics.htJson());
                phy.insert(QStringLiteral("vht_diagnostics"), filePhyDiagnostics.vhtJson());
            }
            showPhyStats(phy);
        }
        startButton->setEnabled(true); stopButton->setEnabled(false);
        updateLiveButtons();
        openButton->setEnabled(QFileInfo(result).size() > 24);
        status->setText(cancelled ? text("Cancelled. A partial PCAP may remain.", "Скасовано. Можливий частковий PCAP.") : code || exit != QProcess::NormalExit ? text("PHY failed; see details below.", "Помилка PHY; подробиці нижче.") :
            openButton->isEnabled() ? text("Packets ready. PCAP time is decoding time, not RF time.", "Пакети готові. Час PCAP — час декодування, не RF.") : text("No CRC-valid OFDM packets decoded.", "OFDM-пакетів із правильною CRC не декодовано."));
    });
    liveThread = new QThread(this);
    liveThread->setObjectName(QStringLiteral("WifiLivePhyThread"));
    liveWorker = new WifiLiveIqWorker;
    liveWorker->moveToThread(liveThread);
    connect(liveThread, &QThread::finished, liveWorker, &QObject::deleteLater);
    connect(liveWorker, &WifiLiveIqWorker::started, this, [this] {
        liveStarting = false;
        liveActive = true;
        emit liveCaptureStarted(result);
        emit liveActiveChanged(true);
        status->setText(text("Decoding live SDR IQ; packets appear as CRC-valid frames arrive.",
                             "Декодування живого IQ SDR; пакети з'являються після перевірки CRC."));
        updateLiveButtons();
    });
    connect(liveWorker, &WifiLiveIqWorker::finished, this, [this](bool successful, const QString &message) {
        const bool wasActive = liveActive;
        liveActive = liveStarting = liveStopping = false;
        if (wasActive) emit liveActiveChanged(false);
        openButton->setEnabled(QFileInfo(result).size() > 24);
        status->setText(successful ? text("Live capture complete.", "Живе захоплення завершено.") :
                         text("Live PHY stopped: ", "Живий PHY зупинено: ") + message);
        updateLiveButtons();
    });
    connect(liveWorker, &WifiLiveIqWorker::packetRow, this, &WifiPhyWidget::livePacketRow);
    connect(liveWorker, &WifiLiveIqWorker::phyStats, this, &WifiPhyWidget::showPhyStats);
    connect(liveWorker, &WifiLiveIqWorker::message, this, [this](const QString &message) {
        log->appendPlainText(message);
    });
    connect(liveWorker, &WifiLiveIqWorker::progress, this,
            [this](quint64 packets, quint64 dropped, qint64 buffered, double fedRate, quint64 fed, quint64 decoded) {
        status->setText(text("IQ: %1 MS/s (%2 M fed) | PHY read: %3 M | packets: %4 | drops: %5 | buffer: %6 KiB",
                             "IQ: %1 Мсемпл/с (%2 млн подано) | PHY прочитав: %3 млн | пакети: %4 | втрати: %5 | буфер: %6 КіБ")
                        .arg(fedRate, 0, 'f', 1).arg(fed / 1000000.0, 0, 'f', 1)
                        .arg(decoded / 1000000.0, 0, 'f', 1).arg(packets).arg(dropped)
                        .arg(buffered / 1024) +
                        (fed > static_cast<quint64>(2 * liveRate) &&
                         fedRate < liveRate / 1e6 * 0.85
                            ? text(" | Receiver/PHY is not keeping up with selected Fs.",
                                   " | Приймач/PHY не встигає за обраною Fs.")
                            : QString()));
    });
    liveThread->start();
}
void WifiPhyWidget::readProcessOutput() {
    processOutputBuffer += process->readAllStandardOutput();
    int start = 0;
    int newline;
    while ((newline = processOutputBuffer.indexOf('\n', start)) >= 0) {
        const QByteArray line = processOutputBuffer.mid(start, newline - start).trimmed();
        start = newline + 1;
        if (line.isEmpty() || filePhyDiagnostics.observe(line)) continue;
        const QJsonObject event = QJsonDocument::fromJson(line).object();
        if (event.value(QStringLiteral("phy")).isObject()) showPhyStats(event.value(QStringLiteral("phy")).toObject());
        else log->appendPlainText(QString::fromLocal8Bit(line.left(2048)));
    }
    if (start > 0) processOutputBuffer.remove(0, start);
    if (processOutputBuffer.size() > 65536) processOutputBuffer = processOutputBuffer.right(8192);
}
WifiPhyWidget::~WifiPhyWidget() {
    stop();
    QMetaObject::invokeMethod(liveWorker, [worker = liveWorker] { worker->stop(); },
                              Qt::BlockingQueuedConnection);
    liveThread->quit();
    liveThread->wait();
    saveSettings();
}
void WifiPhyWidget::stop() {
    stopLive();
    cancelled = true;
    if (process->state() != QProcess::NotRunning) {
        process->write("stop\n"); process->closeWriteChannel();
        if (!process->waitForFinished(500)) { process->kill(); process->waitForFinished(500); }
    }
}
void WifiPhyWidget::setLiveSource(double rate, double frequency, bool available) {
    liveRate = rate;
    liveCenter = frequency;
    liveAvailable = available;
    liveSourceStatus->setText(available
        ? text("Receiver: %1 MS/s, center %2 MHz. Select the Wi-Fi channel center above.",
               "Приймач: %1 Мсемпл/с, центр %2 МГц. Вище оберіть центр каналу Wi-Fi.")
              .arg(rate / 1e6, 0, 'f', 3).arg(frequency / 1e6, 0, 'f', 3)
        : text("Start an RF receiver for live IQ.", "Для живого IQ запустіть RF-приймач."));
    if (liveActive && (!available || std::abs(rate - sampleRate->value() * 1e6) > 1.0 ||
                       std::abs(frequency - center->value() * 1e6) > 1000.0)) stopLive();
    updateLiveButtons();
}
void WifiPhyWidget::appendLiveIq(const QByteArray &iq, double rate, double frequency, int count) {
    liveWorker->enqueue(iq, rate, frequency, count);
}
void WifiPhyWidget::setCaptureBusy(bool busy) {
    captureBusy = busy;
    updateLiveButtons();
}
void WifiPhyWidget::updateLiveButtons() {
    liveStartButton->setEnabled(liveAvailable && !captureBusy && !liveActive && !liveStarting && !liveStopping && process->state() == QProcess::NotRunning);
    liveStopButton->setEnabled(liveActive || liveStarting);
    startButton->setEnabled(!liveActive && !liveStarting && !liveStopping && process->state() == QProcess::NotRunning);
    const bool editable = !liveActive && !liveStarting && !liveStopping;
    for (auto *field : {sampleRate, center, channel, threshold}) field->setEnabled(editable);
    phyMode->setEnabled(editable && process->state() == QProcess::NotRunning);
}
void WifiPhyWidget::startLive() {
    if (!liveAvailable || captureBusy || liveActive || liveStarting || liveStopping || process->state() != QProcess::NotRunning) return;
    const QString script = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("wireless/wifi_phy_decoder.py"));
    const double selectedChannel = channel->value() * 1e6;
    const QString pythonPath = python->text().trimmed();
    if (!QFileInfo(script).isFile()) {
        status->setText(text("Wi-Fi PHY decoder script is missing: %1", "Відсутній скрипт декодера Wi-Fi PHY: %1").arg(script));
        return;
    }
    if (!QFileInfo(pythonPath).isFile()) {
        status->setText(text("Wi-Fi PHY Python is missing: %1", "Відсутній Python рушія Wi-Fi PHY: %1").arg(pythonPath));
        return;
    }
    if (liveRate < 20e6 || liveRate > 1e9 ||
        std::abs(liveCenter - selectedChannel) + 10e6 > liveRate / 2) {
        status->setText(text("A complete 20 MHz channel must fit the live IQ bandwidth.",
                             "У живій смузі IQ має вміщатися весь канал 20 МГц."));
        return;
    }
    const QString destination = QDir(directory->text()).filePath(
        QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz")));
    if (!QDir().mkpath(destination)) {
        status->setText(text("Cannot create output folder.", "Не вдалося створити папку результатів."));
        return;
    }
    const QString working = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/wireless-phy");
    if (!QDir().mkpath(working)) return;
    result = QDir(destination).filePath(QStringLiteral("wifi-live.pcap"));
    sampleRate->setValue(liveRate / 1e6);
    center->setValue(liveCenter / 1e6);
    log->clear(); phyStatus->clear(); openButton->setEnabled(false);
    QStringList arguments{QStringLiteral("-I"), QStringLiteral("-B"), script,
        QStringLiteral("--live"), QStringLiteral("--output"), result,
        QStringLiteral("--sample-rate"), QString::number(liveRate, 'g', 15),
        QStringLiteral("--center"), QString::number(liveCenter, 'g', 15),
        QStringLiteral("--channel"), QString::number(selectedChannel, 'g', 15),
        QStringLiteral("--threshold"), QString::number(threshold->value(), 'g', 5)};
    if (phyMode->currentIndex() == 1) arguments << QStringLiteral("--extended-phy");
    liveStarting = true;
    updateLiveButtons();
    status->setText(text("Starting live PHY...", "Запуск живого PHY..."));
    QMetaObject::invokeMethod(liveWorker,
        [worker = liveWorker, program = pythonPath, arguments, working,
         rate = liveRate, frequency = liveCenter] {
            worker->start(program, arguments, working, rate, frequency);
        }, Qt::QueuedConnection);
    saveSettings();
}
void WifiPhyWidget::stopLive() {
    if (!liveActive && !liveStarting) return;
    liveWorker->stopAccepting();
    const bool wasActive = liveActive;
    liveActive = false;
    liveStarting = false;
    liveStopping = true;
    if (wasActive) emit liveActiveChanged(false);
    liveStopButton->setEnabled(false);
    updateLiveButtons();
    QMetaObject::invokeMethod(liveWorker, [worker = liveWorker] { worker->stop(); }, Qt::QueuedConnection);
}
void WifiPhyWidget::saveSettings() {
    QSettings settings("ObriiSDR", "WirelessLab");
    if (phyMode->currentIndex() == 1) extendedPythonPath = python->text().trimmed();
    else legacyPythonPath = python->text().trimmed();
    settings.setValue("phy/python", legacyPythonPath);
    settings.setValue("phy/extendedPython", extendedPythonPath);
    settings.setValue("phy/mode", phyMode->currentIndex());
    settings.setValue("phy/output", directory->text());
    settings.setValue("phy/format", format->currentText()); settings.setValue("phy/rate", sampleRate->value());
    settings.setValue("phy/center", center->value()); settings.setValue("phy/channel", channel->value()); settings.setValue("phy/threshold", threshold->value());
}
void WifiPhyWidget::showPhyStats(const QJsonObject &stats) {
    QStringList rates;
    const QJsonObject counts = stats.value(QStringLiteral("rates_mbps")).toObject();
    for (int rate : {6, 9, 12, 18, 24, 36, 48, 54}) {
        const double count = counts.value(QString::number(rate)).toDouble();
        if (count > 0) rates << QStringLiteral("%1: %2").arg(rate).arg(count, 0, 'f', 0);
    }
    const auto value = [&stats](const char *key) {
        return QString::number(stats.value(QLatin1String(key)).toDouble(), 'f', 0);
    };
    QString result = text("PHY candidates: short %1 | long %2 | SIGNAL %3 | CRC valid %4",
                          "Кандидати PHY: коротка %1 | довга %2 | SIGNAL %3 | CRC добре %4")
                         .arg(value("short"), value("long"), value("signal"), value("crc_valid"));
    if (!rates.isEmpty()) result += text("\nLegacy rates, Mb/s: ", "\nШвидкості legacy, Мбіт/с: ") + rates.join(QStringLiteral(", "));
    const QJsonObject extended = stats.value(QStringLiteral("extended_encodings")).toObject();
    QStringList modern;
    for (auto it = extended.begin(); it != extended.end(); ++it)
        modern << QStringLiteral("%1: %2").arg(it.key()).arg(it.value().toInt());
    if (!modern.isEmpty()) result += text("\nExtended CRC-valid: ", "\nРозширені CRC-коректні: ") + modern.join(QStringLiteral(", "));
    const QJsonObject ht = stats.value(QStringLiteral("ht_diagnostics")).toObject();
    if (!ht.isEmpty()) {
        result += text("\nHT: SIG CRC OK %1 | data CRC pass %2 | fail %3",
                       "\nHT: SIG CRC OK %1 | CRC даних добре %2 | помилка %3")
            .arg(QString::number(ht.value(QStringLiteral("ht_sig_crc_ok")).toDouble(), 'f', 0),
                 QString::number(ht.value(QStringLiteral("ht_data_crc_pass")).toDouble(), 'f', 0),
                 QString::number(ht.value(QStringLiteral("ht_data_crc_fail")).toDouble(), 'f', 0));
    }
    const QJsonObject vht = stats.value(QStringLiteral("vht_diagnostics")).toObject();
    if (!vht.isEmpty()) {
        result += text("\nVHT: SIG CRC OK %1 | data CRC pass %2 | fail %3",
                       "\nVHT: SIG CRC OK %1 | CRC даних добре %2 | помилка %3")
            .arg(QString::number(vht.value(QStringLiteral("vht_sig_crc_ok")).toDouble(), 'f', 0),
                 QString::number(vht.value(QStringLiteral("vht_data_crc_pass")).toDouble(), 'f', 0),
                 QString::number(vht.value(QStringLiteral("vht_data_crc_fail")).toDouble(), 'f', 0));
        const QJsonObject bandwidths = vht.value(QStringLiteral("bandwidth_mhz")).toObject();
        QStringList bandwidthCounts;
        for (const char *bandwidth : {"20", "40", "80", "160"}) {
            const int count = bandwidths.value(QLatin1String(bandwidth)).toInt();
            if (count > 0) bandwidthCounts << QStringLiteral("%1 MHz: %2").arg(QLatin1String(bandwidth)).arg(count);
        }
        if (!bandwidthCounts.isEmpty())
            result += text(" | header widths ", " | ширина заголовків ") + bandwidthCounts.join(QStringLiteral(", "));
    }
    const QJsonObject frames = stats.value(QStringLiteral("frame_classes")).toObject();
    if (!frames.isEmpty())
        result += text("\nFrames: management %1 | control %2 | data %3",
                       "\nКадри: системні %1 | керуючі %2 | дані %3")
            .arg(frames.value(QStringLiteral("Management")).toInt())
            .arg(frames.value(QStringLiteral("Control")).toInt())
            .arg(frames.value(QStringLiteral("Data")).toInt());
    if (stats.value(QStringLiteral("avg_snr_db")).isDouble())
        result += QStringLiteral(" | SNR %1 dB").arg(stats.value(QStringLiteral("avg_snr_db")).toDouble(), 0, 'f', 1);
    phyStatus->setText(result);
}
void WifiPhyWidget::start() {
    if (liveActive || liveStarting || liveStopping || process->state() != QProcess::NotRunning) return;
    const QString script = QDir(QCoreApplication::applicationDirPath()).filePath("wireless/wifi_phy_decoder.py");
    if (!QFileInfo(script).isFile() || !QFileInfo(python->text().trimmed()).isFile() ||
        !QFileInfo(input->text()).isFile()) {
        status->setText(text("Select an IQ file and verify the PHY script and Python paths.",
                             "Оберіть запис IQ і перевірте шляхи до скрипта та Python рушія PHY."));
        return;
    }
    const QString destination = QDir(directory->text()).filePath(QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss-zzz"));
    if (!QDir().mkpath(destination)) { status->setText(text("Cannot create output folder.", "Не вдалося створити папку результатів.")); return; }
    const QString working = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/wireless-phy";
    if (!QDir().mkpath(working)) return;
    result = QDir(destination).filePath("wifi.pcap"); cancelled = false; log->clear(); phyStatus->clear(); processOutputBuffer.clear(); filePhyDiagnostics = {}; openButton->setEnabled(false);
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("PYTHONDONTWRITEBYTECODE", "1"); environment.remove("PYTHONPATH");
    process->setProcessEnvironment(environment); process->setWorkingDirectory(working);
    environment.insert("OPENBLAS_NUM_THREADS", "1"); environment.insert("OMP_NUM_THREADS", "1");
    process->setProcessEnvironment(environment);
    QStringList arguments{"-I", "-B", script, "--input", input->text(), "--output", result, "--format", format->currentText(), "--sample-rate", QString::number(sampleRate->value()*1e6, 'g', 15),
        "--center", QString::number(center->value()*1e6, 'g', 15), "--channel", QString::number(channel->value()*1e6, 'g', 15), "--threshold", QString::number(threshold->value(), 'g', 5)};
    if (phyMode->currentIndex() == 1) arguments << "--extended-phy";
    process->start(python->text().trimmed(), arguments);
    startButton->setEnabled(false); stopButton->setEnabled(true); saveSettings();
    updateLiveButtons();
    status->setText(text("Decoding OFDM...", "Декодування OFDM..."));
}

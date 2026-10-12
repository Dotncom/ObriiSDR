#include "wirelesslabdialog.h"
#include "wirelesscaptureworker.h"
#include <QAbstractTableModel>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTableView>
#include <QTableWidget>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

class WirelessPacketModel : public QAbstractTableModel {
public:
    explicit WirelessPacketModel(bool uk, QObject *parent) : QAbstractTableModel(parent) {
        headers = uk
            ? QStringList{QStringLiteral("#"), QStringLiteral("Час UTC"), QStringLiteral("Протокол"),
              QStringLiteral("Джерело"), QStringLiteral("Одержувач"), QStringLiteral("Байт"),
              QStringLiteral("BSSID"), QStringLiteral("SSID"), QStringLiteral("МГц"),
              QStringLiteral("RSSI dBm"), QStringLiteral("Тип"), QStringLiteral("Повтор"),
              QStringLiteral("Захищений"), QStringLiteral("FCS"), QStringLiteral("Seq"), QStringLiteral("Опис")}
            : QStringList{QStringLiteral("#"), QStringLiteral("UTC"), QStringLiteral("Protocol"),
              QStringLiteral("Source"), QStringLiteral("Destination"), QStringLiteral("Bytes"),
              QStringLiteral("BSSID"), QStringLiteral("SSID"), QStringLiteral("MHz"),
              QStringLiteral("RSSI dBm"), QStringLiteral("Type"), QStringLiteral("Retry"),
              QStringLiteral("Protected"), QStringLiteral("FCS"), QStringLiteral("Seq"), QStringLiteral("Info")};
    }
    int rowCount(const QModelIndex &p = {}) const override { return p.isValid() ? 0 : rows.size(); }
    int columnCount(const QModelIndex &p = {}) const override { return p.isValid() ? 0 : headers.size(); }
    QVariant headerData(int section, Qt::Orientation o, int role) const override {
        return role == Qt::DisplayRole && o == Qt::Horizontal ? QVariant(headers.value(section)) : QVariant();
    }
    QVariant data(const QModelIndex &index, int role) const override {
        if (!index.isValid() || role != Qt::DisplayRole) return {};
        const QString value = rows[index.row()].value(index.column());
        if (index.column() == 1) {
            bool ok = false;
            const double epoch = value.toDouble(&ok);
            if (ok && std::isfinite(epoch) && epoch >= 0 && epoch < 1.0e11)
                return QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(epoch * 1000), Qt::UTC)
                    .toString(QStringLiteral("HH:mm:ss.zzz"));
        }
        return value;
    }
    void append(QVector<QStringList> batch) {
        if (batch.size() > 5000) batch = batch.mid(batch.size() - 5000);
        const int remove = (std::max)(0, rows.size() + batch.size() - 5000);
        if (remove) { beginRemoveRows({}, 0, remove - 1); rows.remove(0, remove); endRemoveRows(); }
        if (batch.isEmpty()) return;
        beginInsertRows({}, rows.size(), rows.size() + batch.size() - 1);
        rows += batch;
        endInsertRows();
    }
    void clear() { beginResetModel(); rows.clear(); endResetModel(); }
    QStringList headers;
    QVector<QStringList> rows;
};

QString WirelessLabDialog::text(const char *en, const char *uk) const {
    return QString::fromUtf8(ukrainian ? uk : en);
}
WirelessLabDialog::WirelessLabDialog(bool uk, QWidget *parent)
    : QDialog(parent), ukrainian(uk), worker(new WirelessCaptureWorker),
      thread(new QThread(this)), timer(new QTimer(this)),
      model(new WirelessPacketModel(uk, this)) {
    setWindowTitle(text("Wireless lab (beta)", "Бездротова лабораторія (бета)"));
    resize(1050, 640);
    thread->setObjectName(QStringLiteral("WirelessPacketThread"));
    worker->moveToThread(thread);
    connect(thread, &QThread::finished, worker, &QObject::deleteLater);
    thread->start();
    auto *layout = new QVBoxLayout(this);
    auto *controls = new QGridLayout;
    programEdit = new QLineEdit(this);
    QString executable = QStandardPaths::findExecutable(QStringLiteral("tshark"));
#ifdef Q_OS_WIN
    if (executable.isEmpty() && QFileInfo::exists(QStringLiteral("C:/Program Files/Wireshark/tshark.exe")))
        executable = QStringLiteral("C:/Program Files/Wireshark/tshark.exe");
#endif
    QSettings settings(QStringLiteral("ObriiSDR"), QStringLiteral("WirelessLab"));
    restoreGeometry(settings.value(QStringLiteral("geometry")).toByteArray());
    programEdit->setText(settings.value(QStringLiteral("tshark"), executable).toString());
    auto *browse = new QPushButton(QStringLiteral("..."), this);
    browse->setMaximumWidth(36);
    protocolCombo = new QComboBox(this);
    protocolCombo->addItems({QStringLiteral("Wi-Fi / 802.11"), QStringLiteral("Bluetooth / BLE"),
                             QStringLiteral("GSM / 2G"), QStringLiteral("LTE")});
    protocolCombo->setCurrentIndex((std::clamp)(settings.value(QStringLiteral("protocol"), 0).toInt(), 0, 3));
    interfaceCombo = new QComboBox(this);
    interfaceCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    interfaceCombo->setMinimumContentsLength(16);
    refreshButton = new QPushButton(text("Interfaces", "Інтерфейси"), this);
    monitorCheckbox = new QCheckBox(text("Monitor mode", "Режим монітора"), this);
    monitorCheckbox->setToolTip(text("Requires a compatible Wi-Fi adapter and driver; may disconnect that adapter from its network.",
        "Потрібен сумісний Wi-Fi адаптер і драйвер; адаптер може від'єднатися від своєї мережі."));
    saveCheckbox = new QCheckBox(text("Save PCAPNG", "Зберігати PCAPNG"), this);
    saveCheckbox->setToolTip(text("Live saved captures ignore the display filter; limit: 5 minutes / 100 MiB.",
        "Збережений живий захват без фільтра показу; ліміт: 5 хвилин / 100 МіБ."));
    filterEdit = new QLineEdit(Wireless::displayFilter(static_cast<Wireless::Protocol>(protocolCombo->currentIndex())), this);
    profileEdit = new QLineEdit(settings.value(QStringLiteral("profile")).toString(), this);
    profileEdit->setPlaceholderText(text("Default", "Типовий"));
    controls->addWidget(new QLabel(QStringLiteral("TShark"), this), 0, 0);
    controls->addWidget(programEdit, 0, 1, 1, 3);
    controls->addWidget(browse, 0, 4);
    controls->addWidget(protocolCombo, 0, 5);
    controls->addWidget(new QLabel(text("Interface", "Інтерфейс"), this), 1, 0);
    controls->addWidget(interfaceCombo, 1, 1, 1, 2);
    controls->addWidget(refreshButton, 1, 3);
    controls->addWidget(monitorCheckbox, 1, 4);
    controls->addWidget(saveCheckbox, 1, 5);
    controls->addWidget(new QLabel(text("Display filter", "Фільтр показу"), this), 2, 0);
    controls->addWidget(filterEdit, 2, 1, 1, 3);
    controls->addWidget(new QLabel(text("Wireshark profile", "Профіль Wireshark"), this), 2, 4);
    controls->addWidget(profileEdit, 2, 5);
    controls->setColumnStretch(1, 1);
    controls->setColumnStretch(2, 1);
    layout->addLayout(controls);
    auto *commands = new QHBoxLayout;
    openButton = new QPushButton(text("Open PCAP...", "Відкрити PCAP..."), this);
    captureButton = new QPushButton(text("Capture", "Захоплення"), this);
    stopButton = new QPushButton(text("Stop", "Стоп"), this);
    auto *wireshark = new QPushButton(QStringLiteral("Wireshark"), this);
    auto *exportButton = new QPushButton(text("Export CSV", "Експорт CSV"), this);
    auto *clearButton = new QPushButton(text("Clear", "Очистити"), this);
    for (auto *button : {openButton, captureButton, stopButton, wireshark, exportButton, clearButton})
        commands->addWidget(button);
    commands->addStretch();
    layout->addLayout(commands);
    stats = new QLabel(this); stats->setWordWrap(true); layout->addWidget(stats);
    auto *tabs = new QTabWidget(this);
    auto *packets = new QTableView(this);
    packets->setModel(model);
    packets->setSelectionBehavior(QAbstractItemView::SelectRows);
    packets->setAlternatingRowColors(true);
    packets->setWordWrap(false);
    packets->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    packets->horizontalHeader()->setDefaultSectionSize(110);
    tabs->addTab(packets, text("Packets", "Пакети"));
    networks = new QTableWidget(0, 5, this);
    networks->setHorizontalHeaderLabels({QStringLiteral("BSSID"), QStringLiteral("SSID"), QStringLiteral("MHz"),
        text("Packets", "Пакети"), QStringLiteral("RSSI dBm")});
    networks->setEditTriggers(QAbstractItemView::NoEditTriggers);
    networks->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    tabs->addTab(networks, text("Wi-Fi networks", "Мережі Wi-Fi"));
    auto *rf = new QWidget(this);
    auto *rfLayout = new QVBoxLayout(rf);
    bandCombo = new QComboBox(rf);
    bandCombo->addItems({QStringLiteral("2.4 GHz"), QStringLiteral("5 GHz"), QStringLiteral("6 GHz")});
    rfLayout->addWidget(bandCombo);
    rfStatus = new QLabel(rf); rfStatus->setWordWrap(true); rfLayout->addWidget(rfStatus);
    channels = new QTableWidget(0, 4, rf);
    channels->setHorizontalHeaderLabels({text("Channel", "Канал"), QStringLiteral("MHz"),
        text("Coverage %", "Охоплення %"), text("Mean power dBFS", "Середня потужність dBFS")});
    channels->setEditTriggers(QAbstractItemView::NoEditTriggers);
    channels->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    rfLayout->addWidget(channels);
    tabs->addTab(rf, text("SDR channel energy", "Енергія каналів SDR"));
    layout->addWidget(tabs, 1);
    status = new QLabel(text("Idle", "Очікування"), this); status->setWordWrap(true); layout->addWidget(status);
    connect(browse, &QPushButton::clicked, this, [this]() {
        const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("TShark"));
        if (!path.isEmpty()) programEdit->setText(path);
    });
    connect(protocolCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        filterEdit->setText(Wireless::displayFilter(static_cast<Wireless::Protocol>(index)));
    });
    connect(saveCheckbox, &QCheckBox::toggled, this, [this](bool save) {
        filterEdit->setToolTip(save
            ? text("Ignored while saving live capture. Applied when reading a file.",
                   "Ігнорується при збереженні живого захвату. Діє при читанні файлу.")
            : QString());
    });
    connect(refreshButton, &QPushButton::clicked, this, &WirelessLabDialog::refreshInterfaces);
    connect(openButton, &QPushButton::clicked, this, [this]() { startAnalysis(false); });
    connect(captureButton, &QPushButton::clicked, this, [this]() { startAnalysis(true); });
    connect(stopButton, &QPushButton::clicked, this, &WirelessLabDialog::stopCapture);
    connect(clearButton, &QPushButton::clicked, this, &WirelessLabDialog::clearPackets);
    connect(worker, &WirelessCaptureWorker::statusChanged, this, [this](const QString &s) { status->setText(s); });
    connect(worker, &WirelessCaptureWorker::finished, this, [this]() { updateRunning(false); });
    connect(worker, &WirelessCaptureWorker::interfacesReady, this, [this](const QString &list) {
        interfaceCombo->clear();
        const QRegularExpression pattern(QStringLiteral("^([0-9]+)\\. (.+)$"));
        for (const QString &line : list.split(QLatin1Char('\n'))) {
            const auto match = pattern.match(line.trimmed());
            if (match.hasMatch()) interfaceCombo->addItem(match.captured(2), match.captured(1));
        }
        if (!interfaceCombo->count()) status->setText(text("No capture interfaces", "Інтерфейсів захоплення немає"));
    });
    connect(wireshark, &QPushButton::clicked, this, [this]() {
        QString executable = QDir(QFileInfo(programEdit->text()).absolutePath())
            .filePath(QStringLiteral("wireshark") +
#ifdef Q_OS_WIN
                      QStringLiteral(".exe")
#else
                      QString()
#endif
                      );
        if (!QFileInfo::exists(executable)) executable = QStandardPaths::findExecutable(QStringLiteral("wireshark"));
        if (captureFile.isEmpty() || executable.isEmpty() ||
            !QProcess::startDetached(executable, {QStringLiteral("-r"), captureFile}))
            status->setText(text("Select a capture file and install Wireshark", "Оберіть файл захоплення та встановіть Wireshark"));
    });
    connect(exportButton, &QPushButton::clicked, this, [this]() {
        const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("CSV"),
            QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
                .filePath(QStringLiteral("wireless-packets.csv")), QStringLiteral("CSV (*.csv)"));
        if (path.isEmpty()) return;
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly)) { status->setText(file.errorString()); return; }
        QTextStream stream(&file); stream.setCodec("UTF-8");
        auto write = [&stream](QStringList values) {
            for (QString &value : values) {
                if (!value.isEmpty() && QStringLiteral("=+-@").contains(value[0])) value.prepend(QLatin1Char('\''));
                value.replace(QLatin1Char('"'), QStringLiteral("\"\""));
                value = QLatin1Char('"') + value + QLatin1Char('"');
            }
            stream << values.join(QLatin1Char(',')) << '\n';
        };
        write(model->headers);
        for (const auto &row : model->rows) write(row);
        stream.flush();
        if (!file.commit()) status->setText(file.errorString());
    });
    connect(channels, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        if (auto *item = channels->item(row, 1)) emit frequencyRequested(item->text().toDouble() * 1.0e6);
    });
    connect(timer, &QTimer::timeout, this, &WirelessLabDialog::pollPackets);
    timer->start(100);
    updateRunning(false);
    clearPackets();
}
WirelessLabDialog::~WirelessLabDialog() {
    QMetaObject::invokeMethod(worker, [w = worker]() {
        w->stop(); QThread::currentThread()->quit();
    }, Qt::QueuedConnection);
    thread->wait();
}
void WirelessLabDialog::closeEvent(QCloseEvent *event) {
    stopCapture();
    QSettings settings(QStringLiteral("ObriiSDR"), QStringLiteral("WirelessLab"));
    settings.setValue(QStringLiteral("tshark"), programEdit->text());
    settings.setValue(QStringLiteral("profile"), profileEdit->text());
    settings.setValue(QStringLiteral("protocol"), protocolCombo->currentIndex());
    settings.setValue(QStringLiteral("geometry"), saveGeometry());
    QDialog::closeEvent(event);
}
void WirelessLabDialog::updateRunning(bool running) {
    stopButton->setEnabled(running);
    for (auto *button : {openButton, captureButton, refreshButton}) button->setEnabled(!running);
    for (auto *widget : {static_cast<QWidget *>(programEdit), static_cast<QWidget *>(protocolCombo),
                         static_cast<QWidget *>(interfaceCombo), static_cast<QWidget *>(monitorCheckbox),
                         static_cast<QWidget *>(saveCheckbox), static_cast<QWidget *>(filterEdit),
                         static_cast<QWidget *>(profileEdit)}) widget->setEnabled(!running);
}
void WirelessLabDialog::stopCapture() {
    QMetaObject::invokeMethod(worker, [w = worker]() { w->stop(); }, Qt::QueuedConnection);
}
void WirelessLabDialog::refreshInterfaces() {
    if (programEdit->text().trimmed().isEmpty()) {
        status->setText(text("Install Wireshark/TShark and select its executable", "Встановіть Wireshark/TShark та оберіть його виконуваний файл"));
        return;
    }
    updateRunning(true);
    const QString program = programEdit->text().trimmed();
    QMetaObject::invokeMethod(worker, [w = worker, program]() { w->start(program, {QStringLiteral("-D")}, true); },
                              Qt::QueuedConnection);
}
void WirelessLabDialog::startAnalysis(bool live) {
    if (programEdit->text().trimmed().isEmpty()) {
        status->setText(text("Install Wireshark/TShark and select its executable", "Встановіть Wireshark/TShark та оберіть його виконуваний файл"));
        return;
    }
    QStringList arguments = Wireless::tsharkOutputArguments();
    if (!profileEdit->text().trimmed().isEmpty()) arguments << QStringLiteral("-C") << profileEdit->text().trimmed();
    if (live) {
        if (interfaceCombo->currentData().toString().isEmpty()) { refreshInterfaces(); return; }
        QString output;
        if (saveCheckbox->isChecked()) {
            output = QFileDialog::getSaveFileName(this, QStringLiteral("PCAPNG"),
                QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
                    .filePath(QStringLiteral("wireless-capture.pcapng")), QStringLiteral("PCAPNG (*.pcapng)"));
            if (output.isEmpty()) return;
        }
        arguments << QStringLiteral("-i") << interfaceCombo->currentData().toString()
                  << QStringLiteral("-a") << QStringLiteral("duration:300")
                  << QStringLiteral("-a") << QStringLiteral("filesize:102400");
        if (monitorCheckbox->isChecked()) arguments << QStringLiteral("-I");
        if (!output.isEmpty()) {
            captureFile = output;
            // TShark cannot apply a display filter while saving a live capture.
            arguments << QStringLiteral("-w") << output << QStringLiteral("-P");
        } else if (!filterEdit->text().trimmed().isEmpty()) {
            arguments << QStringLiteral("-Y") << filterEdit->text().trimmed();
        }
    } else {
        const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("PCAP / PCAPNG"), QString(),
            QStringLiteral("Capture (*.pcap *.pcapng *.cap *.pcap.gz);;All files (*)"));
        if (path.isEmpty()) return;
        captureFile = path;
        arguments << QStringLiteral("-r") << path;
        if (!filterEdit->text().trimmed().isEmpty()) arguments << QStringLiteral("-Y") << filterEdit->text().trimmed();
    }
    clearPackets();
    updateRunning(true);
    const QString program = programEdit->text().trimmed();
    QMetaObject::invokeMethod(worker, [w = worker, program, arguments]() { w->start(program, arguments); },
                              Qt::QueuedConnection);
}
void WirelessLabDialog::clearPackets() {
    model->clear(); networkStats.clear(); networks->setRowCount(0);
    packetCount = retryCount = protectedCount = queueDrops = 0;
    firstTime = lastTime = 0.0;
    stats->setText(text("No packets", "Пакетів немає"));
}
void WirelessLabDialog::pollPackets() {
    quint64 dropped = 0;
    auto batch = worker->takePackets(dropped);
    queueDrops += dropped;
    if (batch.isEmpty()) return;
    for (const auto &row : batch) {
        ++packetCount;
        if (row[11] == QStringLiteral("1")) ++retryCount;
        if (row[12] == QStringLiteral("1")) ++protectedCount;
        const double time = row[1].toDouble();
        if (std::isfinite(time)) {
            if (firstTime == 0.0) firstTime = time;
            lastTime = time;
        }
        if (!row[6].isEmpty() && (networkStats.contains(row[6]) || networkStats.size() < 1024)) {
            auto &network = networkStats[row[6]];
            if (!row[7].isEmpty()) network.ssid = row[7].left(128);
            if (!row[8].isEmpty()) network.frequency = row[8];
            ++network.packets;
            bool ok = false; const double rssi = row[9].toDouble(&ok);
            if (ok && std::isfinite(rssi)) { network.rssiSum += rssi; ++network.rssiCount; }
        }
    }
    model->append(std::move(batch));
    networks->setRowCount(networkStats.size());
    int index = 0;
    for (auto it = networkStats.cbegin(); it != networkStats.cend(); ++it, ++index) {
        const QStringList values{it.key(), it->ssid, it->frequency, QString::number(it->packets),
            it->rssiCount ? QString::number(it->rssiSum / it->rssiCount, 'f', 1) : QStringLiteral("--")};
        for (int column = 0; column < values.size(); ++column) {
            if (!networks->item(index, column)) networks->setItem(index, column, new QTableWidgetItem);
            networks->item(index, column)->setText(values[column]);
        }
    }
    stats->setText(text("Packets: %1 | Retry: %2 | Protected: %3 | Rate: %4/s | UI drops: %5 | Visible: %6",
                        "Пакети: %1 | Повтори: %2 | Захищені: %3 | Темп: %4/с | Втрати UI: %5 | Видимі: %6")
        .arg(packetCount).arg(retryCount).arg(protectedCount)
        .arg(lastTime > firstTime ? (packetCount - 1) / (lastTime - firstTime) : 0.0, 0, 'f', 1)
        .arg(queueDrops).arg(model->rowCount()));
}
void WirelessLabDialog::appendSpectrum(const std::vector<float> &f, const std::vector<float> &db, double sampleRate) {
    if (!isVisible() || f.size() != db.size() || f.size() < 2) return;
    if (rfTimer.isValid() && rfTimer.elapsed() < 200) return;
    rfTimer.restart();
    if (f.front() >= f.back()) return;
    QVector<int> frequencies;
    const int band = bandCombo->currentIndex();
    if (band == 0) {
        for (int ch = 1; ch <= 13; ++ch) frequencies << 2407 + ch * 5;
        frequencies << 2484;
    } else if (band == 1) {
        for (int ch : {36,40,44,48,52,56,60,64,100,104,108,112,116,120,124,128,132,136,140,144,149,153,157,161,165})
            frequencies << 5000 + ch * 5;
    } else for (int ch = 1; ch <= 233; ch += 4) frequencies << 5950 + ch * 5;
    channels->setRowCount(frequencies.size());
    for (int row = 0; row < frequencies.size(); ++row) {
        const double center = frequencies[row] * 1.0e6;
        const double low = center - 10.0e6, high = center + 10.0e6;
        const double covered = (std::max)(0.0, (std::min)(high, double(f.back())) -
                                              (std::max)(low, double(f.front())));
        const auto begin = std::lower_bound(f.begin(), f.end(), low);
        const auto end = std::upper_bound(f.begin(), f.end(), high);
        const std::size_t first = static_cast<std::size_t>(begin - f.begin());
        const std::size_t last = static_cast<std::size_t>(end - f.begin());
        const std::size_t step = (std::max)(std::size_t(1), (last - first) / 4096);
        double power = 0; int count = 0;
        for (std::size_t i = first; i < last; i += step) {
            if (std::isfinite(db[i])) { power += std::pow(10.0, db[i] / 10.0); ++count; }
        }
        const QStringList values{QString::number(Wireless::wifiChannel(frequencies[row])),
            QString::number(frequencies[row]), QString::number(covered / 200000.0, 'f', 1),
            count && power > 0 ? QString::number(10 * std::log10(power / count), 'f', 1) : QStringLiteral("--")};
        for (int column = 0; column < values.size(); ++column) {
            if (!channels->item(row, column)) channels->setItem(row, column, new QTableWidgetItem);
            channels->item(row, column)->setText(values[column]);
        }
    }
    rfStatus->setText(text("SR %1 Msps | Observed FFT-bin power, not packet decoding or calibrated RSSI.",
                          "SR %1 Мсемпл/с | Потужність бінів FFT, не декодування пакетів і не калібрований RSSI.")
        .arg(sampleRate / 1.0e6, 0, 'f', 3));
}

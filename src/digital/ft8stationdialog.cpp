#include "ft8stationdialog.h"
#include "ft8countrycatalog.h"

#include "appsettingsutils.h"

#include <QBrush>
#include <QColor>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHideEvent>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSaveFile>
#include <QSet>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTabWidget>
#include <QTimer>
#include <QUdpSocket>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <utility>

namespace {
constexpr int InfoRole = Qt::UserRole + 40;

QString defaultFt8LogPath() {
    const QString root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(root).filePath(QStringLiteral("logs/ObriiSDR_FT8_Log.adi"));
}

QString displayReport(QString report) {
    if (report.startsWith(QLatin1Char('R'))) report.remove(0, 1);
    return report;
}

QStringList splitPrefixes(const QString &value) {
    QStringList result;
    QSet<QString> seen;
    const QStringList parts = value.toUpper().split(
        QRegularExpression(QStringLiteral("[\\s,;]+")), Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        if (!seen.contains(part)) { result.push_back(part); seen.insert(part); }
    }
    return result;
}
}

Ft8StationDialog::Ft8StationDialog(const QString &languageValue, QWidget *parent)
    : QDialog(parent), language(languageValue) {
    setAttribute(Qt::WA_DeleteOnClose, false);
    setMinimumSize(900, 620);
    resize(1120, 760);
    buildUi();
    loadSettings();
    loadLog();
    updateTexts();
}

Ft8StationDialog::~Ft8StationDialog() {
    saveSettings();
}

QString Ft8StationDialog::text(const QString &english, const QString &ukrainian) const {
    return language.startsWith(QStringLiteral("uk"), Qt::CaseInsensitive) ? ukrainian : english;
}

void Ft8StationDialog::setLanguage(const QString &value) {
    language = value;
    updateTexts();
}

void Ft8StationDialog::setDialFrequency(double frequencyHz) {
    if (frequencyHz <= 0.0) return;
    currentDialFrequencyHz = frequencyHz;
    dialFrequencySpin->setValue(frequencyHz / 1e6);
}

void Ft8StationDialog::setAutomaticTransmitHandler(AutomaticTransmitHandler handler) {
    automaticTransmitHandler = std::move(handler);
}
void Ft8StationDialog::setAutomaticCancelHandler(AutomaticCancelHandler handler) {
    automaticCancelHandler = std::move(handler);
}

void Ft8StationDialog::buildUi() {
    auto *root = new QVBoxLayout(this);
    tabs = new QTabWidget(this);
    root->addWidget(tabs);

    auto *activity = new QWidget(tabs);
    auto *activityLayout = new QVBoxLayout(activity);
    auto *stationRow = new QHBoxLayout;
    stationCallEdit = new QLineEdit(activity);
    stationCallEdit->setMaxLength(15);
    stationGridEdit = new QLineEdit(activity);
    stationGridEdit->setMaxLength(8);
    dialFrequencySpin = new QDoubleSpinBox(activity);
    dialFrequencySpin->setDecimals(6);
    dialFrequencySpin->setRange(0.001, 7500.0);
    dialFrequencySpin->setSuffix(QStringLiteral(" MHz"));
    txAudioSpin = new QSpinBox(activity);
    txAudioSpin->setRange(200, 4000);
    txAudioSpin->setSuffix(QStringLiteral(" Hz"));
    slotCombo = new QComboBox(activity);
    slotCombo->addItem(QString(), -1);
    slotCombo->addItem(QString(), 0);
    slotCombo->addItem(QString(), 1);
    stationRow->addWidget(new QLabel(text(QStringLiteral("My call"), QString::fromUtf8(u8"Мій позивний")), activity));
    stationRow->addWidget(stationCallEdit);
    stationRow->addWidget(new QLabel(text(QStringLiteral("Grid"), QString::fromUtf8(u8"Локатор")), activity));
    stationRow->addWidget(stationGridEdit);
    stationRow->addWidget(new QLabel(text(QStringLiteral("Dial"), QString::fromUtf8(u8"Частота")), activity));
    stationRow->addWidget(dialFrequencySpin);
    stationRow->addWidget(new QLabel(text(QStringLiteral("TX audio"), QString::fromUtf8(u8"TX аудіо")), activity));
    stationRow->addWidget(txAudioSpin);
    stationRow->addWidget(slotCombo);
    activityLayout->addLayout(stationRow);

    decodeTable = new QTableWidget(0, 8, activity);
    decodeTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    decodeTable->setSelectionMode(QAbstractItemView::SingleSelection);
    decodeTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    decodeTable->setSortingEnabled(true);
    decodeTable->horizontalHeader()->setStretchLastSection(true);
    activityLayout->addWidget(decodeTable, 1);

    auto *qsoBox = new QGroupBox(activity);
    auto *qso = new QGridLayout(qsoBox);
    dxCallEdit = new QLineEdit(qsoBox);
    dxGridEdit = new QLineEdit(qsoBox);
    sentReportEdit = new QLineEdit(QStringLiteral("-10"), qsoBox);
    receivedReportEdit = new QLineEdit(qsoBox);
    commentEdit = new QLineEdit(qsoBox);
    useDecodeButton = new QPushButton(qsoBox);
    logQsoButton = new QPushButton(qsoBox);
    qso->addWidget(new QLabel(text(QStringLiteral("DX call"), QString::fromUtf8(u8"Позивний DX")), qsoBox), 0, 0);
    qso->addWidget(dxCallEdit, 0, 1);
    qso->addWidget(new QLabel(text(QStringLiteral("DX grid"), QString::fromUtf8(u8"Локатор DX")), qsoBox), 0, 2);
    qso->addWidget(dxGridEdit, 0, 3);
    qso->addWidget(new QLabel(text(QStringLiteral("Sent"), QString::fromUtf8(u8"Передано")), qsoBox), 1, 0);
    qso->addWidget(sentReportEdit, 1, 1);
    qso->addWidget(new QLabel(text(QStringLiteral("Received"), QString::fromUtf8(u8"Прийнято")), qsoBox), 1, 2);
    qso->addWidget(receivedReportEdit, 1, 3);
    qso->addWidget(new QLabel(text(QStringLiteral("Comment"), QString::fromUtf8(u8"Коментар")), qsoBox), 2, 0);
    qso->addWidget(commentEdit, 2, 1, 1, 3);
    qso->addWidget(useDecodeButton, 0, 4);
    qso->addWidget(logQsoButton, 1, 4);
    activityLayout->addWidget(qsoBox);

    auto *messages = new QHBoxLayout;
    for (int i = 0; i < 6; ++i) {
        auto *button = new QPushButton(activity);
        messageButtons.push_back(button);
        messages->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, i]() { sendStandardMessage(i); });
    }
    activityLayout->addLayout(messages);

    automaticBox = new QGroupBox(activity);
    auto *automaticLayout = new QGridLayout(automaticBox);
    automaticReplyCheck = new QCheckBox(automaticBox);
    automaticMinSnrSpin = new QSpinBox(automaticBox);
    automaticMinSnrSpin->setRange(-30, 20);
    automaticMinSnrSpin->setValue(-24);
    automaticMinSnrSpin->setSuffix(QStringLiteral(" dB"));
    automaticRetriesSpin = new QSpinBox(automaticBox);
    automaticRetriesSpin->setRange(0, 5);
    automaticRetriesSpin->setValue(2);
    automaticTimeoutSpin = new QSpinBox(automaticBox);
    automaticTimeoutSpin->setRange(35, 120);
    automaticTimeoutSpin->setValue(35);
    automaticTimeoutSpin->setSuffix(QStringLiteral(" s"));
    automaticHourlyLimitSpin = new QSpinBox(automaticBox);
    automaticHourlyLimitSpin->setRange(1, 50);
    automaticHourlyLimitSpin->setValue(6);
    automaticCooldownSpin = new QSpinBox(automaticBox);
    automaticCooldownSpin->setRange(0, 1440);
    automaticCooldownSpin->setValue(30);
    automaticCooldownSpin->setSuffix(QStringLiteral(" min"));
    automaticStopButton = new QPushButton(automaticBox);
    transmitterSetupButton = new QPushButton(automaticBox);
    automaticStatusLabel = new QLabel(automaticBox);
    automaticStatusLabel->setWordWrap(true);
    auto *minimumSnrLabel = new QLabel(automaticBox);
    minimumSnrLabel->setObjectName(QStringLiteral("ft8AutoMinimumSnrLabel"));
    auto *retriesLabel = new QLabel(automaticBox);
    retriesLabel->setObjectName(QStringLiteral("ft8AutoRetriesLabel"));
    auto *timeoutLabel = new QLabel(automaticBox);
    timeoutLabel->setObjectName(QStringLiteral("ft8AutoTimeoutLabel"));
    auto *hourlyLabel = new QLabel(automaticBox);
    hourlyLabel->setObjectName(QStringLiteral("ft8AutoHourlyLabel"));
    auto *cooldownLabel = new QLabel(automaticBox);
    cooldownLabel->setObjectName(QStringLiteral("ft8AutoCooldownLabel"));
    automaticLayout->addWidget(automaticReplyCheck, 0, 0, 1, 4);
    automaticLayout->addWidget(transmitterSetupButton, 0, 4);
    automaticLayout->addWidget(automaticStopButton, 0, 5);
    automaticLayout->addWidget(minimumSnrLabel, 1, 0);
    automaticLayout->addWidget(automaticMinSnrSpin, 1, 1);
    automaticLayout->addWidget(retriesLabel, 1, 2);
    automaticLayout->addWidget(automaticRetriesSpin, 1, 3);
    automaticLayout->addWidget(timeoutLabel, 1, 4);
    automaticLayout->addWidget(automaticTimeoutSpin, 1, 5);
    automaticLayout->addWidget(hourlyLabel, 2, 0);
    automaticLayout->addWidget(automaticHourlyLimitSpin, 2, 1);
    automaticLayout->addWidget(cooldownLabel, 2, 2);
    automaticLayout->addWidget(automaticCooldownSpin, 2, 3);
    automaticLayout->addWidget(automaticStatusLabel, 2, 4, 1, 2);
    activityLayout->addWidget(automaticBox);

    qsoStatusLabel = new QLabel(activity);
    qsoStatusLabel->setWordWrap(true);
    activityLayout->addWidget(qsoStatusLabel);
    tabs->addTab(activity, QString());

    connect(decodeTable, &QTableWidget::cellDoubleClicked, this, [this](int, int) { selectCurrentDecode(); });
    connect(useDecodeButton, &QPushButton::clicked, this, [this]() { selectCurrentDecode(); });
    connect(logQsoButton, &QPushButton::clicked, this, [this]() { logCurrentQso(); });
    connect(dialFrequencySpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            [this](double mhz) { currentDialFrequencyHz = mhz * 1e6; });
    auto *logPage = new QWidget(tabs);
    auto *logLayout = new QVBoxLayout(logPage);
    logPathLabel = new QLabel(logPage);
    logPathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    logLayout->addWidget(logPathLabel);
    logTable = new QTableWidget(0, 9, logPage);
    logTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    logTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    logTable->setSortingEnabled(true);
    logTable->horizontalHeader()->setStretchLastSection(true);
    logLayout->addWidget(logTable, 1);
    auto *logButtons = new QHBoxLayout;
    exportButton = new QPushButton(logPage);
    openFolderButton = new QPushButton(logPage);
    mapExportButton = new QPushButton(logPage);
    logButtons->addWidget(exportButton);
    logButtons->addWidget(openFolderButton);
    logButtons->addWidget(mapExportButton);
    logButtons->addStretch();
    logLayout->addLayout(logButtons);
    tabs->addTab(logPage, QString());
    connect(exportButton, &QPushButton::clicked, this, [this]() { exportAdif(); });
    connect(openFolderButton, &QPushButton::clicked, this, [this]() { openLogFolder(); });
    connect(mapExportButton, &QPushButton::clicked, this, [this]() {
        const auto markers = ft8LogMapMarkers(logRecords);
        if (markers.isEmpty()) {
            QMessageBox::information(this, windowTitle(), text("No logged stations with valid Maidenhead locators.", QString::fromUtf8(u8"У журналі немає абонентів з валідними Maidenhead-локаторами.")));
            return;
        }
        emit mapExportRequested(markers);
    });

    auto *filterPage = new QWidget(tabs);
    auto *filterLayout = new QVBoxLayout(filterPage);
    filterHelpLabel = new QLabel(filterPage);
    filterHelpLabel->setWordWrap(true);
    filterLayout->addWidget(filterHelpLabel);
    blockedCallsLabel = new QLabel(filterPage);
    filterLayout->addWidget(blockedCallsLabel);
    blockedCallsEdit = new QPlainTextEdit(filterPage);
    blockedCallsEdit->setMaximumHeight(110);
    filterLayout->addWidget(blockedCallsEdit);
    countryHelpLabel = new QLabel(filterPage);
    countryHelpLabel->setWordWrap(true);
    filterLayout->addWidget(countryHelpLabel);
    countrySearchEdit = new QLineEdit(filterPage);
    filterLayout->addWidget(countrySearchEdit);
    countryRuleTable = new QTableWidget(0, 3, filterPage);
    countryRuleTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    countryRuleTable->setSelectionMode(QAbstractItemView::SingleSelection);
    countryRuleTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    countryRuleTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    countryRuleTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    filterLayout->addWidget(countryRuleTable, 1);
    auto *countryButtons = new QHBoxLayout;
    countryAddButton = new QPushButton(filterPage);
    countryRemoveButton = new QPushButton(filterPage);
    countryImportButton = new QPushButton(filterPage);
    countryButtons->addWidget(countryAddButton);
    countryButtons->addWidget(countryRemoveButton);
    countryButtons->addWidget(countryImportButton);
    countryButtons->addStretch();
    filterLayout->addLayout(countryButtons);
    tabs->addTab(filterPage, QString());

    auto *reportPage = new QWidget(tabs);
    auto *reportLayout = new QVBoxLayout(reportPage);
    auto *wsjtBox = new QGroupBox(reportPage);
    auto *wsjtForm = new QFormLayout(wsjtBox);
    wsjtEnabledCheck = new QCheckBox(wsjtBox);
    wsjtAddressEdit = new QLineEdit(QStringLiteral("127.0.0.1"), wsjtBox);
    wsjtPortSpin = new QSpinBox(wsjtBox);
    wsjtPortSpin->setRange(1, 65535);
    wsjtPortSpin->setValue(2237);
    wsjtForm->addRow(wsjtEnabledCheck);
    wsjtForm->addRow(text(QStringLiteral("UDP address"), QString::fromUtf8(u8"UDP-адреса")), wsjtAddressEdit);
    wsjtForm->addRow(text(QStringLiteral("UDP port"), QString::fromUtf8(u8"UDP-порт")), wsjtPortSpin);
    reportLayout->addWidget(wsjtBox);

    auto *pskBox = new QGroupBox(reportPage);
    auto *pskForm = new QFormLayout(pskBox);
    pskEnabledCheck = new QCheckBox(pskBox);
    antennaEdit = new QLineEdit(pskBox);
    pskForm->addRow(pskEnabledCheck);
    pskForm->addRow(text(QStringLiteral("Antenna description"), QString::fromUtf8(u8"Опис антени")), antennaEdit);
    reportLayout->addWidget(pskBox);

    auto *qrzBox = new QGroupBox(reportPage);
    auto *qrzForm = new QFormLayout(qrzBox);
    qrzKeyEdit = new QLineEdit(qrzBox);
    qrzKeyEdit->setEchoMode(QLineEdit::Password);
    qrzAutoUploadCheck = new QCheckBox(qrzBox);
    qrzUploadButton = new QPushButton(qrzBox);
    qrzForm->addRow(text(QStringLiteral("QRZ API key"), QString::fromUtf8(u8"Ключ API QRZ")), qrzKeyEdit);
    qrzForm->addRow(qrzAutoUploadCheck);
    qrzForm->addRow(qrzUploadButton);
    reportLayout->addWidget(qrzBox);

    auto *lotwBox = new QGroupBox(reportPage);
    auto *lotwForm = new QFormLayout(lotwBox);
    tqslPathEdit = new QLineEdit(lotwBox);
    tqslLocationEdit = new QLineEdit(lotwBox);
    tqslBrowseButton = new QPushButton(lotwBox);
    tqslUploadButton = new QPushButton(lotwBox);
    auto *pathRow = new QWidget(lotwBox);
    auto *pathLayout = new QHBoxLayout(pathRow);
    pathLayout->setContentsMargins(0, 0, 0, 0);
    pathLayout->addWidget(tqslPathEdit, 1);
    pathLayout->addWidget(tqslBrowseButton);
    lotwForm->addRow(QStringLiteral("TQSL"), pathRow);
    lotwForm->addRow(text(QStringLiteral("Station location"), QString::fromUtf8(u8"Місцезнаходження станції")), tqslLocationEdit);
    lotwForm->addRow(tqslUploadButton);
    reportLayout->addWidget(lotwBox);
    onlineStatusLabel = new QLabel(reportPage);
    onlineStatusLabel->setWordWrap(true);
    reportLayout->addWidget(onlineStatusLabel);
    reportLayout->addStretch();
    tabs->addTab(reportPage, QString());

    connect(qrzUploadButton, &QPushButton::clicked, this, [this]() { uploadSelectedToQrz(); });
    connect(tqslUploadButton, &QPushButton::clicked, this, [this]() { uploadLogToLotw(); });
    connect(tqslBrowseButton, &QPushButton::clicked, this, [this]() {
        const QString path = QFileDialog::getOpenFileName(this, text(QStringLiteral("Select TQSL"), QString::fromUtf8(u8"Виберіть TQSL")), tqslPathEdit->text());
        if (!path.isEmpty()) tqslPathEdit->setText(path);
    });

    wsjtSocket = new QUdpSocket(this);
    pskSocket = new QUdpSocket(this);
    network = new QNetworkAccessManager(this);
    tqslProcess = new QProcess(this);
    wsjtHeartbeatTimer = new QTimer(this);
    wsjtHeartbeatTimer->setInterval(15000);
    pskFlushTimer = new QTimer(this);
    pskFlushTimer->setSingleShot(true);
    automaticTimer = new QTimer(this);
    automaticTimer->setInterval(1000);
    settingsSaveTimer = new QTimer(this);
    settingsSaveTimer->setSingleShot(true);
    settingsSaveTimer->setInterval(500);

    connect(settingsSaveTimer, &QTimer::timeout, this, [this]() { saveSettings(); });
    connect(automaticTimer, &QTimer::timeout, this, [this]() { checkAutomaticTimeout(); });
    connect(automaticReplyCheck, &QCheckBox::toggled, this,
            [this](bool enabled) { setAutomaticEnabled(enabled); });
    connect(automaticStopButton, &QPushButton::clicked, this, [this]() {
        resetAutomaticQso(text(QStringLiteral("Automatic QSO stopped by the operator."),
                               QString::fromUtf8(u8"Автоматичне QSO зупинено оператором.")));
    });
    connect(transmitterSetupButton, &QPushButton::clicked, this,
            [this]() { emit transmitterSetupRequested(); });
    connect(countryAddButton, &QPushButton::clicked, this, [this]() {
        addCountryRuleRow({QString(), {}, false});
        countryRuleTable->editItem(countryRuleTable->currentItem());
        scheduleSettingsSave();
    });
    connect(countryRemoveButton, &QPushButton::clicked, this, [this]() {
        const int row = countryRuleTable->currentRow();
        if (row >= 0) countryRuleTable->removeRow(row);
        countryRulesDirty = true;
        refreshDecodeFilters();
        scheduleSettingsSave();
    });
    connect(blockedCallsEdit, &QPlainTextEdit::textChanged, this, [this]() {
        refreshDecodeFilters();
        QString reason;
        if (automaticStage != AutoQsoStage::Idle &&
            !callsignAllowed(automaticPartnerCall, &reason)) {
            resetAutomaticQso(reason, true);
        }
        scheduleSettingsSave();
    });
    connect(countryRuleTable, &QTableWidget::itemChanged, this,
            [this](QTableWidgetItem *) {
        countryRulesDirty = true;
        refreshDecodeFilters();
        QString reason;
        if (automaticStage != AutoQsoStage::Idle &&
            !callsignAllowed(automaticPartnerCall, &reason)) {
            resetAutomaticQso(reason, true);
        }
        scheduleSettingsSave();
    });
    connect(countrySearchEdit, &QLineEdit::textChanged, this, [this](const QString &query) {
        for (int row = 0; row < countryRuleTable->rowCount(); ++row) {
            const auto *name = countryRuleTable->item(row, 1), *prefix = countryRuleTable->item(row, 2);
            countryRuleTable->setRowHidden(row, !(name && name->text().contains(query, Qt::CaseInsensitive)) &&
                                                !(prefix && prefix->text().contains(query, Qt::CaseInsensitive)));
        }
    });
    connect(countryImportButton, &QPushButton::clicked, this, [this]() {
        const auto path = QFileDialog::getOpenFileName(this, text("Import CTY catalog", QString::fromUtf8(u8"Імпорт каталогу CTY")), QString(), "CTY (*.dat);;All files (*)");
        if (path.isEmpty()) return;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) || file.size() > 4*1024*1024) return;
        const auto catalog = parseFt8CountryCatalog(file.readAll());
        if (catalog.size() < 300) { QMessageBox::warning(this, windowTitle(), text("Incomplete or invalid CTY catalog.", QString::fromUtf8(u8"Неповний або некоректний каталог CTY."))); return; }
        auto saved = countryRules();
        // Keep block choices; replace the prefix data for named catalog entities.
        QVector<Ft8CallsignRule> updated;
        for (auto rule : catalog) {
            for (const auto &old : saved) if (old.country == rule.country) {rule.blocked = old.blocked; break;}
            updated.push_back(rule);
        }
        for (const auto &old : saved) {
            bool present = false; for (const auto &rule : catalog) if (old.country == rule.country) {present = true; break;}
            if (!present) updated.push_back(old);
        }
        const QSignalBlocker blocker(countryRuleTable); countryRuleTable->setSortingEnabled(false);
        countryRuleTable->setRowCount(0); for (const auto &rule : updated) addCountryRuleRow(rule);
        countryRuleTable->setSortingEnabled(true); countrySearchEdit->clear(); countryRulesDirty = true;
        refreshDecodeFilters(); scheduleSettingsSave();
    });
    const auto saveLater = [this]() { scheduleSettingsSave(); };
    connect(stationCallEdit, &QLineEdit::textChanged, this, saveLater);
    connect(stationGridEdit, &QLineEdit::textChanged, this, saveLater);    connect(stationCallEdit, &QLineEdit::textEdited, this, [this](const QString &) {
        if (automaticReplyCheck->isChecked()) automaticReplyCheck->setChecked(false);
    });
    connect(stationGridEdit, &QLineEdit::textEdited, this, [this](const QString &) {
        if (automaticReplyCheck->isChecked()) automaticReplyCheck->setChecked(false);
    });
    connect(dxCallEdit, &QLineEdit::textChanged, this, saveLater);
    connect(dxGridEdit, &QLineEdit::textChanged, this, saveLater);
    connect(sentReportEdit, &QLineEdit::textChanged, this, saveLater);
    connect(receivedReportEdit, &QLineEdit::textChanged, this, saveLater);
    connect(commentEdit, &QLineEdit::textChanged, this, saveLater);
    for (QSpinBox *spin : {automaticMinSnrSpin, automaticRetriesSpin,
                           automaticTimeoutSpin, automaticHourlyLimitSpin,
                           automaticCooldownSpin}) {
        connect(spin, QOverload<int>::of(&QSpinBox::valueChanged), this,
                [this](int) { scheduleSettingsSave(); });
    }

    connect(wsjtHeartbeatTimer, &QTimer::timeout, this, [this]() { sendWsjtHeartbeatAndStatus(); });
    connect(wsjtEnabledCheck, &QCheckBox::toggled, this, [this](bool enabled) {
        if (enabled) {
            sendWsjtHeartbeatAndStatus();
            wsjtHeartbeatTimer->start();
        } else {
            wsjtHeartbeatTimer->stop();
        }
        saveSettings();
    });
    connect(pskEnabledCheck, &QCheckBox::toggled, this, [this](bool enabled) {
        if (enabled) pskSocket->connectToHost(QStringLiteral("report.pskreporter.info"), 4739);
        else {
            pskFlushTimer->stop();
            pendingPskReports.clear();
            pskSocket->disconnectFromHost();
        }
        saveSettings();
    });
    connect(pskFlushTimer, &QTimer::timeout, this, [this]() { flushPskReports(); });
    connect(tqslProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus status) {
        const QString details = QString::fromLocal8Bit(tqslProcess->readAll()).trimmed();
        setOnlineStatus(status == QProcess::NormalExit && code == 0
                            ? text(QStringLiteral("TQSL completed successfully."), QString::fromUtf8(u8"TQSL успішно завершив роботу."))
                            : text(QStringLiteral("TQSL failed: "), QString::fromUtf8(u8"Помилка TQSL: ")) + details,
                        status != QProcess::NormalExit || code != 0);
    });
}

void Ft8StationDialog::updateTexts() {
    setWindowTitle(text(QStringLiteral("FT8 Station"), QString::fromUtf8(u8"Станція FT8")));
    tabs->setTabText(0, text(QStringLiteral("Activity"), QString::fromUtf8(u8"Ефір")));
    tabs->setTabText(1, text(QStringLiteral("Logbook"), QString::fromUtf8(u8"Журнал QSO")));
    tabs->setTabText(2, text(QStringLiteral("Call filters"), QString::fromUtf8(u8"Фільтри позивних")));
    tabs->setTabText(3, text(QStringLiteral("Reporting"), QString::fromUtf8(u8"Обмін даними")));
    decodeTable->setHorizontalHeaderLabels({QStringLiteral("UTC"), QStringLiteral("SNR*"), QStringLiteral("DT"),
        text(QStringLiteral("Audio"), QString::fromUtf8(u8"Аудіо")), QStringLiteral("RF"),
        text(QStringLiteral("Quality"), QString::fromUtf8(u8"Якість")),
        text(QStringLiteral("Country / filter"), QString::fromUtf8(u8"Країна / фільтр")),
        text(QStringLiteral("Message"), QString::fromUtf8(u8"Повідомлення"))});
    logTable->setHorizontalHeaderLabels({QStringLiteral("UTC"), QStringLiteral("Call"), QStringLiteral("Grid"),
        QStringLiteral("MHz"), text(QStringLiteral("Sent"), QString::fromUtf8(u8"Передано")),
        text(QStringLiteral("Received"), QString::fromUtf8(u8"Прийнято")), QStringLiteral("Band"),
        QStringLiteral("Mode"), text(QStringLiteral("Comment"), QString::fromUtf8(u8"Коментар"))});
    countryRuleTable->setHorizontalHeaderLabels({
        text(QStringLiteral("Block"), QString::fromUtf8(u8"Блокувати")),
        text(QStringLiteral("Country / group"), QString::fromUtf8(u8"Країна / група")),
        text(QStringLiteral("Call prefixes"), QString::fromUtf8(u8"Префікси позивних"))});
    slotCombo->setItemText(0, text(QStringLiteral("Next slot"), QString::fromUtf8(u8"Наступний слот")));
    slotCombo->setItemText(1, text(QStringLiteral("Even slot"), QString::fromUtf8(u8"Парний слот")));
    slotCombo->setItemText(2, text(QStringLiteral("Odd slot"), QString::fromUtf8(u8"Непарний слот")));
    useDecodeButton->setText(text(QStringLiteral("Use selected"), QString::fromUtf8(u8"Використати вибране")));
    logQsoButton->setText(text(QStringLiteral("Log QSO"), QString::fromUtf8(u8"Записати QSO")));
    exportButton->setText(text(QStringLiteral("Export ADIF..."), QString::fromUtf8(u8"Експорт ADIF...")));
    openFolderButton->setText(text(QStringLiteral("Open log folder"), QString::fromUtf8(u8"Відкрити папку журналу")));
    mapExportButton->setText(text("Stations to map", QString::fromUtf8(u8"Абоненти на мапу")));
    mapExportButton->setToolTip(text("Export logged stations to QTH markers. Locations are Maidenhead cell centers, not precise station coordinates.", QString::fromUtf8(u8"Перенести абонентів журналу в QTH-маркери. Положення — центри комірок Maidenhead, не точні координати станцій.")));
    const QStringList names = {QStringLiteral("CQ"), QStringLiteral("Tx1 Grid"), QStringLiteral("Tx2 Report"),
                               QStringLiteral("Tx3 R+report"), QStringLiteral("Tx4 RR73"), QStringLiteral("Tx5 73")};
    for (int i = 0; i < messageButtons.size(); ++i) messageButtons[i]->setText(names.value(i));

    automaticBox->setTitle(text(QStringLiteral("Automatic CQ reply"),
                                QString::fromUtf8(u8"Автоматична відповідь на CQ")));
    automaticReplyCheck->setText(text(
        QStringLiteral("ARM automatic replies and complete the FT8 QSO"),
        QString::fromUtf8(u8"ОЗБРОЇТИ автоматичні відповіді та завершувати FT8 QSO")));
    automaticStopButton->setText(text(QStringLiteral("Stop current QSO"),
                                      QString::fromUtf8(u8"Зупинити поточне QSO")));
    transmitterSetupButton->setText(text(QStringLiteral("TX setup..."),
                                        QString::fromUtf8(u8"Налаштування TX...")));
    if (auto *label = findChild<QLabel *>(QStringLiteral("ft8AutoMinimumSnrLabel")))
        label->setText(text(QStringLiteral("Minimum SNR*"), QString::fromUtf8(u8"Мінімум SNR*")));
    if (auto *label = findChild<QLabel *>(QStringLiteral("ft8AutoRetriesLabel")))
        label->setText(text(QStringLiteral("Retries"), QString::fromUtf8(u8"Повтори")));
    if (auto *label = findChild<QLabel *>(QStringLiteral("ft8AutoTimeoutLabel")))
        label->setText(text(QStringLiteral("Reply timeout"), QString::fromUtf8(u8"Очікування відповіді")));
    if (auto *label = findChild<QLabel *>(QStringLiteral("ft8AutoHourlyLabel")))
        label->setText(text(QStringLiteral("New QSOs/hour"), QString::fromUtf8(u8"Нових QSO/год")));
    if (auto *label = findChild<QLabel *>(QStringLiteral("ft8AutoCooldownLabel")))
        label->setText(text(QStringLiteral("Call cooldown"), QString::fromUtf8(u8"Пауза для позивного")));

    filterHelpLabel->setText(text(
        QStringLiteral("These local rules prevent manual and automatic FT8 replies. No online lookup is used. Country recognition is an editable prefix approximation; no country is blocked by default."),
        QString::fromUtf8(u8"Ці локальні правила забороняють ручні й автоматичні FT8-відповіді. Онлайн-пошук не використовується. Країна визначається приблизно за редагованими префіксами; типово жодну країну не заблоковано.")));
    blockedCallsLabel->setText(text(
        QStringLiteral("Blocked callsigns or masks (one per line; * and ? are supported):"),
        QString::fromUtf8(u8"Заблоковані позивні або маски (по одному на рядок; підтримуються * та ?):")));
    blockedCallsEdit->setPlaceholderText(QStringLiteral("RA3ABC\nR9*\n*/MM"));
    countryHelpLabel->setText(text(
        QStringLiteral("Check Block for a country/group. Prefixes are separated by spaces, commas or semicolons; rows can be edited directly."),
        QString::fromUtf8(u8"Позначте Блокувати для країни/групи. Префікси розділяються пробілами, комами або крапками з комою; рядки редагуються безпосередньо.")));
    countryAddButton->setText(text(QStringLiteral("Add rule"), QString::fromUtf8(u8"Додати правило")));
    countryRemoveButton->setText(text(QStringLiteral("Remove selected"), QString::fromUtf8(u8"Видалити вибране")));
    countryImportButton->setText(text("Import CTY...", QString::fromUtf8(u8"Імпорт CTY...")));
    countrySearchEdit->setPlaceholderText(text("Find country or prefix", QString::fromUtf8(u8"Пошук країни чи префікса")));

    wsjtEnabledCheck->setText(text(QStringLiteral("Send WSJT-X-compatible UDP events (local loggers)"), QString::fromUtf8(u8"Надсилати сумісні з WSJT-X UDP-події (локальні логери)")));
    pskEnabledCheck->setText(text(QStringLiteral("Upload reception reports to PSK Reporter"), QString::fromUtf8(u8"Надсилати звіти прийому до PSK Reporter")));
    qrzAutoUploadCheck->setText(text(QStringLiteral("Upload newly logged QSOs automatically"), QString::fromUtf8(u8"Автоматично надсилати нові QSO")));
    qrzUploadButton->setText(text(QStringLiteral("Upload selected QSO to QRZ"), QString::fromUtf8(u8"Надіслати вибране QSO до QRZ")));
    tqslBrowseButton->setText(text(QStringLiteral("Browse..."), QString::fromUtf8(u8"Огляд...")));
    tqslUploadButton->setText(text(QStringLiteral("Sign and upload log with TQSL"), QString::fromUtf8(u8"Підписати й надіслати журнал через TQSL")));
    logPathLabel->setText(text(QStringLiteral("Local ADIF log: "), QString::fromUtf8(u8"Локальний журнал ADIF: ")) + defaultFt8LogPath());
    qsoStatusLabel->setText(text(QStringLiteral("SNR* is an estimate derived from decoder sync quality, not a calibrated instrument reading."), QString::fromUtf8(u8"SNR* — оцінка з якості синхронізації декодера, а не каліброване вимірювання.")));
    updateAutomaticStatus();
}
void Ft8StationDialog::loadSettings() {
    QSettings s(persistentSettingsFilePath(), QSettings::IniFormat);
    stationCallEdit->setText(s.value(QStringLiteral("ft8/stationCall")).toString());
    stationGridEdit->setText(s.value(QStringLiteral("ft8/stationGrid")).toString());
    dialFrequencySpin->setValue(s.value(QStringLiteral("ft8/dialMHz"), 14.074).toDouble());
    txAudioSpin->setValue(s.value(QStringLiteral("ft8/txAudioHz"), 1500).toInt());
    slotCombo->setCurrentIndex((std::max)(0, slotCombo->findData(
        s.value(QStringLiteral("ft8/slotParity"), -1).toInt())));
    automaticMinSnrSpin->setValue(s.value(QStringLiteral("ft8/automaticMinimumSnr"), -24).toInt());
    automaticRetriesSpin->setValue(s.value(QStringLiteral("ft8/automaticRetries"), 2).toInt());
    automaticTimeoutSpin->setValue(s.value(QStringLiteral("ft8/automaticTimeoutSeconds"), 35).toInt());
    automaticHourlyLimitSpin->setValue(s.value(QStringLiteral("ft8/automaticQsoHourlyLimit"), 6).toInt());
    automaticCooldownSpin->setValue(s.value(QStringLiteral("ft8/automaticCallCooldownMinutes"), 30).toInt());
    blockedCallsEdit->setPlainText(s.value(QStringLiteral("ft8/blockedCallsigns")).toString());

    QVector<Ft8CallsignRule> rules;
    const QJsonDocument rulesDocument = QJsonDocument::fromJson(
        s.value(QStringLiteral("ft8/countryRulesJson")).toByteArray());
    if (rulesDocument.isArray()) {
        for (const QJsonValue &value : rulesDocument.array()) {
            const QJsonObject object = value.toObject();
            Ft8CallsignRule rule;
            rule.country = object.value(QStringLiteral("country")).toString();
            rule.blocked = object.value(QStringLiteral("blocked")).toBool(false);
            const QJsonValue prefixes = object.value(QStringLiteral("prefixes"));
            if (prefixes.isArray()) {
                for (const QJsonValue &prefix : prefixes.toArray()) {
                    const QString value = prefix.toString().trimmed().toUpper();
                    if (!value.isEmpty()) rule.prefixes.push_back(value);
                }
            } else {
                rule.prefixes = splitPrefixes(prefixes.toString());
            }
            if (!rule.country.trimmed().isEmpty() || !rule.prefixes.isEmpty()) rules.push_back(rule);
        }
    }
    if (s.value("ft8/countryCatalogVersion", 0).toInt() < 1 || rules.isEmpty())
        rules = mergeFt8CountryCatalog(rules, defaultFt8CountryCatalog());
    {
        const QSignalBlocker blocker(countryRuleTable);
        countryRuleTable->setRowCount(0);
        for (const Ft8CallsignRule &rule : rules) addCountryRuleRow(rule);
        countryRuleTable->setSortingEnabled(true);
    }
    {
        const QSignalBlocker blocker(automaticReplyCheck);
        automaticReplyCheck->setChecked(false);
    }

    wsjtAddressEdit->setText(s.value(QStringLiteral("ft8/wsjtAddress"), QStringLiteral("127.0.0.1")).toString());
    wsjtPortSpin->setValue(s.value(QStringLiteral("ft8/wsjtPort"), 2237).toInt());
    antennaEdit->setText(s.value(QStringLiteral("ft8/antenna")).toString());
    qrzKeyEdit->setText(s.value(QStringLiteral("ft8/qrzApiKey")).toString());
    tqslPathEdit->setText(s.value(QStringLiteral("ft8/tqslPath")).toString());
    tqslLocationEdit->setText(s.value(QStringLiteral("ft8/tqslLocation")).toString());
    qrzAutoUploadCheck->setChecked(s.value(QStringLiteral("ft8/qrzAutoUpload"), false).toBool());
    wsjtEnabledCheck->setChecked(s.value(QStringLiteral("ft8/wsjtEnabled"), false).toBool());
    pskEnabledCheck->setChecked(s.value(QStringLiteral("ft8/pskEnabled"), false).toBool());
    currentDialFrequencyHz = dialFrequencySpin->value() * 1e6;
    refreshDecodeFilters();
}

void Ft8StationDialog::saveSettings() const {
    QSettings s(persistentSettingsFilePath(), QSettings::IniFormat);
    s.setValue("ft8/countryCatalogVersion", 1);
    s.setValue(QStringLiteral("ft8/stationCall"), stationCallEdit->text().trimmed().toUpper());
    s.setValue(QStringLiteral("ft8/stationGrid"), stationGridEdit->text().trimmed().toUpper());
    s.setValue(QStringLiteral("ft8/dialMHz"), dialFrequencySpin->value());
    s.setValue(QStringLiteral("ft8/txAudioHz"), txAudioSpin->value());
    s.setValue(QStringLiteral("ft8/slotParity"), slotCombo->currentData());
    s.setValue(QStringLiteral("ft8/automaticMinimumSnr"), automaticMinSnrSpin->value());
    s.setValue(QStringLiteral("ft8/automaticRetries"), automaticRetriesSpin->value());
    s.setValue(QStringLiteral("ft8/automaticTimeoutSeconds"), automaticTimeoutSpin->value());
    s.setValue(QStringLiteral("ft8/automaticQsoHourlyLimit"), automaticHourlyLimitSpin->value());
    s.setValue(QStringLiteral("ft8/automaticCallCooldownMinutes"), automaticCooldownSpin->value());
    s.setValue(QStringLiteral("ft8/blockedCallsigns"), blockedCallsEdit->toPlainText());
    QJsonArray rulesJson;
    for (const Ft8CallsignRule &rule : countryRules()) {
        QJsonObject object;
        object.insert(QStringLiteral("country"), rule.country);
        object.insert(QStringLiteral("blocked"), rule.blocked);
        object.insert(QStringLiteral("prefixes"), QJsonArray::fromStringList(rule.prefixes));
        rulesJson.push_back(object);
    }
    s.setValue(QStringLiteral("ft8/countryRulesJson"),
               QJsonDocument(rulesJson).toJson(QJsonDocument::Compact));
    s.setValue(QStringLiteral("ft8/wsjtEnabled"), wsjtEnabledCheck->isChecked());
    s.setValue(QStringLiteral("ft8/wsjtAddress"), wsjtAddressEdit->text().trimmed());
    s.setValue(QStringLiteral("ft8/wsjtPort"), wsjtPortSpin->value());
    s.setValue(QStringLiteral("ft8/pskEnabled"), pskEnabledCheck->isChecked());
    s.setValue(QStringLiteral("ft8/antenna"), antennaEdit->text().trimmed());
    s.setValue(QStringLiteral("ft8/qrzApiKey"), qrzKeyEdit->text().trimmed());
    s.setValue(QStringLiteral("ft8/qrzAutoUpload"), qrzAutoUploadCheck->isChecked());
    s.setValue(QStringLiteral("ft8/tqslPath"), tqslPathEdit->text().trimmed());
    s.setValue(QStringLiteral("ft8/tqslLocation"), tqslLocationEdit->text().trimmed());
}

void Ft8StationDialog::scheduleSettingsSave() {
    if (settingsSaveTimer) settingsSaveTimer->start();
}
void Ft8StationDialog::loadLog() {
    QFile file(defaultFt8LogPath());
    if (file.open(QIODevice::ReadOnly)) logRecords = parseFt8Adif(file.readAll());
    refreshLogTable();
}

void Ft8StationDialog::refreshLogTable() {
    logTable->setSortingEnabled(false);
    logTable->setRowCount(logRecords.size());
    for (int row = 0; row < logRecords.size(); ++row) {
        const Ft8QsoRecord &r = logRecords[row];
        const QStringList values = {r.timeOnUtc.toUTC().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")), r.call,
            r.grid, QString::number(r.frequencyHz / 1e6, 'f', 6), r.reportSent, r.reportReceived,
            ft8BandForFrequency(r.frequencyHz), QStringLiteral("FT8"), r.comment};
        for (int column = 0; column < values.size(); ++column) {
            auto *item = new QTableWidgetItem(values[column]);
            item->setData(Qt::UserRole, row);
            logTable->setItem(row, column, item);
        }
    }
    logTable->setSortingEnabled(true);
}

void Ft8StationDialog::addDecode(const QDateTime &utc, double rfFrequencyHz,
                                 double audioFrequencyHz, double dtSeconds,
                                 int snrDbEstimate, double qualityDb, int costasHits,
                                 const QString &message) {
    const Ft8MessageInfo info = parseFt8Message(message);
    const Ft8CallsignFilterResult filter = filterCallsign(info.senderCall);
    QString countryText = filter.country;
    if (filter.blocked) {
        countryText += countryText.isEmpty() ? text(QStringLiteral("Blocked"), QString::fromUtf8(u8"Заблоковано"))
                                             : text(QStringLiteral(" (blocked)"), QString::fromUtf8(u8" (заблоковано)"));
    }

    decodeTable->setSortingEnabled(false);
    decodeTable->insertRow(0);
    const QStringList values = {utc.toUTC().toString(QStringLiteral("HH:mm:ss")),
        QString::number(snrDbEstimate), QString::number(dtSeconds, 'f', 2),
        QString::number(audioFrequencyHz, 'f', 0), QString::number(rfFrequencyHz / 1e6, 'f', 6),
        QStringLiteral("%1 dB / %2").arg(qualityDb, 0, 'f', 1).arg(costasHits),
        countryText, message};
    for (int column = 0; column < values.size(); ++column) {
        auto *item = new QTableWidgetItem(values[column]);
        item->setData(InfoRole, QVariant::fromValue(message));
        item->setData(InfoRole + 1, utc);
        item->setData(InfoRole + 2, rfFrequencyHz);
        item->setData(InfoRole + 3, audioFrequencyHz);
        item->setData(InfoRole + 4, snrDbEstimate);
        decodeTable->setItem(0, column, item);
    }
    applyDecodeFilterAppearance(0, info, filter);
    while (decodeTable->rowCount() > 500) decodeTable->removeRow(decodeTable->rowCount() - 1);
    decodeTable->setSortingEnabled(true);
    if (rfFrequencyHz > 0.0 && audioFrequencyHz > 0.0) setDialFrequency(rfFrequencyHz - audioFrequencyHz);
    if (wsjtEnabledCheck->isChecked()) {
        sendWsjtDatagram(buildWsjtDecode(QStringLiteral("ObriiSDR"), utc, snrDbEstimate,
                                         dtSeconds, static_cast<quint32>(std::llround(audioFrequencyHz)), message));
    }
    if (pskEnabledCheck->isChecked() && info.valid) queuePskReport(info, utc, rfFrequencyHz);
    if (automaticReplyCheck->isChecked() && info.valid) {
        handleAutomaticDecode(info, utc, rfFrequencyHz, audioFrequencyHz, snrDbEstimate);
    }
}
void Ft8StationDialog::selectCurrentDecode() {
    const int row = decodeTable->currentRow();
    if (row < 0 || !decodeTable->item(row, 0)) return;
    QTableWidgetItem *item = decodeTable->item(row, 0);
    const Ft8MessageInfo info = parseFt8Message(item->data(InfoRole).toString());
    if (!info.valid) return;
    setQsoPartner(info, item->data(InfoRole + 1).toDateTime(),
                  item->data(InfoRole + 2).toDouble(), item->data(InfoRole + 3).toDouble());
}

void Ft8StationDialog::setQsoPartner(const Ft8MessageInfo &info, const QDateTime &utc,
                                     double rfFrequencyHz, double audioFrequencyHz) {
    const QString myCall = stationCallEdit->text().trimmed().toUpper();
    const bool decodedOwnTransmission = !myCall.isEmpty() && info.senderCall == myCall &&
                                        info.recipientCall != QStringLiteral("CQ") &&
                                        info.recipientCall != QStringLiteral("QRZ");
    const QString partnerCall = decodedOwnTransmission ? info.recipientCall : info.senderCall;
    dxCallEdit->setText(partnerCall);
    if (!decodedOwnTransmission && !info.grid.isEmpty()) dxGridEdit->setText(info.grid);
    if (!decodedOwnTransmission && !info.report.isEmpty()) receivedReportEdit->setText(displayReport(info.report));
    if (utc.isValid()) qsoStartUtc = utc.toUTC();
    if (rfFrequencyHz > 0.0) setDialFrequency(rfFrequencyHz - audioFrequencyHz);
    if (audioFrequencyHz > 0.0) txAudioSpin->setValue(static_cast<int>(std::llround(audioFrequencyHz)));
    qsoStatusLabel->setText(text(QStringLiteral("Selected %1 for the QSO sequence."), QString::fromUtf8(u8"Для послідовності QSO вибрано %1.")).arg(partnerCall));
}

QString Ft8StationDialog::standardMessage(int index) const {
    const QString me = stationCallEdit->text().trimmed().toUpper();
    const QString grid = stationGridEdit->text().trimmed().toUpper();
    const QString dx = dxCallEdit->text().trimmed().toUpper();
    const QString report = displayReport(sentReportEdit->text().trimmed().toUpper());
    if (index == 0) return me.isEmpty() ? QString() : QStringLiteral("CQ %1 %2").arg(me, grid).trimmed();
    if (dx.isEmpty() || me.isEmpty()) return QString();
    switch (index) {
    case 1: return QStringLiteral("%1 %2 %3").arg(dx, me, grid).trimmed();
    case 2: return QStringLiteral("%1 %2 %3").arg(dx, me, report);
    case 3: return QStringLiteral("%1 %2 R%3").arg(dx, me, report);
    case 4: return QStringLiteral("%1 %2 RR73").arg(dx, me);
    case 5: return QStringLiteral("%1 %2 73").arg(dx, me);
    default: return QString();
    }
}

void Ft8StationDialog::sendStandardMessage(int index) {
    const QString message = standardMessage(index).simplified();
    if (message.isEmpty()) {
        QMessageBox::warning(this, windowTitle(), text(QStringLiteral("Enter your call sign and select a DX station first."), QString::fromUtf8(u8"Спочатку введіть свій позивний і виберіть станцію DX.")));
        return;
    }
    if (index > 0) {
        QString reason;
        if (!callsignAllowed(dxCallEdit->text(), &reason)) {
            QMessageBox::warning(this, windowTitle(), reason);
            return;
        }
    }
    saveSettings();
    emit transmitMessageRequested(message, dialFrequencySpin->value() * 1e6,
                                  txAudioSpin->value(), slotCombo->currentData().toInt());
}

Ft8QsoRecord Ft8StationDialog::currentQsoRecord() const {
    Ft8QsoRecord record;
    record.timeOnUtc = qsoStartUtc.isValid() ? qsoStartUtc : QDateTime::currentDateTimeUtc();
    record.timeOffUtc = QDateTime::currentDateTimeUtc();
    record.stationCall = stationCallEdit->text().trimmed().toUpper();
    record.stationGrid = stationGridEdit->text().trimmed().toUpper();
    record.call = dxCallEdit->text().trimmed().toUpper();
    record.grid = dxGridEdit->text().trimmed().toUpper();
    record.reportSent = sentReportEdit->text().trimmed();
    record.reportReceived = receivedReportEdit->text().trimmed();
    record.comment = commentEdit->text().trimmed();
    record.frequencyHz = dialFrequencySpin->value() * 1e6;
    return record;
}
bool Ft8StationDialog::appendQso(const Ft8QsoRecord &record, QString *error) {
    const QString path = defaultFt8LogPath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    const bool newFile = !QFileInfo::exists(path) || QFileInfo(path).size() == 0;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        if (error) *error = file.errorString();
        return false;
    }
    QByteArray data;
    if (newFile) data += QByteArray("<ADIF_VER:5>3.1.7\n<PROGRAMID:8>ObriiSDR\n<PROGRAMVERSION:5>5.1.0\n<EOH>\n");
    data += ft8AdifRecord(record).toUtf8() + '\n';
    if (file.write(data) != data.size()) {
        if (error) *error = file.errorString();
        return false;
    }
    return true;
}

void Ft8StationDialog::logCurrentQso() {
    const Ft8QsoRecord record = currentQsoRecord();
    if (record.stationCall.isEmpty() || record.call.isEmpty()) {
        QMessageBox::warning(this, windowTitle(), text(QStringLiteral("My call and DX call are required."), QString::fromUtf8(u8"Потрібні власний позивний і позивний DX.")));
        return;
    }
    QString error;
    if (!appendQso(record, &error)) {
        QMessageBox::critical(this, windowTitle(), error);
        return;
    }
    logRecords.push_back(record);
    refreshLogTable();
    if (wsjtEnabledCheck->isChecked()) {
        sendWsjtDatagram(buildWsjtQsoLogged(QStringLiteral("ObriiSDR"), record));
        sendWsjtDatagram(buildWsjtLoggedAdif(QStringLiteral("ObriiSDR"), ft8AdifRecord(record)));
    }
    if (qrzAutoUploadCheck->isChecked()) uploadRecordToQrz(record);
    saveSettings();
    qsoStatusLabel->setText(text(QStringLiteral("QSO saved locally."), QString::fromUtf8(u8"QSO збережено локально.")));
}

void Ft8StationDialog::exportAdif() {
    const QString path = QFileDialog::getSaveFileName(this,
        text(QStringLiteral("Export FT8 log"), QString::fromUtf8(u8"Експорт журналу FT8")),
        QDir::home().filePath(QStringLiteral("ObriiSDR_FT8_Log.adi")), QStringLiteral("ADIF (*.adi *.adif)"));
    if (path.isEmpty()) return;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::critical(this, windowTitle(), file.errorString());
        return;
    }
    const QByteArray data = ft8AdifDocument(logRecords).toUtf8();
    if (file.write(data) != data.size() || !file.commit()) {
        QMessageBox::critical(this, windowTitle(), file.errorString());
    }
}

void Ft8StationDialog::openLogFolder() {
    const QString path = defaultFt8LogPath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
}

void Ft8StationDialog::addCountryRuleRow(const Ft8CallsignRule &rule) {
    countryRulesDirty = true;
    const QSignalBlocker guard(countryRuleTable);
    const bool sorting = countryRuleTable->isSortingEnabled();
    countryRuleTable->setSortingEnabled(false);
    const int row = countryRuleTable->rowCount();
    countryRuleTable->insertRow(row);
    auto *blocked = new QTableWidgetItem;
    blocked->setFlags((blocked->flags() | Qt::ItemIsUserCheckable) & ~Qt::ItemIsEditable);
    blocked->setCheckState(rule.blocked ? Qt::Checked : Qt::Unchecked);
    countryRuleTable->setItem(row, 0, blocked);
    countryRuleTable->setItem(row, 1, new QTableWidgetItem(rule.country));
    countryRuleTable->setItem(row, 2, new QTableWidgetItem(rule.prefixes.join(QStringLiteral("; "))));
    auto *name = countryRuleTable->item(row, 1);
    countryRuleTable->item(row, 2)->setToolTip(rule.prefixes.join(QStringLiteral("; ")));
    countryRuleTable->setSortingEnabled(sorting);
    countryRuleTable->setCurrentItem(name);
}

QVector<Ft8CallsignRule> Ft8StationDialog::countryRules() const {
    if (!countryRulesDirty) return cachedCountryRules;
    QVector<Ft8CallsignRule> rules;
    for (int row = 0; row < countryRuleTable->rowCount(); ++row) {
        Ft8CallsignRule rule;
        const QTableWidgetItem *blocked = countryRuleTable->item(row, 0);
        const QTableWidgetItem *country = countryRuleTable->item(row, 1);
        const QTableWidgetItem *prefixes = countryRuleTable->item(row, 2);
        rule.blocked = blocked && blocked->checkState() == Qt::Checked;
        rule.country = country ? country->text().trimmed() : QString();
        rule.prefixes = prefixes ? splitPrefixes(prefixes->text()) : QStringList();
        if (!rule.country.isEmpty() || !rule.prefixes.isEmpty()) rules.push_back(rule);
    }
    cachedCountryRules = rules; countryRulesDirty = false;
    return cachedCountryRules;
}

Ft8CallsignFilterResult Ft8StationDialog::filterCallsign(const QString &callsign) const {
    return evaluateFt8Callsign(callsign,
                               parseFt8CallsignPatterns(blockedCallsEdit->toPlainText()),
                               countryRules());
}

bool Ft8StationDialog::callsignAllowed(const QString &callsign, QString *reason) const {
    const QString normalized = normalizeFt8Callsign(callsign);
    const Ft8CallsignFilterResult filter = filterCallsign(normalized);
    if (!filter.blocked) return true;
    if (reason) {
        if (filter.callsignBlocked) {
            *reason = text(
                QStringLiteral("Transmission to %1 is blocked by callsign rule %2."),
                QString::fromUtf8(u8"Передавання для %1 заборонене правилом позивного %2."))
                          .arg(normalized, filter.matchedPattern);
        } else {
            *reason = text(
                QStringLiteral("Transmission to %1 is blocked by the country/group rule %2 (%3)."),
                QString::fromUtf8(u8"Передавання для %1 заборонене правилом країни/групи %2 (%3)."))
                          .arg(normalized, filter.country, filter.matchedPrefix);
        }
    }
    return false;
}

void Ft8StationDialog::refreshDecodeFilters() {
    if (!decodeTable) return;
    decodeTable->setSortingEnabled(false);
    for (int row = 0; row < decodeTable->rowCount(); ++row) {
        const QTableWidgetItem *source = decodeTable->item(row, 0);
        if (!source) continue;
        const Ft8MessageInfo info = parseFt8Message(source->data(InfoRole).toString());
        const Ft8CallsignFilterResult filter = filterCallsign(info.senderCall);
        QString countryText = filter.country;
        if (filter.blocked) {
            countryText += countryText.isEmpty()
                               ? text(QStringLiteral("Blocked"), QString::fromUtf8(u8"Заблоковано"))
                               : text(QStringLiteral(" (blocked)"), QString::fromUtf8(u8" (заблоковано)"));
        }
        if (QTableWidgetItem *country = decodeTable->item(row, 6)) country->setText(countryText);
        applyDecodeFilterAppearance(row, info, filter);
    }
    decodeTable->setSortingEnabled(true);
}

void Ft8StationDialog::applyDecodeFilterAppearance(
    int row, const Ft8MessageInfo &info, const Ft8CallsignFilterResult &filter) {
    QString tooltip;
    if (filter.callsignBlocked) {
        tooltip = text(QStringLiteral("Blocked callsign rule: %1"),
                       QString::fromUtf8(u8"Правило заблокованого позивного: %1"))
                      .arg(filter.matchedPattern);
    } else if (filter.countryBlocked) {
        tooltip = text(QStringLiteral("Blocked country/group: %1; prefix %2"),
                       QString::fromUtf8(u8"Заблокована країна/група: %1; префікс %2"))
                      .arg(filter.country, filter.matchedPrefix);
    } else if (!filter.country.isEmpty()) {
        tooltip = text(QStringLiteral("Prefix match: %1 (%2)"),
                       QString::fromUtf8(u8"Збіг префікса: %1 (%2)"))
                      .arg(filter.country, filter.matchedPrefix);
    }
    if (!info.senderCall.isEmpty()) {
        tooltip = info.senderCall + (tooltip.isEmpty() ? QString() : QStringLiteral("\n") + tooltip);
    }
    for (int column = 0; column < decodeTable->columnCount(); ++column) {
        if (QTableWidgetItem *item = decodeTable->item(row, column)) {
            item->setToolTip(tooltip);
            item->setForeground(filter.blocked ? QBrush(QColor(QStringLiteral("#e85d5d")))
                                               : QBrush());
            item->setBackground(filter.blocked ? QBrush(QColor(120, 24, 24, 55))
                                               : QBrush());
        }
    }
}

void Ft8StationDialog::setAutomaticEnabled(bool enabled) {
    if (!enabled) {
        if (automaticCancelHandler) automaticCancelHandler();
        if (automaticTimer) automaticTimer->stop();
        resetAutomaticQso(text(QStringLiteral("Automatic FT8 replies are disarmed."),
                               QString::fromUtf8(u8"Автоматичні FT8-відповіді роззброєно.")));
        return;
    }

    const QString myCall = normalizeFt8Callsign(stationCallEdit->text());
    const QString myGrid = stationGridEdit->text().trimmed().toUpper();
    QString error;
    if (myCall.isEmpty() || myGrid.isEmpty()) {
        error = text(QStringLiteral("Enter your call sign and grid before arming automatic replies."),
                     QString::fromUtf8(u8"Перед озброєнням автоматичних відповідей введіть свій позивний і локатор."));
    } else if (!automaticTransmitHandler) {
        error = text(QStringLiteral("The transmitter path is not available."),
                     QString::fromUtf8(u8"Шлях передавача недоступний."));
    }
    if (!error.isEmpty()) {
        const QSignalBlocker blocker(automaticReplyCheck);
        automaticReplyCheck->setChecked(false);
        QMessageBox::warning(this, windowTitle(), error);
        updateAutomaticStatus(error);
        return;
    }

    const QMessageBox::StandardButton answer = QMessageBox::warning(
        this, windowTitle(),
        text(QStringLiteral("Automatic FT8 mode can emit RF without another click when an eligible CQ is decoded. Configure a separate TX device, connect a suitable antenna/load, arm RF output in TX setup, and observe local regulations. Continue?"),
             QString::fromUtf8(u8"Автоматичний режим FT8 може випромінювати RF без додаткового натискання після декодування дозволеного CQ. Налаштуйте окремий TX-пристрій, під'єднайте відповідну антену/навантаження, озбройте RF-вихід у налаштуваннях TX і дотримуйтеся місцевих правил. Продовжити?")),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) {
        const QSignalBlocker blocker(automaticReplyCheck);
        automaticReplyCheck->setChecked(false);
        updateAutomaticStatus();
        return;
    }

    automaticStage = AutoQsoStage::Idle;
    automaticPartnerCall.clear();
    automaticLastMessage.clear();
    automaticRetryCount = 0;
    automaticLastTxMs = 0;
    automaticTimer->start();
    updateAutomaticStatus(text(QStringLiteral("Armed; listening for an eligible CQ."),
                               QString::fromUtf8(u8"Озброєно; очікування дозволеного CQ.")));
}

QString Ft8StationDialog::formattedSignalReport(int snrDbEstimate) const {
    const int value = (std::max)(-24, (std::min)(20, snrDbEstimate));
    return QStringLiteral("%1%2")
        .arg(value >= 0 ? QStringLiteral("+") : QStringLiteral("-"))
        .arg(std::abs(value), 2, 10, QLatin1Char('0'));
}

void Ft8StationDialog::handleAutomaticDecode(const Ft8MessageInfo &info,
                                              const QDateTime &utc,
                                              double rfFrequencyHz,
                                              double audioFrequencyHz,
                                              int snrDbEstimate) {
    if (!automaticReplyCheck->isChecked() || !isVisible()) return;
    const QString myCall = normalizeFt8Callsign(stationCallEdit->text());
    const QString sender = normalizeFt8Callsign(info.senderCall);
    const QString recipient = normalizeFt8Callsign(info.recipientCall);
    if (myCall.isEmpty() || sender.isEmpty() || sender == myCall) return;

    if (automaticStage == AutoQsoStage::Idle) {
        if (!info.cq || snrDbEstimate < automaticMinSnrSpin->value()) return;
        QString filterReason;
        if (!callsignAllowed(sender, &filterReason)) {
            updateAutomaticStatus(filterReason);
            return;
        }

        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if (nowMs < automaticNextQsoAllowedMs) return;
        const qint64 hourAgo = nowMs - 3600000;
        automaticQsoStartsMs.erase(
            std::remove_if(automaticQsoStartsMs.begin(), automaticQsoStartsMs.end(),
                           [hourAgo](qint64 value) { return value < hourAgo; }),
            automaticQsoStartsMs.end());
        if (automaticQsoStartsMs.size() >= automaticHourlyLimitSpin->value()) {
            updateAutomaticStatus(text(QStringLiteral("Hourly automatic-QSO limit reached."),
                                       QString::fromUtf8(u8"Досягнуто погодинний ліміт автоматичних QSO.")));
            return;
        }
        const qint64 cooldownMs = static_cast<qint64>(automaticCooldownSpin->value()) * 60000;
        if (cooldownMs > 0 && automaticContactByCall.value(sender, 0) > nowMs - cooldownMs) {
            return;
        }

        setQsoPartner(info, utc, rfFrequencyHz, audioFrequencyHz);
        sentReportEdit->setText(formattedSignalReport(snrDbEstimate));
        receivedReportEdit->clear();
        automaticPartnerCall = sender;
        const QDateTime slotTime = utc.isValid() ? utc.toUTC() : QDateTime::currentDateTimeUtc();
        const int receivedParity = static_cast<int>((slotTime.toMSecsSinceEpoch() / 15000) & 1);
        automaticSlotParity = receivedParity ^ 1;
        const int slotIndex = slotCombo->findData(automaticSlotParity);
        if (slotIndex >= 0) slotCombo->setCurrentIndex(slotIndex);
        automaticRetryCount = 0;
        automaticStage = AutoQsoStage::AwaitReport;
        automaticQsoStartsMs.push_back(nowMs);
        automaticContactByCall.insert(sender, nowMs);
        const QString reply = standardMessage(1).simplified();
        if (reply.isEmpty() || !requestAutomaticTransmission(reply, automaticSlotParity)) {
            resetAutomaticQso(text(QStringLiteral("Automatic FT8 reply could not be started."),
                                   QString::fromUtf8(u8"Не вдалося запустити автоматичну FT8-відповідь.")),
                              true);
        }
        return;
    }

    if (sender != automaticPartnerCall || recipient != myCall) return;
    QString filterReason;
    if (!callsignAllowed(sender, &filterReason)) {
        resetAutomaticQso(filterReason);
        return;
    }
    if (rfFrequencyHz > 0.0 && audioFrequencyHz > 0.0) setDialFrequency(rfFrequencyHz - audioFrequencyHz);
    if (audioFrequencyHz > 0.0) txAudioSpin->setValue(static_cast<int>(std::llround(audioFrequencyHz)));
    if (!info.grid.isEmpty()) dxGridEdit->setText(info.grid);
    if (!info.report.isEmpty()) receivedReportEdit->setText(displayReport(info.report));
    sentReportEdit->setText(formattedSignalReport(snrDbEstimate));
    automaticRetryCount = 0;

    if (info.signoff) {
        if (info.raw.contains(QStringLiteral("RR73"))) {
            const QString reply = standardMessage(5).simplified();
            if (!requestAutomaticTransmission(reply, automaticSlotParity)) {
                resetAutomaticQso(text(QStringLiteral("Final automatic FT8 reply failed."),
                                       QString::fromUtf8(u8"Не вдалася фінальна автоматична FT8-відповідь.")),
                                  true);
                return;
            }
        }
        finishAutomaticQso(text(QStringLiteral("Automatic FT8 QSO completed with %1."),
                                QString::fromUtf8(u8"Автоматичне FT8 QSO з %1 завершено."))
                               .arg(sender), true);
        return;
    }

    QString reply;
    if (info.roger) {
        reply = standardMessage(4).simplified();
        automaticStage = AutoQsoStage::AwaitSignoff;
    } else if (!info.report.isEmpty()) {
        reply = standardMessage(3).simplified();
        automaticStage = AutoQsoStage::AwaitRoger;
    } else if (!info.grid.isEmpty()) {
        reply = standardMessage(2).simplified();
        automaticStage = AutoQsoStage::AwaitRoger;
    }
    if (!reply.isEmpty() && !requestAutomaticTransmission(reply, automaticSlotParity)) {
        resetAutomaticQso(text(QStringLiteral("Automatic FT8 sequence stopped because TX failed."),
                               QString::fromUtf8(u8"Автоматичну FT8-послідовність зупинено через помилку TX.")),
                          true);
    }
}

bool Ft8StationDialog::requestAutomaticTransmission(const QString &message, int slotParity) {
    if (!automaticTransmitHandler || message.trimmed().isEmpty()) return false;
    QString filterReason;
    if (!automaticPartnerCall.isEmpty() &&
        !callsignAllowed(automaticPartnerCall, &filterReason)) {
        updateAutomaticStatus(filterReason);
        return false;
    }
    QString error;
    const bool accepted = automaticTransmitHandler(
        message.simplified().toUpper(), dialFrequencySpin->value() * 1e6,
        txAudioSpin->value(), slotParity, &error);
    if (!accepted) {
        updateAutomaticStatus(error.isEmpty()
                                  ? text(QStringLiteral("Transmitter rejected the automatic FT8 message."),
                                         QString::fromUtf8(u8"Передавач відхилив автоматичне FT8-повідомлення."))
                                  : error);
        return false;
    }
    automaticLastMessage = message.simplified().toUpper();
    automaticLastTxMs = QDateTime::currentMSecsSinceEpoch();
    updateAutomaticStatus(text(QStringLiteral("Scheduled: %1"),
                               QString::fromUtf8(u8"Заплановано: %1"))
                              .arg(automaticLastMessage));
    return true;
}

void Ft8StationDialog::checkAutomaticTimeout() {
    if (!automaticReplyCheck->isChecked() || automaticStage == AutoQsoStage::Idle ||
        automaticLastTxMs <= 0 || automaticLastMessage.isEmpty()) return;
    const qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - automaticLastTxMs;
    if (elapsed < static_cast<qint64>(automaticTimeoutSpin->value()) * 1000) return;
    if (automaticRetryCount >= automaticRetriesSpin->value()) {
        resetAutomaticQso(text(QStringLiteral("Automatic FT8 QSO timed out after %1 retries."),
                               QString::fromUtf8(u8"Час автоматичного FT8 QSO вичерпано після %1 повторів."))
                              .arg(automaticRetryCount));
        return;
    }
    ++automaticRetryCount;
    const QString retryMessage = automaticLastMessage;
    if (!requestAutomaticTransmission(retryMessage, automaticSlotParity)) {
        resetAutomaticQso(text(QStringLiteral("Automatic FT8 retry failed; automatic replies were disarmed."),
                               QString::fromUtf8(u8"Повтор FT8 не вдався; автоматичні відповіді роззброєно.")),
                          true);
        return;
    }
    updateAutomaticStatus(text(QStringLiteral("Retry %1/%2: %3"),
                               QString::fromUtf8(u8"Повтор %1/%2: %3"))
                              .arg(automaticRetryCount)
                              .arg(automaticRetriesSpin->value())
                              .arg(retryMessage));
}

void Ft8StationDialog::resetAutomaticQso(const QString &status, bool disarm) {
    automaticStage = AutoQsoStage::Idle;
    automaticPartnerCall.clear();
    automaticLastMessage.clear();
    automaticSlotParity = -1;
    automaticRetryCount = 0;
    automaticLastTxMs = 0;
    if (disarm && automaticCancelHandler) automaticCancelHandler();
    if (disarm && automaticReplyCheck->isChecked()) {
        const QSignalBlocker blocker(automaticReplyCheck);
        automaticReplyCheck->setChecked(false);
        if (automaticTimer) automaticTimer->stop();
    }
    updateAutomaticStatus(status);
}

void Ft8StationDialog::finishAutomaticQso(const QString &status, bool logQso) {
    const QString partner = automaticPartnerCall;
    automaticNextQsoAllowedMs = QDateTime::currentMSecsSinceEpoch() + 30000;
    if (logQso) logCurrentQso();
    if (!partner.isEmpty()) {
        automaticContactByCall.insert(partner, QDateTime::currentMSecsSinceEpoch());
    }
    resetAutomaticQso(status);
}

void Ft8StationDialog::updateAutomaticStatus(const QString &status) {
    if (!automaticStatusLabel) return;
    automaticStopButton->setEnabled(automaticStage != AutoQsoStage::Idle);
    if (!status.isEmpty()) {
        automaticStatusLabel->setText(status);
        return;
    }
    if (!automaticReplyCheck->isChecked()) {
        automaticStatusLabel->setText(text(QStringLiteral("Disarmed. Filter rules still protect manual replies."),
                                           QString::fromUtf8(u8"Роззброєно. Правила фільтра все одно захищають ручні відповіді.")));
    } else if (automaticStage == AutoQsoStage::Idle) {
        automaticStatusLabel->setText(text(QStringLiteral("Armed; listening for an eligible CQ."),
                                           QString::fromUtf8(u8"Озброєно; очікування дозволеного CQ.")));
    } else {
        automaticStatusLabel->setText(text(QStringLiteral("Automatic QSO with %1 is active."),
                                           QString::fromUtf8(u8"Активне автоматичне QSO з %1."))
                                          .arg(automaticPartnerCall));
    }
}
void Ft8StationDialog::closeEvent(QCloseEvent *event) {
    if (automaticReplyCheck && automaticReplyCheck->isChecked()) {
        automaticReplyCheck->setChecked(false);
    }
    saveSettings();
    hide();
    event->ignore();
}

void Ft8StationDialog::hideEvent(QHideEvent *event) {
    if (automaticReplyCheck && automaticReplyCheck->isChecked()) {
        automaticReplyCheck->setChecked(false);
    }
    QDialog::hideEvent(event);
}

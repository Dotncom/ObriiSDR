#include "analyzerinstrumentdialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QSpinBox>
#include <QTabWidget>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {
QDoubleSpinBox *frequencySpin(QWidget *parent, double value) {
    auto *spin = new QDoubleSpinBox(parent);
    spin->setDecimals(3);
    spin->setRange(0.0, 100.0e9);
    spin->setSingleStep(1.0e6);
    spin->setSuffix(QStringLiteral(" Hz"));
    spin->setValue(value);
    return spin;
}
}
AnalyzerInstrumentDialog::AnalyzerInstrumentDialog(bool ukrainianUi, QWidget *parent)
    : QDialog(parent), ukrainian(ukrainianUi) {
    setWindowTitle(text("Analyzer instrument laboratory (draft)",
                        "Лабораторія приладів Analyzer (чернетка)"));
    resize(820, 720);
    setMinimumSize(680, 540);
    setAttribute(Qt::WA_DeleteOnClose);

    auto *root = new QVBoxLayout(this);
    auto *notice = new QLabel(text(
        "Configuration and validation are active. RF execution remains disabled until a qualified hardware backend is connected; this window never fabricates calibrated results.",
        "Налаштування і перевірка плану вже працюють. RF-запуск вимкнений до підключення перевіреного апаратного backend; це вікно ніколи не підміняє відкалібровані результати макетом."), this);
    notice->setWordWrap(true);
    notice->setStyleSheet(QStringLiteral("color:#e8c96b;padding:6px;border:1px solid #665a2b;"));
    root->addWidget(notice);

    auto *tabs = new QTabWidget(this);
    root->addWidget(tabs, 1);

    auto *planPage = new QWidget(tabs);
    auto *planLayout = new QVBoxLayout(planPage);
    auto *measurementBox = new QGroupBox(text("Measurement plan", "План вимірювання"), planPage);
    auto *measurementForm = new QFormLayout(measurementBox);
    modeCombo = new QComboBox(measurementBox);
    modeCombo->addItem(text("Tracking generator", "Генератор стеження"),
                       AnalyzerInstrumentPlanner::modeId(AnalyzerInstrumentMode::TrackingGenerator));
    modeCombo->addItem(text("Scalar network analyzer", "Скалярний аналізатор мереж"),
                       AnalyzerInstrumentPlanner::modeId(AnalyzerInstrumentMode::ScalarNetworkAnalyzer));
    modeCombo->addItem(text("Return loss / SWR", "Зворотні втрати / КСХ"),
                       AnalyzerInstrumentPlanner::modeId(AnalyzerInstrumentMode::ReturnLossSwr));
    modeCombo->addItem(text("Vector network analyzer", "Векторний аналізатор мереж"),
                       AnalyzerInstrumentPlanner::modeId(AnalyzerInstrumentMode::VectorNetworkAnalyzer));
    nameEdit = new QLineEdit(measurementBox);
    sourceCombo = new QComboBox(measurementBox);
    sourceCombo->addItem(text("SDR transmitter", "Передавач SDR"), false);
    sourceCombo->addItem(text("External SCPI generator", "Зовнішній SCPI-генератор"), true);
    txDeviceEdit = new QLineEdit(measurementBox);
    rxDeviceEdit = new QLineEdit(measurementBox);
    startSpin = frequencySpin(measurementBox, 1.0e6);
    stopSpin = frequencySpin(measurementBox, 30.0e6);
    pointsSpin = new QSpinBox(measurementBox);
    pointsSpin->setRange(2, 1000001);
    pointsSpin->setValue(401);
    sourceLevelSpin = new QDoubleSpinBox(measurementBox);
    sourceLevelSpin->setRange(-140.0, 30.0);
    sourceLevelSpin->setDecimals(2);
    sourceLevelSpin->setSuffix(QStringLiteral(" dBm"));
    sourceLevelSpin->setValue(-30.0);
    dwellSpin = new QDoubleSpinBox(measurementBox);
    dwellSpin->setRange(0.0, 60000.0);
    dwellSpin->setSuffix(QStringLiteral(" ms"));
    dwellSpin->setValue(10.0);
    settleSpin = new QDoubleSpinBox(measurementBox);
    settleSpin->setRange(0.0, 60000.0);
    settleSpin->setSuffix(QStringLiteral(" ms"));
    settleSpin->setValue(5.0);
    averagesSpin = new QSpinBox(measurementBox);
    averagesSpin->setRange(1, 100000);
    bidirectionalCheck = new QCheckBox(text("Sweep forward and backward", "Сканувати вперед і назад"), measurementBox);
    gridSummaryLabel = new QLabel(measurementBox);
    gridSummaryLabel->setWordWrap(true);
    measurementForm->addRow(text("Mode:", "Режим:"), modeCombo);
    measurementForm->addRow(text("Plan name:", "Назва плану:"), nameEdit);
    measurementForm->addRow(text("Stimulus source:", "Джерело стимулу:"), sourceCombo);
    measurementForm->addRow(text("TX device ID:", "ID пристрою TX:"), txDeviceEdit);
    measurementForm->addRow(text("RX device ID:", "ID пристрою RX:"), rxDeviceEdit);
    measurementForm->addRow(text("Start:", "Початок:"), startSpin);
    measurementForm->addRow(text("Stop:", "Кінець:"), stopSpin);
    measurementForm->addRow(text("Points:", "Точки:"), pointsSpin);
    measurementForm->addRow(text("Source level:", "Рівень джерела:"), sourceLevelSpin);
    measurementForm->addRow(text("Settle:", "Стабілізація:"), settleSpin);
    measurementForm->addRow(text("Dwell / point:", "Затримка / точку:"), dwellSpin);
    measurementForm->addRow(text("Averages:", "Усереднення:"), averagesSpin);
    measurementForm->addRow(bidirectionalCheck);
    measurementForm->addRow(text("Planned grid:", "Запланована сітка:"), gridSummaryLabel);
    planLayout->addWidget(measurementBox);

    auto *capabilityBox = new QGroupBox(text("Declared hardware capabilities", "Заявлені можливості обладнання"), planPage);
    auto *capabilityLayout = new QFormLayout(capabilityBox);
    receiverCheck = new QCheckBox(text("RX available", "RX доступний"), capabilityBox);
    transmitterCheck = new QCheckBox(text("TX available", "TX доступний"), capabilityBox);
    fullDuplexCheck = new QCheckBox(text("Simultaneous full duplex", "Одночасний повний дуплекс"), capabilityBox);
    coherentCheck = new QCheckBox(text("Shared coherent reference", "Спільне когерентне опорне джерело"), capabilityBox);
    timestampedCheck = new QCheckBox(text("Timestamped phase-stable IQ", "IQ з часовими мітками і стабільною фазою"), capabilityBox);
    externalGeneratorCheck = new QCheckBox(text("External generator reachable", "Зовнішній генератор доступний"), capabilityBox);
    couplerCheck = new QCheckBox(text("Characterized bridge / coupler", "Охарактеризований міст / відгалужувач"), capabilityBox);
    receiverChannelsSpin = new QSpinBox(capabilityBox);
    receiverChannelsSpin->setRange(0, 64);
    transmitterChannelsSpin = new QSpinBox(capabilityBox);
    transmitterChannelsSpin->setRange(0, 64);
    hardwareMinimumSpin = frequencySpin(capabilityBox, 0.0);
    hardwareMaximumSpin = frequencySpin(capabilityBox, 0.0);
    safeOutputSpin = new QDoubleSpinBox(capabilityBox);
    safeOutputSpin->setRange(-140.0, 30.0);
    safeOutputSpin->setSuffix(QStringLiteral(" dBm"));
    safeOutputSpin->setValue(-100.0);
    auto *capabilitiesRow = new QWidget(capabilityBox);
    auto *capabilitiesChecks = new QVBoxLayout(capabilitiesRow);
    capabilitiesChecks->setContentsMargins(0, 0, 0, 0);
    for (QCheckBox *check : {receiverCheck, transmitterCheck, fullDuplexCheck,
                             coherentCheck, timestampedCheck, externalGeneratorCheck, couplerCheck})
        capabilitiesChecks->addWidget(check);
    capabilityLayout->addRow(capabilitiesRow);
    capabilityLayout->addRow(text("RX channels:", "Канали RX:"), receiverChannelsSpin);
    capabilityLayout->addRow(text("TX channels:", "Канали TX:"), transmitterChannelsSpin);
    capabilityLayout->addRow(text("Hardware minimum:", "Мінімум обладнання:"), hardwareMinimumSpin);
    capabilityLayout->addRow(text("Hardware maximum:", "Максимум обладнання:"), hardwareMaximumSpin);
    capabilityLayout->addRow(text("Safe TX limit:", "Безпечна межа TX:"), safeOutputSpin);
    planLayout->addWidget(capabilityBox);
    planLayout->addStretch();
    tabs->addTab(planPage, text("Plan / hardware", "План / обладнання"));

    auto *calibrationPage = new QWidget(tabs);
    auto *calibrationLayout = new QVBoxLayout(calibrationPage);
    auto *calibrationBox = new QGroupBox(text("Calibration state", "Стан калібрування"), calibrationPage);
    auto *calibrationForm = new QFormLayout(calibrationBox);
    calibrationIdEdit = new QLineEdit(calibrationBox);
    impedanceSpin = new QDoubleSpinBox(calibrationBox);
    impedanceSpin->setRange(1.0, 1000.0);
    impedanceSpin->setValue(50.0);
    impedanceSpin->setSuffix(QStringLiteral(" ohm"));
    normalizationCheck = new QCheckBox(text("Scalar through/reference normalization", "Скалярна нормалізація through/reference"), calibrationBox);
    openCheck = new QCheckBox(QStringLiteral("Open"), calibrationBox);
    shortCheck = new QCheckBox(QStringLiteral("Short"), calibrationBox);
    loadCheck = new QCheckBox(QStringLiteral("Load"), calibrationBox);
    thruCheck = new QCheckBox(QStringLiteral("Thru"), calibrationBox);
    phaseCheck = new QCheckBox(text("Phase reference", "Опорна фаза"), calibrationBox);
    auto *standardsRow = new QWidget(calibrationBox);
    auto *standardsLayout = new QHBoxLayout(standardsRow);
    standardsLayout->setContentsMargins(0, 0, 0, 0);
    for (QCheckBox *check : {openCheck, shortCheck, loadCheck, thruCheck, phaseCheck})
        standardsLayout->addWidget(check);
    calibrationNotesEdit = new QPlainTextEdit(calibrationBox);
    calibrationNotesEdit->setMaximumHeight(90);
    calibrationForm->addRow(text("Calibration ID:", "ID калібрування:"), calibrationIdEdit);
    calibrationForm->addRow(text("Reference impedance:", "Опорний імпеданс:"), impedanceSpin);
    calibrationForm->addRow(normalizationCheck);
    calibrationForm->addRow(text("Measured standards:", "Виміряні еталони:"), standardsRow);
    calibrationForm->addRow(text("Notes:", "Примітки:"), calibrationNotesEdit);
    calibrationLayout->addWidget(calibrationBox);
    auto *calibrationWarning = new QLabel(text(
        "These flags record completed procedures; they do not synthesize calibration coefficients. Future hardware adapters must attach the measured coefficient table and fixture identity.",
        "Ці прапорці лише фіксують виконані процедури; вони не створюють калібрувальні коефіцієнти. Майбутній апаратний адаптер має додати виміряну таблицю коефіцієнтів та ідентифікатор оснастки."), calibrationPage);
    calibrationWarning->setWordWrap(true);
    calibrationLayout->addWidget(calibrationWarning);
    calibrationLayout->addStretch();
    tabs->addTab(calibrationPage, text("Calibration", "Калібрування"));

    auto *externalPage = new QWidget(tabs);
    auto *externalLayout = new QVBoxLayout(externalPage);
    auto *scpiBox = new QGroupBox(QStringLiteral("SCPI"), externalPage);
    auto *scpiForm = new QFormLayout(scpiBox);
    scpiEnabledCheck = new QCheckBox(text("Enable external instrument profile", "Увімкнути профіль зовнішнього приладу"), scpiBox);
    scpiResourceEdit = new QLineEdit(scpiBox);
    scpiResourceEdit->setPlaceholderText(QStringLiteral("TCPIP0::192.168.1.50::5025::SOCKET"));
    scpiIdnEdit = new QLineEdit(QStringLiteral("*IDN?"), scpiBox);
    scpiFrequencyEdit = new QLineEdit(QStringLiteral("FREQ %1 HZ"), scpiBox);
    scpiPowerEdit = new QLineEdit(QStringLiteral("POW %1 DBM"), scpiBox);
    scpiOutputEdit = new QLineEdit(QStringLiteral("OUTP %1"), scpiBox);
    scpiTriggerEdit = new QLineEdit(QStringLiteral("*TRG"), scpiBox);
    scpiReadEdit = new QLineEdit(scpiBox);
    scpiTimeoutSpin = new QSpinBox(scpiBox);
    scpiTimeoutSpin->setRange(100, 60000);
    scpiTimeoutSpin->setValue(2000);
    scpiTimeoutSpin->setSuffix(QStringLiteral(" ms"));
    scpiForm->addRow(scpiEnabledCheck);
    scpiForm->addRow(text("Resource:", "Ресурс:"), scpiResourceEdit);
    scpiForm->addRow(QStringLiteral("*IDN?:"), scpiIdnEdit);
    scpiForm->addRow(text("Set frequency:", "Задати частоту:"), scpiFrequencyEdit);
    scpiForm->addRow(text("Set power:", "Задати потужність:"), scpiPowerEdit);
    scpiForm->addRow(text("Output:", "Вихід:"), scpiOutputEdit);
    scpiForm->addRow(text("Trigger:", "Тригер:"), scpiTriggerEdit);
    scpiForm->addRow(text("Read result:", "Прочитати результат:"), scpiReadEdit);
    scpiForm->addRow(text("Timeout:", "Таймаут:"), scpiTimeoutSpin);
    externalLayout->addWidget(scpiBox);

    auto *sessionBox = new QGroupBox(text("Measurement session metadata", "Метадані сесії вимірювання"), externalPage);
    auto *sessionForm = new QFormLayout(sessionBox);
    operatorEdit = new QLineEdit(sessionBox);
    dutEdit = new QLineEdit(sessionBox);
    commentEdit = new QPlainTextEdit(sessionBox);
    commentEdit->setMaximumHeight(90);
    sessionForm->addRow(text("Operator:", "Оператор:"), operatorEdit);
    sessionForm->addRow(text("Device under test:", "Об'єкт вимірювання:"), dutEdit);
    sessionForm->addRow(text("Comment:", "Коментар:"), commentEdit);
    externalLayout->addWidget(sessionBox);
    externalLayout->addStretch();
    tabs->addTab(externalPage, text("SCPI / session", "SCPI / сесія"));

    validationEdit = new QPlainTextEdit(this);
    validationEdit->setReadOnly(true);
    validationEdit->setMaximumHeight(145);
    root->addWidget(validationEdit);

    auto *actions = new QHBoxLayout();
    auto *loadButton = new QPushButton(text("Open plan...", "Відкрити план..."), this);
    auto *saveButton = new QPushButton(text("Save plan...", "Зберегти план..."), this);
    auto *sessionButton = new QPushButton(text("Save session template...", "Зберегти шаблон сесії..."), this);
    auto *validateButton = new QPushButton(text("Validate", "Перевірити"), this);
    executeButton = new QPushButton(text("Execute RF", "Запустити RF"), this);
    executeButton->setEnabled(false);
    executeButton->setToolTip(text("Requires a qualified hardware backend; not enabled in this draft.",
                                   "Потрібен перевірений апаратний backend; у цій чернетці запуск вимкнений."));
    auto *closeButton = new QPushButton(text("Close", "Закрити"), this);
    actions->addWidget(loadButton);
    actions->addWidget(saveButton);
    actions->addWidget(sessionButton);
    actions->addStretch();
    actions->addWidget(validateButton);
    actions->addWidget(executeButton);
    actions->addWidget(closeButton);
    root->addLayout(actions);

    connect(modeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() { updateValidation(); });
    connect(sourceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() { updateValidation(); });
    for (QDoubleSpinBox *spin : {startSpin, stopSpin, sourceLevelSpin, dwellSpin, settleSpin,
                                 hardwareMinimumSpin, hardwareMaximumSpin, safeOutputSpin, impedanceSpin})
        connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this]() { updateValidation(); });
    for (QSpinBox *spin : {pointsSpin, averagesSpin, receiverChannelsSpin, transmitterChannelsSpin, scpiTimeoutSpin})
        connect(spin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this]() { updateValidation(); });
    for (QCheckBox *check : {bidirectionalCheck, receiverCheck, transmitterCheck, fullDuplexCheck,
                             coherentCheck, timestampedCheck, externalGeneratorCheck, couplerCheck,
                             normalizationCheck, openCheck, shortCheck, loadCheck, thruCheck, phaseCheck,
                             scpiEnabledCheck})
        connect(check, &QCheckBox::toggled, this, [this]() { updateValidation(); });
    for (QCheckBox *check : {normalizationCheck, openCheck, shortCheck, loadCheck, thruCheck, phaseCheck}) {
        connect(check, &QCheckBox::toggled, this, [this](bool checked) {
            if (checked && !calibrationCapturedUtc.isValid())
                calibrationCapturedUtc = QDateTime::currentDateTimeUtc();
        });
    }
    connect(scpiResourceEdit, &QLineEdit::textChanged, this, [this]() { updateValidation(); });
    connect(loadButton, &QPushButton::clicked, this, &AnalyzerInstrumentDialog::loadPlan);
    connect(saveButton, &QPushButton::clicked, this, &AnalyzerInstrumentDialog::savePlan);
    connect(sessionButton, &QPushButton::clicked, this, &AnalyzerInstrumentDialog::saveSessionTemplate);
    connect(validateButton, &QPushButton::clicked, this, &AnalyzerInstrumentDialog::updateValidation);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::close);
    connect(this, &QDialog::finished, this, [this]() { persistLastPlan(); });

    restoreLastPlan();
    updateValidation();
}

AnalyzerInstrumentDialog::~AnalyzerInstrumentDialog() {
    persistLastPlan();
}

QString AnalyzerInstrumentDialog::text(const char *english, const char *ukrainianText) const {
    return QString::fromUtf8(ukrainian ? ukrainianText : english);
}

AnalyzerInstrumentPlan AnalyzerInstrumentDialog::planFromUi() const {
    AnalyzerInstrumentPlan plan;
    plan.mode = AnalyzerInstrumentPlanner::modeFromId(modeCombo->currentData().toString());
    plan.name = nameEdit->text();
    plan.useExternalGenerator = sourceCombo->currentData().toBool();
    plan.transmitDeviceId = txDeviceEdit->text();
    plan.receiveDeviceId = rxDeviceEdit->text();
    plan.startHz = startSpin->value();
    plan.stopHz = stopSpin->value();
    plan.points = pointsSpin->value();
    plan.sourceLevelDbm = sourceLevelSpin->value();
    plan.dwellMs = dwellSpin->value();
    plan.settleMs = settleSpin->value();
    plan.averages = averagesSpin->value();
    plan.bidirectionalSweep = bidirectionalCheck->isChecked();
    plan.capabilities.receiverAvailable = receiverCheck->isChecked();
    plan.capabilities.transmitterAvailable = transmitterCheck->isChecked();
    plan.capabilities.fullDuplex = fullDuplexCheck->isChecked();
    plan.capabilities.coherentReference = coherentCheck->isChecked();
    plan.capabilities.timestampedIq = timestampedCheck->isChecked();
    plan.capabilities.externalGenerator = externalGeneratorCheck->isChecked();
    plan.capabilities.directionalCoupler = couplerCheck->isChecked();
    plan.capabilities.receiverChannels = receiverChannelsSpin->value();
    plan.capabilities.transmitterChannels = transmitterChannelsSpin->value();
    plan.capabilities.minimumFrequencyHz = hardwareMinimumSpin->value();
    plan.capabilities.maximumFrequencyHz = hardwareMaximumSpin->value();
    plan.capabilities.maximumSafeOutputDbm = safeOutputSpin->value();
    plan.calibration.calibrationId = calibrationIdEdit->text();
    plan.calibration.notes = calibrationNotesEdit->toPlainText();
    plan.calibration.capturedUtc = calibrationCapturedUtc;
    plan.calibration.referenceImpedanceOhm = impedanceSpin->value();
    plan.calibration.scalarNormalization = normalizationCheck->isChecked();
    plan.calibration.openMeasured = openCheck->isChecked();
    plan.calibration.shortMeasured = shortCheck->isChecked();
    plan.calibration.loadMeasured = loadCheck->isChecked();
    plan.calibration.thruMeasured = thruCheck->isChecked();
    plan.calibration.phaseReferenceMeasured = phaseCheck->isChecked();
    plan.scpi.enabled = scpiEnabledCheck->isChecked();
    plan.scpi.resource = scpiResourceEdit->text();
    plan.scpi.identityQuery = scpiIdnEdit->text();
    plan.scpi.frequencyCommand = scpiFrequencyEdit->text();
    plan.scpi.powerCommand = scpiPowerEdit->text();
    plan.scpi.outputCommand = scpiOutputEdit->text();
    plan.scpi.triggerCommand = scpiTriggerEdit->text();
    plan.scpi.readCommand = scpiReadEdit->text();
    plan.scpi.timeoutMs = scpiTimeoutSpin->value();
    plan.operatorName = operatorEdit->text();
    plan.deviceUnderTest = dutEdit->text();
    plan.comment = commentEdit->toPlainText();
    return plan;
}

void AnalyzerInstrumentDialog::applyPlan(const AnalyzerInstrumentPlan &plan) {
    modeCombo->setCurrentIndex(std::max(0, modeCombo->findData(AnalyzerInstrumentPlanner::modeId(plan.mode))));
    nameEdit->setText(plan.name);
    sourceCombo->setCurrentIndex(std::max(0, sourceCombo->findData(plan.useExternalGenerator)));
    txDeviceEdit->setText(plan.transmitDeviceId);
    rxDeviceEdit->setText(plan.receiveDeviceId);
    startSpin->setValue(plan.startHz);
    stopSpin->setValue(plan.stopHz);
    pointsSpin->setValue(plan.points);
    sourceLevelSpin->setValue(plan.sourceLevelDbm);
    dwellSpin->setValue(plan.dwellMs);
    settleSpin->setValue(plan.settleMs);
    averagesSpin->setValue(plan.averages);
    bidirectionalCheck->setChecked(plan.bidirectionalSweep);
    receiverCheck->setChecked(plan.capabilities.receiverAvailable);
    transmitterCheck->setChecked(plan.capabilities.transmitterAvailable);
    fullDuplexCheck->setChecked(plan.capabilities.fullDuplex);
    coherentCheck->setChecked(plan.capabilities.coherentReference);
    timestampedCheck->setChecked(plan.capabilities.timestampedIq);
    externalGeneratorCheck->setChecked(plan.capabilities.externalGenerator);
    couplerCheck->setChecked(plan.capabilities.directionalCoupler);
    receiverChannelsSpin->setValue(plan.capabilities.receiverChannels);
    transmitterChannelsSpin->setValue(plan.capabilities.transmitterChannels);
    hardwareMinimumSpin->setValue(plan.capabilities.minimumFrequencyHz);
    hardwareMaximumSpin->setValue(plan.capabilities.maximumFrequencyHz);
    safeOutputSpin->setValue(plan.capabilities.maximumSafeOutputDbm);
    calibrationIdEdit->setText(plan.calibration.calibrationId);
    calibrationCapturedUtc = plan.calibration.capturedUtc;
    impedanceSpin->setValue(plan.calibration.referenceImpedanceOhm);
    normalizationCheck->setChecked(plan.calibration.scalarNormalization);
    openCheck->setChecked(plan.calibration.openMeasured);
    shortCheck->setChecked(plan.calibration.shortMeasured);
    loadCheck->setChecked(plan.calibration.loadMeasured);
    thruCheck->setChecked(plan.calibration.thruMeasured);
    phaseCheck->setChecked(plan.calibration.phaseReferenceMeasured);
    calibrationNotesEdit->setPlainText(plan.calibration.notes);
    scpiEnabledCheck->setChecked(plan.scpi.enabled);
    scpiResourceEdit->setText(plan.scpi.resource);
    scpiIdnEdit->setText(plan.scpi.identityQuery);
    scpiFrequencyEdit->setText(plan.scpi.frequencyCommand);
    scpiPowerEdit->setText(plan.scpi.powerCommand);
    scpiOutputEdit->setText(plan.scpi.outputCommand);
    scpiTriggerEdit->setText(plan.scpi.triggerCommand);
    scpiReadEdit->setText(plan.scpi.readCommand);
    scpiTimeoutSpin->setValue(plan.scpi.timeoutMs);
    operatorEdit->setText(plan.operatorName);
    dutEdit->setText(plan.deviceUnderTest);
    commentEdit->setPlainText(plan.comment);
    updateValidation();
}

void AnalyzerInstrumentDialog::updateValidation() {
    const AnalyzerInstrumentPlan plan = planFromUi();
    const AnalyzerPlanValidation validation = AnalyzerInstrumentPlanner::validate(plan);
    const double step = plan.points > 1 ? (plan.stopHz - plan.startHz) / (plan.points - 1) : 0.0;
    const double durationSeconds = std::max(0.0, (plan.dwellMs + plan.settleMs) *
        static_cast<double>(plan.points) * plan.averages * (plan.bidirectionalSweep ? 2.0 : 1.0) / 1000.0);
    gridSummaryLabel->setText(text("%1 Hz/point; estimated %2 s", "%1 Гц/точку; орієнтовно %2 с")
                                  .arg(step, 0, 'g', 8)
                                  .arg(durationSeconds, 0, 'f', 2));
    QString display = validation.summary();
    display += text("\nBACKEND: no qualified RF executor is connected in this build.",
                    "\nBACKEND: у цій збірці не підключено перевірений RF-виконавець.");
    validationEdit->setPlainText(display);
    validationEdit->setStyleSheet(validation.errors.isEmpty() && validation.requirements.isEmpty()
                                      ? QStringLiteral("color:#8fe0c2;")
                                      : QStringLiteral("color:#e8c96b;"));
}

void AnalyzerInstrumentDialog::loadPlan() {
    const QString path = QFileDialog::getOpenFileName(this, text("Open analyzer plan", "Відкрити план аналізатора"),
                                                       QString(), QStringLiteral("Analyzer plan (*.json)"));
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, windowTitle(), file.errorString());
        return;
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError) {
        QMessageBox::warning(this, windowTitle(), error.errorString());
        return;
    }
    if (!document.isObject() ||
        document.object().value(QStringLiteral("schema")).toString() !=
            QStringLiteral("obrii-analyzer-plan-v1") ||
        !QStringList{QStringLiteral("tracking-generator"), QStringLiteral("sna"),
                     QStringLiteral("return-loss-swr"), QStringLiteral("vna")}
             .contains(document.object().value(QStringLiteral("mode")).toString())) {
        QMessageBox::warning(this, windowTitle(),
                             text("Unsupported analyzer plan format or instrument mode.",
                                  "Непідтримуваний формат плану аналізатора або режим приладу."));
        return;
    }
    applyPlan(AnalyzerInstrumentPlan::fromJson(document.object()));
}

void AnalyzerInstrumentDialog::savePlan() {
    const QString path = QFileDialog::getSaveFileName(this, text("Save analyzer plan", "Зберегти план аналізатора"),
                                                       QStringLiteral("analyzer-plan.json"),
                                                       QStringLiteral("Analyzer plan (*.json)"));
    if (path.isEmpty()) return;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, windowTitle(), file.errorString());
        return;
    }
    const QByteArray data = QJsonDocument(planFromUi().toJson()).toJson(QJsonDocument::Indented);
    if (file.write(data) != data.size() || !file.commit())
        QMessageBox::warning(this, windowTitle(), file.errorString());
}

void AnalyzerInstrumentDialog::saveSessionTemplate() {
    const QString path = QFileDialog::getSaveFileName(this, text("Save analyzer session template", "Зберегти шаблон сесії аналізатора"),
                                                       QStringLiteral("analyzer-session.json"),
                                                       QStringLiteral("Analyzer session (*.json)"));
    if (path.isEmpty()) return;
    AnalyzerMeasurementSession session;
    session.sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    session.applicationVersion = QString::fromLatin1(OBRII_ANALYZER_VERSION);
    session.startedUtc = QDateTime::currentDateTimeUtc();
    session.plan = planFromUi();
    session.eventLog << QStringLiteral("Draft session created; no RF measurement was executed");
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, windowTitle(), file.errorString());
        return;
    }
    const QByteArray data = QJsonDocument(session.toJson()).toJson(QJsonDocument::Indented);
    if (file.write(data) != data.size() || !file.commit())
        QMessageBox::warning(this, windowTitle(), file.errorString());
}

void AnalyzerInstrumentDialog::restoreLastPlan() {
    QSettings settings;
    const QByteArray json = settings.value(QStringLiteral("instrumentLab/lastPlan")).toByteArray();
    if (json.isEmpty()) return;
    const QJsonDocument document = QJsonDocument::fromJson(json);
    if (document.isObject()) applyPlan(AnalyzerInstrumentPlan::fromJson(document.object()));
}

void AnalyzerInstrumentDialog::persistLastPlan() const {
    QSettings settings;
    settings.setValue(QStringLiteral("instrumentLab/lastPlan"),
                      QJsonDocument(planFromUi().toJson()).toJson(QJsonDocument::Compact));
}

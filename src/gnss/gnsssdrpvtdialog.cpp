#include "gnsssdrpvtdialog.h"
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QVBoxLayout>
#include <cmath>

GnssSdrPvtDialog::GnssSdrPvtDialog(const Translator &translate, QWidget *parent)
    : QDialog(parent), text(translate) {
    setWindowTitle(text("gnss_pvt_title", "GNSS SDR: tracking / navigation / PVT (PocketSDR)"));
    resize(670, 480);
    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout;
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    source = new QComboBox(this);
    source->addItems({text("live_iq", "Live IQ"), text("raw_iq_file", "Raw IQ file")});
    form->addRow(text("source", "Source"), source);
    auto *fileRow = new QHBoxLayout;
    path = new QLineEdit(this); auto *browse = new QPushButton(text("open", "Open"), this);
    fileRow->addWidget(path, 1); fileRow->addWidget(browse);
    form->addRow(text("file", "File"), fileRow);
    format = new QComboBox(this); format->addItems({"cf32", "cs16", "cs8"});
    form->addRow(text("iq_format", "IQ format (I+jQ, little endian)"), format);
    sampleRate = new QDoubleSpinBox(this); sampleRate->setRange(2.046, 32); sampleRate->setDecimals(6); sampleRate->setSuffix(" MS/s"); sampleRate->setValue(4);
    center = new QDoubleSpinBox(this); center->setRange(1000, 1700); center->setDecimals(6); center->setSuffix(" MHz"); center->setValue(1575.42);
    form->addRow(text("sample_rate", "Sample rate"), sampleRate);
    form->addRow(text("center_frequency", "Center frequency"), center);
    auto *systemRow = new QGridLayout;
    int systemIndex = 0;
    for (const QString &name : {QStringLiteral("GPS"), QStringLiteral("GLONASS"), QStringLiteral("Galileo"), QStringLiteral("BeiDou"), QStringLiteral("QZSS"), QStringLiteral("SBAS")}) {
        auto *check = new QCheckBox(name, this); check->setChecked(name == QStringLiteral("GPS")); systems << check; systemRow->addWidget(check, systemIndex / 3, systemIndex % 3); ++systemIndex;
    }
    form->addRow(text("gnss_systems", "Systems"), systemRow);
    map = new QCheckBox(text("gnss_pvt_map", "Use this source for map and satellite table"), this);
    map->setChecked(true); form->addRow(map);
    layout->addLayout(form);
    auto *buttons = new QHBoxLayout;
    startButton = new QPushButton(text("start", "Start"), this);
    stopButton = new QPushButton(text("stop", "Stop"), this); stopButton->setEnabled(false);
    buttons->addWidget(startButton); buttons->addWidget(stopButton); layout->addLayout(buttons);
    status = new QLabel(text("gnss_pvt_ready", "Ready. No automatic background tracking."), this); status->setWordWrap(true); layout->addWidget(status);
    log = new QPlainTextEdit(this); log->setReadOnly(true); log->setMaximumBlockCount(150); layout->addWidget(log, 1);
    process = new QProcess(this); process->setProcessChannelMode(QProcess::MergedChannels);
#ifdef Q_OS_WIN
    process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *arguments) {
        arguments->flags |= 0x08000000; // No console; continuous IQ processing needs normal priority.
    });
#endif
    connect(process, &QProcess::readyReadStandardOutput, this, [this] { appendLog(QString::fromLocal8Bit(process->readAllStandardOutput().right(8192))); });
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, [this](int code, QProcess::ExitStatus exit) {
        if (stoppingProcess) return;
        if (stream) stream->requestStop();
        startButton->setEnabled(true); stopButton->setEnabled(false);
        status->setText(exit == QProcess::NormalExit && code == 0 ? text("completed", "Completed") : text("backend_exit", "Backend exit: %1").arg(code));
    });
    connect(process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        if (stoppingProcess) return;
        const QString error = process->errorString();
        appendLog(error);
        stop();
        status->setText(text("backend_exit", "Backend error: %1").arg(error));
    });
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString selected = QFileDialog::getOpenFileName(this, text("raw_iq_file", "Raw IQ file"), path->text(), "IQ (*.wav *.raw *.bin *.cf32 *.cs16 *.cs8);;All files (*)");
        if (!selected.isEmpty()) {
            path->setText(selected);
            GnssSdr::Input in; in.path = selected; in.sampleRateHz = sampleRate->value() * 1e6; in.centerHz = center->value() * 1e6; in.format = format->currentText();
            QString error;
            if (GnssSdr::readInputMetadata(in, &error)) { sampleRate->setValue(in.sampleRateHz / 1e6); center->setValue(in.centerHz / 1e6); format->setCurrentText(in.format); }
            else status->setText(error);
        }
    });
    connect(source, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        path->setEnabled(index == 1); format->setEnabled(index == 1); sampleRate->setEnabled(index == 1); center->setEnabled(index == 1);
        if (index == 0) setReceiverContext(receiverRunning, receiverAllowed, receiverRate, receiverCenter);
    });
    connect(startButton, &QPushButton::clicked, this, &GnssSdrPvtDialog::start);
    connect(stopButton, &QPushButton::clicked, this, &GnssSdrPvtDialog::stop);
    path->setEnabled(false); format->setEnabled(false); sampleRate->setEnabled(false); center->setEnabled(false);
}
GnssSdrPvtDialog::~GnssSdrPvtDialog() { stop(); }
void GnssSdrPvtDialog::closeEvent(QCloseEvent *event) { stop(); QDialog::closeEvent(event); }
void GnssSdrPvtDialog::appendLog(const QString &message) { if (!message.trimmed().isEmpty()) log->appendPlainText(message.trimmed()); }
void GnssSdrPvtDialog::setReceiverContext(bool running, bool allowed, double rate, double frequency) {
    receiverRunning = running; receiverAllowed = allowed; receiverRate = rate; receiverCenter = frequency;
    if (stream && activeInput.live && (!running || !allowed || rate != activeInput.sampleRateHz || frequency != activeInput.centerHz)) {
        stop(); status->setText(text("gnss_pvt_retune_stop", "Tracking stopped: RX stopped, retuned or its IQ source changed."));
    }
    if (source->currentIndex() == 0) { sampleRate->setValue(rate / 1e6); center->setValue(frequency / 1e6); }
}
void GnssSdrPvtDialog::start() {
    stop();
    activeInput.live = source->currentIndex() == 0;
    activeInput.path = path->text(); activeInput.format = format->currentText();
    activeInput.sampleRateHz = activeInput.live ? receiverRate : sampleRate->value() * 1e6;
    activeInput.centerHz = activeInput.live ? receiverCenter : center->value() * 1e6;
    QString error;
    if (!activeInput.live && !GnssSdr::readInputMetadata(activeInput, &error)) { status->setText(error); return; }
    if (activeInput.live && (!receiverRunning || !receiverAllowed)) {
        status->setText(text("gnss_pvt_rx_required", "Start a stationary local RX first; disconnect serial GNSS to avoid mixing position sources.")); return;
    }
    QString executable = QDir(QCoreApplication::applicationDirPath()).filePath("pocketsdr/pocket_trk");
#ifdef Q_OS_WIN
    executable += ".exe";
#endif
    if (!QFileInfo::exists(executable)) executable = QStandardPaths::findExecutable("pocket_trk");
    if (executable.isEmpty()) { status->setText(text("gnss_pvt_missing", "PocketSDR runtime is missing. See THIRD_PARTY_LICENSES / backend setup.")); return; }
    QStringList selected; for (auto *check : systems) if (check->isChecked()) selected << check->text();
    GnssSdr::trackingArguments(activeInput, selected, 1, 2, &error);
    if (!error.isEmpty()) { status->setText(error); return; }
    const QString working = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/gnss-pvt";
    if (!QDir().mkpath(working)) { status->setText(text("private_directory_error", "Cannot create private backend working directory.")); return; }
    process->setWorkingDirectory(working);
    if (map->isChecked() && receiverAllowed) emit mapSourceStarted();
    stream = new GnssSdrStream(activeInput, this);
    const int run = generation;
    connect(stream, &GnssSdrStream::listening, this, [this, selected, executable, run](quint16 iq, quint16 nmea) {
        if (!stream || generation != run) return;
        QString error; const QStringList args = GnssSdr::trackingArguments(activeInput, selected, iq, nmea, &error);
        stoppingProcess = false;
        process->start(executable, args);
    });
    connect(stream, &GnssSdrStream::nmeaLine, this, [this, run](const QString &line) { if (generation != run) return; appendLog(line); if (map->isChecked() && receiverAllowed) emit positionSentence(line); });
    connect(stream, &GnssSdrStream::samplesSent, this, [this, run](quint64 count) { if (generation != run) return; status->setText(text("gnss_pvt_tracking", "Tracking: %1 s of IQ supplied; waiting for navigation data and a valid fix.").arg(count / activeInput.sampleRateHz, 0, 'f', 1)); });
    connect(stream, &GnssSdrStream::streamError, this, [this, run](const QString &message) { if (generation != run) return; appendLog(message); stop(); status->setText(message); });
    startButton->setEnabled(false); stopButton->setEnabled(true);
    status->setText(text("gnss_pvt_starting", "Starting PocketSDR...")); stream->start(QThread::NormalPriority);
}
void GnssSdrPvtDialog::stop() {
    ++generation;
    if (stream) stream->requestStop();
    stoppingProcess = true;
    if (process->state() != QProcess::NotRunning) {
        process->terminate(); if (!process->waitForFinished(300)) { process->kill(); process->waitForFinished(1000); }
    }
    if (stream) { stream->wait(); delete stream; stream = nullptr; }
    startButton->setEnabled(true); stopButton->setEnabled(false);
}

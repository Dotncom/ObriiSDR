#include "lrptdecodedialog.h"
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDirIterator>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSaveFile>
#include <QStandardPaths>
#include <QThread>
#include <QUrl>
#include <QVBoxLayout>
#include <memory>

namespace {
QString preparePrivateRuntime(const QString &source, const QString &destination) {
    if (!QDir().mkpath(destination)) return QStringLiteral("Cannot create private SatDump directory.");
    if (source.isEmpty()) return {};
    // SatDump 1.2.2 finds its resources relative to cwd on Windows. Keep its
    // config/cache/logs out of the portable distribution and out of releases.
    for (const QString &entry : {QStringLiteral("resources"), QStringLiteral("plugins"), QStringLiteral("pipelines")}) {
        const QDir root(source + '/' + entry);
        QDirIterator iterator(root.absolutePath(), QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
        while (iterator.hasNext()) {
            if (QThread::currentThread()->isInterruptionRequested()) return QStringLiteral("Cancelled");
            const QString file = iterator.next();
            const QString target = destination + '/' + entry + '/' + root.relativeFilePath(file);
            const QFileInfo from(file), to(target);
            if (to.exists() && to.size() == from.size() && to.lastModified() >= from.lastModified()) continue;
            if (!QDir().mkpath(QFileInfo(target).absolutePath())) return QStringLiteral("Cannot create runtime resource directory.");
            if (to.exists() && !QFile::remove(target)) return QStringLiteral("Cannot update runtime resource.");
            if (!QFile::copy(file, target)) return QStringLiteral("Cannot copy runtime resource: %1").arg(from.fileName());
        }
    }
    QFile config(source + "/satdump_cfg.json");
    if (!config.open(QIODevice::ReadOnly)) return QStringLiteral("SatDump configuration is missing.");
    QJsonObject object = QJsonDocument::fromJson(config.readAll()).object();
    if (object.isEmpty()) return QStringLiteral("Invalid SatDump configuration.");
    QJsonObject general = object.value("satdump_general").toObject();
    QJsonObject logging = general.value("log_to_file").toObject(); logging.insert("value", false);
    general.insert("log_to_file", logging); object.insert("satdump_general", general);
    QSaveFile saved(destination + "/satdump_cfg.json");
    if (!saved.open(QIODevice::WriteOnly) || saved.write(QJsonDocument(object).toJson()) < 0 || !saved.commit())
        return QStringLiteral("Cannot save private SatDump configuration.");
    return {};
}
}

LrptDecodeDialog::LrptDecodeDialog(const Translator &translate, QWidget *parent) : QDialog(parent), text(translate) {
    setWindowTitle(text("lrpt_decode_title", "Meteor LRPT image decoder (SatDump)")); resize(820, 700);
    auto *layout = new QVBoxLayout(this); auto *form = new QFormLayout;
    const auto fileRow = [this](QLineEdit *edit, bool directory) {
        auto *row = new QHBoxLayout; auto *button = new QPushButton(text("open", "Open"), this);
        row->addWidget(edit, 1); row->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, edit, directory] {
            const QString selected = directory ? QFileDialog::getExistingDirectory(this, text("output_directory", "Output directory"), edit->text())
                : QFileDialog::getOpenFileName(this, text("raw_iq_file", "Raw IQ file"), edit->text(), "IQ (*.wav *.raw *.bin *.cf32 *.cs16 *.cs8 *.cu8);;All files (*)");
            if (!selected.isEmpty()) edit->setText(selected);
        }); return row;
    };
    input = new QLineEdit(this); output = new QLineEdit(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/ObriiSDR/LRPT", this);
    form->addRow(text("raw_iq_file", "Raw IQ file"), fileRow(input, false));
    form->addRow(text("output_directory", "Output directory"), fileRow(output, true));
    pipeline = new QComboBox(this);
    pipeline->addItem("Meteor M2-x OQPSK 72k", "meteor_m2-x_lrpt");
    pipeline->addItem("Meteor M2-x OQPSK 80k", "meteor_m2-x_lrpt_80k");
    pipeline->addItem("Meteor M2 QPSK 72k", "meteor_m2_lrpt");
    form->addRow(text("mode", "Mode"), pipeline);
    format = new QComboBox(this); format->addItems({"cf32", "cs16", "cs8", "cu8"});
    form->addRow(text("iq_format", "IQ format (I+jQ, little endian)"), format);
    sampleRate = new QDoubleSpinBox(this); sampleRate->setRange(0.144, 1000); sampleRate->setDecimals(6); sampleRate->setSuffix(" MS/s"); sampleRate->setValue(1);
    form->addRow(text("sample_rate", "Sample rate"), sampleRate);
    auto *options = new QHBoxLayout;
    dcBlock = new QCheckBox(text("dc_block", "DC removal"), this); dcBlock->setChecked(true);
    iqSwap = new QCheckBox(text("iq_swap", "Swap I/Q"), this); options->addWidget(dcBlock); options->addWidget(iqSwap); form->addRow(options);
    layout->addLayout(form);
    auto *row = new QHBoxLayout;
    startButton = new QPushButton(text("decode", "Decode"), this); stopButton = new QPushButton(text("stop", "Stop"), this); stopButton->setEnabled(false);
    auto *folder = new QPushButton(text("open_output", "Open output"), this);
    row->addWidget(startButton); row->addWidget(stopButton); row->addWidget(folder); layout->addLayout(row);
    status = new QLabel(text("lrpt_decode_ready", "Offline IQ decoder ready."), this); status->setWordWrap(true); layout->addWidget(status);
    images = new QComboBox(this); layout->addWidget(images);
    image = new QLabel(this); image->setAlignment(Qt::AlignCenter); image->setMinimumSize(200, 160); image->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding); layout->addWidget(image, 1);
    log = new QPlainTextEdit(this); log->setReadOnly(true); log->setMaximumBlockCount(200); log->setMaximumHeight(100); layout->addWidget(log);
    process = new QProcess(this); process->setProcessChannelMode(QProcess::MergedChannels);
#ifdef Q_OS_WIN
    process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *arguments) {
        arguments->flags |= 0x08000000 | 0x00004000;
    });
#endif
    connect(process, &QProcess::readyReadStandardOutput, this, [this] {
        const QString text = QString::fromLocal8Bit(process->readAllStandardOutput().right(8192)).trimmed();
        if (!text.isEmpty()) log->appendPlainText(text);
    });
    connect(process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) { status->setText(process->errorString()); startButton->setEnabled(true); stopButton->setEnabled(false); });
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, [this](int code, QProcess::ExitStatus exit) {
        loadResults(); startButton->setEnabled(true); stopButton->setEnabled(false);
        if (exit != QProcess::NormalExit || code != 0) status->setText(text("backend_exit", "Backend exit: %1").arg(code));
        else status->setText(images->count() ? text("lrpt_images_decoded", "Decoded images: %1").arg(images->count()) : text("lrpt_no_images", "Processing finished, but no image was decoded. Check signal, mode and IQ metadata."));
    });
    connect(images, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] { preview(); });
    connect(folder, &QPushButton::clicked, this, [this] { if (!resultDirectory.isEmpty()) QDesktopServices::openUrl(QUrl::fromLocalFile(resultDirectory)); });
    connect(startButton, &QPushButton::clicked, this, &LrptDecodeDialog::start);
    connect(stopButton, &QPushButton::clicked, this, &LrptDecodeDialog::stop);
}
LrptDecodeDialog::~LrptDecodeDialog() { stop(); }
void LrptDecodeDialog::closeEvent(QCloseEvent *event) { stop(); QDialog::closeEvent(event); }
void LrptDecodeDialog::stop() {
    ++generation;
    if (preparation) { preparation->requestInterruption(); preparation->wait(); }
    if (process->state() != QProcess::NotRunning) { process->terminate(); if (!process->waitForFinished(300)) { process->kill(); process->waitForFinished(1000); } }
    startButton->setEnabled(true); stopButton->setEnabled(false);
}
void LrptDecodeDialog::start() {
    stop();
    if (!QFileInfo(input->text()).isFile()) { status->setText(text("select_iq_file", "Select an IQ recording first.")); return; }
    const QDir bundle(QCoreApplication::applicationDirPath() + "/satdump");
    QString executable = bundle.filePath("satdump");
#ifdef Q_OS_WIN
    executable += ".exe";
#endif
    QString bundledRoot;
    if (QFileInfo::exists(executable)) bundledRoot = bundle.absolutePath();
    else executable = QStandardPaths::findExecutable("satdump");
    if (executable.isEmpty()) { status->setText(text("lrpt_runtime_missing", "SatDump runtime is missing. See THIRD_PARTY_LICENSES / backend setup.")); return; }
    resultDirectory = QDir(output->text()).absoluteFilePath(QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss-zzz"));
    if (!QDir().mkpath(resultDirectory)) { status->setText(text("output_directory_error", "Cannot create output directory.")); return; }
    const QString working = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/satdump-1.2.2";
    QStringList args{pipeline->currentData().toString(), "baseband", QFileInfo(input->text()).absoluteFilePath(), resultDirectory,
                     "--samplerate", QString::number(sampleRate->value() * 1e6, 'g', 12), "--baseband_format", format->currentText(),
                     "--tle_override", working + "/offline-no-network.tle"};
    if (dcBlock->isChecked()) args << "--dc_block";
    if (iqSwap->isChecked()) args << "--iq_swap";
    const int run = generation;
    auto error = std::make_shared<QString>();
    auto *worker = QThread::create([bundledRoot, working, error] { *error = preparePrivateRuntime(bundledRoot, working); });
    preparation = worker;
    connect(worker, &QThread::finished, this, [this, worker, run, error, working, executable, args] {
        if (preparation == worker) preparation = nullptr;
        if (generation != run) return;
        if (!error->isEmpty()) { status->setText(*error); startButton->setEnabled(true); stopButton->setEnabled(false); return; }
        process->setWorkingDirectory(working); status->setText(text("decoding", "Decoding...")); process->start(executable, args);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    status->setText(text("preparing_backend", "Preparing private backend resources...")); startButton->setEnabled(false); stopButton->setEnabled(true);
    worker->start(QThread::LowPriority);
}
void LrptDecodeDialog::loadResults() {
    images->clear();
    QDirIterator iterator(resultDirectory, {"*.png", "*.jpg", "*.jpeg"}, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (iterator.hasNext() && images->count() < 128) { const QString file = iterator.next(); images->addItem(QDir(resultDirectory).relativeFilePath(file), file); }
    preview();
}
void LrptDecodeDialog::preview() {
    QImageReader reader(images->currentData().toString()); const QSize size = reader.size();
    if (size.isValid()) reader.setScaledSize(size.scaled(image->size().expandedTo(QSize(200, 160)), Qt::KeepAspectRatio));
    const QImage decoded = reader.read(); image->setPixmap(QPixmap::fromImage(decoded));
}

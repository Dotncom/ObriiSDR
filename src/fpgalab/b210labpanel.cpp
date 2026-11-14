#include "b210labpanel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QVBoxLayout>

namespace {
const auto dual = QStringLiteral("b210-dual-fabric");
const auto siso = QStringLiteral("b210-siso-strobe");

QString shellArgument(QString arg) {
    return QStringLiteral("'") + arg.replace(QLatin1Char('\''), QStringLiteral("'\\''")) +
           QLatin1Char('\'');
}

QLineEdit *pathRow(QFormLayout *form, const QString &label, const QString &key,
                   bool directory = false, const QString &filter = {}) {
    auto *edit = new QLineEdit;
    auto *row = new QHBoxLayout;
    auto *button = new QPushButton(QStringLiteral("Огляд…"));
    row->addWidget(edit, 1);
    row->addWidget(button);
    form->addRow(label, row);
    if (!key.isEmpty()) {
        const QString setting = QStringLiteral("b210/") + key;
        edit->setText(QSettings().value(setting).toString());
        QObject::connect(edit, &QLineEdit::textChanged, edit, [setting](const QString &text) {
            QSettings().setValue(setting, text);
        });
    }
    QObject::connect(button, &QPushButton::clicked, edit, [=] {
        const QString path = directory ? QFileDialog::getExistingDirectory(edit, label, edit->text())
                                       : QFileDialog::getOpenFileName(edit, label, edit->text(), filter);
        if (!path.isEmpty()) edit->setText(path);
    });
    return edit;
}

QLineEdit *remotePath(QFormLayout *form, const QString &label, const QString &key) {
    auto *edit = new QLineEdit;
    const QString setting = QStringLiteral("b210/") + key;
    edit->setText(QSettings().value(setting).toString());
    form->addRow(label, edit);
    QObject::connect(edit, &QLineEdit::textChanged, edit, [setting](const QString &text) {
        QSettings().setValue(setting, text);
    });
    return edit;
}
}

B210LabPanel::B210LabPanel(std::function<QByteArray(QString *)> pipeline,
                           std::function<void(const QString &)> openProject,
                           std::function<bool()> prepareBuild, QWidget *parent)
    : QWidget(parent), pipeline_(std::move(pipeline)), openProject_(std::move(openProject)),
      prepareBuild_(std::move(prepareBuild)), rtlFile_(new QTemporaryFile(this)) {
    setObjectName(QStringLiteral("b210LabPanel"));
    auto *layout = new QVBoxLayout(this);
    auto *buildBox = new QGroupBox(QStringLiteral("Проєкт openXC7"), this);
    auto *buildForm = new QFormLayout(buildBox);
    recipe_ = pathRow(buildForm, QStringLiteral("Тека B210-Kintex7-openXC7"), QStringLiteral("recipe"), true);
    vendor_ = pathRow(buildForm, QStringLiteral("Вихідний проєкт виробника"), QStringLiteral("vendor"), true);
    environment_ = pathRow(buildForm, QStringLiteral("Оточення Linux/WSL (.sh)"), QStringLiteral("environment"));
    environment_->setToolTip(QStringLiteral("Необов'язковий shell-файл з B210_YOSYS, B210_NEXTPNR_ROOT, B210_CHIPDB, B210_FPGA_AS і B210_PRJXRAY_BIN."));
    variant_ = new QComboBox;
    variant_->addItem(QStringLiteral("Два канали + GNSS"), dual);
    variant_->addItem(QStringLiteral("Один канал + GNSS"), siso);
    buildForm->addRow(QStringLiteral("Варіант"), variant_);
    passthrough_ = new QCheckBox(QStringLiteral("Вбудувати наскрізний IQ-блок з ескізу"));
    buildForm->addRow(passthrough_);
    create_ = new QPushButton(QStringLiteral("Створити проєкт…"));
    build_ = new QPushButton(QStringLiteral("Зібрати образ…"));
    auto *buildButtons = new QHBoxLayout;
    buildButtons->addWidget(create_);
    buildButtons->addWidget(build_);
    buildForm->addRow(buildButtons);
    layout->addWidget(buildBox);

    auto *testBox = new QGroupBox(QStringLiteral("Порівняння RX на B210 · 2 × 30,72 MS/s"), this);
    auto *testForm = new QFormLayout(testBox);
    host_ = remotePath(testForm, QStringLiteral("SSH на Raspberry"), QStringLiteral("host"));
    host_->setPlaceholderText(QStringLiteral("користувач@адреса або порожньо для локального B210"));
    cli_ = remotePath(testForm, QStringLiteral("Шлях ObriiCli на Raspberry"), QStringLiteral("cli"));
    baseline_ = remotePath(testForm, QStringLiteral("Еталонний .bin на Raspberry"), QStringLiteral("baseline"));
    library_ = remotePath(testForm, QStringLiteral("Бібліотека UHD на Raspberry"), QStringLiteral("library"));
    candidate_ = pathRow(testForm, QStringLiteral("Тестовий образ (.bin)"), {}, false, QStringLiteral("UHD (*.bin)"));
    seconds_ = new QSpinBox;
    seconds_->setRange(5, 120);
    seconds_->setValue(15);
    seconds_->setSuffix(QStringLiteral(" с"));
    center_ = new QDoubleSpinBox;
    center_->setRange(70.0, 6000.0);
    center_->setDecimals(6);
    center_->setValue(100.0);
    center_->setSuffix(QStringLiteral(" MHz"));
    testForm->addRow(QStringLiteral("Центральна частота RX"), center_);
    gnss_ = new QCheckBox(QStringLiteral("NMEA / фікс (UART 38400)"));
    pps_ = new QCheckBox(QStringLiteral("GPS PPS"));
    auto *options = new QHBoxLayout;
    options->addWidget(seconds_);
    options->addWidget(gnss_);
    options->addWidget(pps_);
    options->addStretch();
    testForm->addRow(QStringLiteral("Тривалість кожного RX"), options);
    test_ = new QPushButton(QStringLiteral("Порівняти через UHD"));
    auto *save = new QPushButton(QStringLiteral("Зберегти звіт…"));
    auto *testButtons = new QHBoxLayout;
    testButtons->addWidget(test_);
    testButtons->addWidget(save);
    testForm->addRow(testButtons);
    layout->addWidget(testBox);
    log_ = new QPlainTextEdit;
    log_->setReadOnly(true);
    log_->setMaximumBlockCount(5000);
    log_->setPlaceholderText(QStringLiteral("Журнал збірки, таймінги, SHA-256 і звіт RX."));
    layout->addWidget(log_, 1);

    connect(create_, &QPushButton::clicked, this, [this] { startBuild(true); });
    connect(build_, &QPushButton::clicked, this, [this] { startBuild(false); });
    connect(test_, &QPushButton::clicked, this, [this] { startTest(); });
    connect(save, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Звіт B210"), {}, QStringLiteral("Текст (*.txt)"));
        if (path.isEmpty()) return;
        const QByteArray contents = log_->toPlainText().toUtf8();
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size() || !file.commit())
            log(file.errorString());
    });
    connect(&process_, &QProcess::readyReadStandardOutput, this, [this] {
        const QByteArray output = process_.readAllStandardOutput();
        if (job_ == Job::Test) result_ += output;
        else log(QString::fromUtf8(output));
    });
    connect(&process_, &QProcess::readyReadStandardError, this, [this] {
        log(QString::fromUtf8(process_.readAllStandardError()));
    });
    connect(&process_, &QProcess::started, this, [this] {
        if (job_ == Job::Test) {
            const QByteArray length = QByteArray::number(checkScript_.size()).rightJustified(8, '0');
            process_.write(length + checkScript_);
            sendImage();
        } else process_.closeWriteChannel();
    });
    connect(&process_, &QProcess::bytesWritten, this, [this] { sendImage(); });
    connect(&process_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &B210LabPanel::finish);
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;
        log(process_.errorString());
        image_.close();
        if (!rtlFile_->fileName().isEmpty()) rtlFile_->remove();
        setBusy(false);
    });
}

void B210LabPanel::setOpenPath(const QString &folder, const QString &image) {
    projectPath_.clear();
    variant_->setEnabled(true);
    if (!image.isEmpty()) {
        candidate_->setText(image);
        return;
    }
    const QDir project(folder);
    if (project.exists(QStringLiteral("build.sh")) && project.exists(QStringLiteral("vendor-inputs.json")))
        recipe_->setText(folder);
    if (project.exists(QStringLiteral("B210_Project_Firmwire.xpr")))
        vendor_->setText(folder);
    QFile manifest(project.filePath(QStringLiteral(".obrii-b210.json")));
    if (!manifest.open(QIODevice::ReadOnly)) return;
    const auto data = QJsonDocument::fromJson(manifest.read(65536)).object();
    const int index = variant_->findData(data.value(QStringLiteral("variant")).toString());
    if (data.value(QStringLiteral("schema")).toInt() != 1 || index < 0) {
        log(QStringLiteral("Непідтримуваний формат проєкту B210."));
        return;
    }
    projectPath_ = folder;
    variant_->setCurrentIndex(index);
    variant_->setEnabled(false);
}

void B210LabPanel::startBuild(bool create) {
    if (!prepareBuild_()) return;
    const QDir recipe(recipe_->text().trimmed());
    const QString vendor = vendor_->text().trimmed();
    const QString entry = recipe.absoluteFilePath(create ? QStringLiteral("project.py") : QStringLiteral("build.sh"));
    const QString env = environment_->text().trimmed();
    if (recipe_->text().trimmed().isEmpty() || !QFileInfo(entry).isFile() ||
        vendor.isEmpty() || !QFileInfo(vendor).isDir() || (!env.isEmpty() && !QFileInfo(env).isFile())) {
        log(QStringLiteral("Вкажіть теку скриптів openXC7, вихідний проєкт виробника та наявний файл оточення, якщо потрібен."));
        return;
    }
    const QString parent = QFileDialog::getExistingDirectory(this, create
        ? QStringLiteral("Де створити проєкт B210") : QStringLiteral("Куди зберегти результат збірки"));
    if (parent.isEmpty()) return;
    QString destination = parent;
    if (create) {
        bool accepted = false;
        const QString name = QInputDialog::getText(this, QStringLiteral("Новий проєкт B210"),
            QStringLiteral("Назва теки"), QLineEdit::Normal, {}, &accepted).trimmed();
        if (!accepted) return;
        if (name.isEmpty() || name == QStringLiteral(".") || name == QStringLiteral("..") ||
            name.contains('/') || name.contains('\\')) {
            log(QStringLiteral("Вкажіть назву нової теки."));
            return;
        }
        destination = QDir(parent).filePath(name);
    }
    if (create && QFileInfo::exists(destination)) {
        log(QStringLiteral("Для проєкту потрібна нова тека."));
        return;
    }
    variantName_ = variant_->currentData().toString();
    outputPath_ = create ? destination : QDir(destination).filePath(variantName_ + QLatin1Char('-') +
                    QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz")));

    QString rtlPath;
    if (!create && passthrough_->isChecked()) {
        QString error;
        const auto rtl = pipeline_(&error);
        if (rtl.isEmpty()) { log(error); return; }
        rtlFile_->setFileTemplate(QDir(destination).filePath(QStringLiteral(".obrii-iq-XXXXXX")));
        if (!rtlFile_->open() || rtlFile_->write(rtl) != rtl.size() || !rtlFile_->flush()) {
            log(rtlFile_->errorString());
            rtlFile_->remove();
            return;
        }
        rtlPath = rtlFile_->fileName();
        rtlFile_->close();
    }
    QString shell = QStringLiteral(
        "set -euo pipefail\n"
        "path() { if command -v wslpath >/dev/null && [[ \"$1\" =~ ^[A-Za-z]: ]]; then wslpath -u \"$1\"; else printf '%s\\n' \"$1\"; fi; }\n"
        "entry=$(path \"$1\"); vendor=$(path \"$2\"); dest=$(path \"$4\")\n");
    if (create) {
        shell += QStringLiteral("exec python3 \"$entry\" create \"$vendor\" \"$3\" \"$dest\"\n");
    } else {
        shell += QStringLiteral(
            "variant=$3; env_file=$5; project=$6; rtl=$7\n"
            "if [[ -n \"$env_file\" ]]; then source \"$(path \"$env_file\")\"; fi\n"
            "args=(); if [[ -n \"$project\" ]]; then args+=(--project \"$(path \"$project\")\"); fi\n"
            "unset B210_IQ_PIPELINE\n"
            "if [[ -n \"$rtl\" ]]; then export B210_IQ_PIPELINE=$(path \"$rtl\"); fi\n"
            "mkdir -- \"$dest\"\n"
            "tmp=$(mktemp -d -t obrii-b210.XXXXXXXX)\n"
            "printf 'Linux build directory: %s\\n' \"$tmp/out\"\n"
            "bash \"$entry\" \"$vendor\" \"$variant\" \"$tmp/out\" \"${args[@]}\"\n"
            "cp -- \"$tmp/out/$variant.bin\" \"$tmp/out/$variant.bit\" \"$tmp/out/$variant.bin.sha256\" \"$tmp/out/route.json\" \"$tmp/out/synth.log\" \"$tmp/out/route.log\" \"$tmp/out/assemble.log\" \"$dest/\"\n"
            "if [[ -f \"$tmp/out/project-sources.json\" ]]; then cp -- \"$tmp/out/project-sources.json\" \"$dest/\"; fi\n");
    }
    QStringList arguments{QStringLiteral("-c"), shell, QStringLiteral("obrii-b210"),
                          entry, vendor, variantName_, outputPath_, env, projectPath_, rtlPath};
#ifdef Q_OS_WIN
    arguments.prepend(QStringLiteral("bash"));
    arguments.prepend(QStringLiteral("--exec"));
    const QString program = QStringLiteral("wsl.exe");
#else
    const QString program = QStringLiteral("bash");
#endif
    start(create ? Job::Create : Job::Build, program, arguments);
}

void B210LabPanel::startTest() {
    const QString host = host_->text().trimmed();
    static const QRegularExpression hostPattern(QStringLiteral("^[A-Za-z0-9_][A-Za-z0-9_.@:-]*$"));
    const QString cli = cli_->text().trimmed(), baseline = baseline_->text().trimmed();
    const QString library = library_->text().trimmed();
    if ((!host.isEmpty() && !hostPattern.match(host).hasMatch()) ||
        !cli.startsWith('/') || !baseline.startsWith('/') ||
        (!library.isEmpty() && !library.startsWith('/'))) {
        log(QStringLiteral("Вкажіть адресу або псевдонім SSH та абсолютні шляхи на Raspberry."));
        return;
    }
#ifdef Q_OS_WIN
    if (host.isEmpty()) {
        log(QStringLiteral("Вкажіть SSH-адресу Raspberry. Локальна перевірка UHD працює в Linux."));
        return;
    }
#endif
    QFile runner(QStringLiteral(":/fpga/b210_fpga_check.py"));
    if (!runner.open(QIODevice::ReadOnly)) { log(runner.errorString()); return; }
    checkScript_ = runner.readAll();
    image_.setFileName(candidate_->text().trimmed());
    if (!image_.open(QIODevice::ReadOnly) || image_.size() <= 0 || image_.size() > 32 * 1024 * 1024) {
        log(QStringLiteral("Оберіть сумісний тестовий .bin (до 32 МіБ)."));
        image_.close();
        return;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&image_) || !image_.seek(0)) {
        log(QStringLiteral("Не вдалося прочитати тестовий образ."));
        image_.close();
        return;
    }
    const QString bootstrap = QString::fromLatin1(R"PY(
import shutil, sys, tempfile
from pathlib import Path
stream = sys.stdin.buffer
size = int(stream.read(8))
source = stream.read(size)
if len(source) != size:
    raise SystemExit("Incomplete runner")
with tempfile.TemporaryDirectory(prefix="obrii-b210-") as folder:
    candidate = Path(folder) / "candidate.bin"
    with candidate.open("wb") as image:
        shutil.copyfileobj(stream, image)
    sys.argv += ["--candidate", str(candidate)]
    exec(compile(source, "<obrii-b210>", "exec"), {"__name__": "__main__"})
)PY");
    QStringList runnerArgs{QStringLiteral("--cli"), cli, QStringLiteral("--baseline"), baseline,
                           QStringLiteral("--expected-candidate-sha256"), QString::fromLatin1(hash.result().toHex()),
                           QStringLiteral("--rate"), QStringLiteral("30720000"),
                           QStringLiteral("--center"), QString::number(qRound64(center_->value() * 1000000.0)),
                           QStringLiteral("--seconds"), QString::number(seconds_->value())};
    if (!library.isEmpty()) runnerArgs << QStringLiteral("--library") << library;
    const QString imagesDir = QFileInfo(cli).path() + QStringLiteral("/uhd-images");
    if (gnss_->isChecked()) runnerArgs << QStringLiteral("--gnss-baud") << QStringLiteral("38400");
    if (pps_->isChecked()) runnerArgs << QStringLiteral("--check-pps");
    if (host.isEmpty()) {
        if (QFileInfo(imagesDir).isDir()) runnerArgs << QStringLiteral("--images-dir") << imagesDir;
        runnerArgs.prepend(bootstrap);
        runnerArgs.prepend(QStringLiteral("-c"));
        start(Job::Test, QStringLiteral("python3"), runnerArgs);
        return;
    }
    const QString script = QStringLiteral("if [ -d %1 ]; then export UHD_IMAGES_DIR=%1; fi; exec python3 -c ")
                               .arg(shellArgument(imagesDir));
    QString command = script + shellArgument(bootstrap);
    for (const QString &arg : runnerArgs) command += QLatin1Char(' ') + shellArgument(arg);
    start(Job::Test, QStringLiteral("ssh"), {QStringLiteral("-T"), QStringLiteral("-o"), QStringLiteral("BatchMode=yes"),
          QStringLiteral("-o"), QStringLiteral("StrictHostKeyChecking=yes"), QStringLiteral("-o"),
          QStringLiteral("ConnectTimeout=10"), host, command});
}

void B210LabPanel::sendImage() {
    if (!image_.isOpen() || process_.state() != QProcess::Running) return;
    while (process_.bytesToWrite() < 256 * 1024 && !image_.atEnd()) {
        const QByteArray block = image_.read(65536);
        if (block.isEmpty() || process_.write(block) != block.size()) {
            log(QStringLiteral("Не вдалося передати тестовий образ."));
            image_.close();
            process_.closeWriteChannel();
            return;
        }
    }
    if (image_.atEnd() || image_.error() != QFileDevice::NoError) {
        image_.close();
        process_.closeWriteChannel();
    }
}

void B210LabPanel::start(Job job, const QString &program, const QStringList &arguments) {
    log_->clear();
    result_.clear();
    job_ = job;
    setBusy(true);
    process_.start(program, arguments);
}

void B210LabPanel::finish(int exitCode, QProcess::ExitStatus status) {
    image_.close();
    if (!rtlFile_->fileName().isEmpty()) rtlFile_->remove();
    if (job_ == Job::Test) {
        result_ += process_.readAllStandardOutput();
        const QJsonDocument report = QJsonDocument::fromJson(result_);
        if (report.isObject()) {
            const auto object = report.object();
            const auto comparison = object.value(QStringLiteral("comparison")).toString();
            if (comparison == QStringLiteral("clean"))
                log(QStringLiteral("Обидва RX-прогони без пропусків IQ за лічильниками UHD."));
            else if (comparison == QStringLiteral("partial"))
                log(QStringLiteral("Приймання відбулося. Для повного висновку бракує підсумкових лічильників."));
            if (object.value(QStringLiteral("restored")).toBool())
                log(QStringLiteral("Еталонний образ відновлено."));
        }
        log(QString::fromUtf8(report.isObject() ? report.toJson() : result_));
    }
    setBusy(false);
    log(QStringLiteral("Код завершення: %1").arg(status == QProcess::NormalExit ? exitCode : -1));
    if (exitCode != 0 || status != QProcess::NormalExit) return;
    if (job_ == Job::Create) openProject_(outputPath_);
    if (job_ == Job::Build) candidate_->setText(QDir(outputPath_).filePath(variantName_ + QStringLiteral(".bin")));
}

void B210LabPanel::setBusy(bool active) {
    create_->setEnabled(!active);
    build_->setEnabled(!active);
    test_->setEnabled(!active);
}

void B210LabPanel::log(const QString &line) {
    if (!line.trimmed().isEmpty()) log_->appendPlainText(line.trimmed());
}

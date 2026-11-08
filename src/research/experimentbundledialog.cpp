#include "experimentbundledialog.h"
#include "experimentbundle.h"
#include "spectrumframereplaydialog.h"
#include "appsettingsutils.h"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStyle>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <atomic>
#include <algorithm>
#include <cmath>

struct ExperimentBundleDialog::Impl {
    struct Job {
        std::atomic_bool cancel{false};
        std::atomic_int progress{0};
        bool success = false;
        QString error;
        ExperimentBundle::Contents contents;
        QJsonObject header;
        QString manifestPath;
        SpectrumFrameRecording recording;
    };
    ExperimentBundleDialog *owner;
    Snapshot snapshot;
    ApplyWorkspace applyWorkspace, applySession;
    Translator translator;
    bool importing, uk, busy = false;
    QLineEdit *source, *destination = nullptr;
    QCheckBox *completed = nullptr;
    QPlainTextEdit *preview;
    QProgressBar *progress;
    QSpinBox *memoryMiB = nullptr;
    QLabel *status;
    QPushButton *action, *close, *replay = nullptr, *workspace = nullptr, *session = nullptr;
    std::shared_ptr<Job> job, verified;

    QString text(const char *key, const char *en, const char *ua) const {
        const QString fallback = QString::fromUtf8(uk ? ua : en);
        return translator ? translator(QString::fromLatin1(key), fallback) : fallback;
    }
    void setBusy(bool value) {
        busy = value;
        source->setEnabled(!value);
        if (destination) destination->setEnabled(!value);
        if (completed) completed->setEnabled(!value);
        if (memoryMiB) memoryMiB->setEnabled(!value);
        action->setEnabled(!value);
        if (replay) replay->setEnabled(!value && bool(verified));
        if (workspace) workspace->setEnabled(!value && verified && applyWorkspace &&
            !verified->contents.context.value("workspace").toObject().isEmpty());
        if (session) session->setEnabled(!value && verified && applySession &&
            verified->contents.context.value("session").isObject());
        close->setText(value ? text("cancel", "Cancel", "Скасувати") : text("close", "Close", "Закрити"));
    }
    void showPreview() {
        const auto &c = verified->contents;
        const auto recorded = verified->header.value("scientificMetadata").toObject().value("experimentContext").toObject();
        QString summary = text("experiment_verified", "Verified experiment", "Перевірений експеримент") + "\n";
        summary += QFileInfo(c.recordingPath).fileName() + "\n";
        summary += recorded.isEmpty()
            ? text("experiment_fallback", "Export-time context: not the original acquisition settings. Recording header takes precedence.",
                   "Контекст часу експорту: це не початкові налаштування запису. Заголовок запису має пріоритет.")
            : text("experiment_recorded_context", "Context captured at recording start.", "Контекст збережено на початку запису.");
        summary += "\n" + text("experiment_limits", "Stored spectrum values are unchanged; calibration is provenance only. Workspace restore is layout only, not engine reproduction.",
            "Значення спектра не змінюються; калібрування є лише метаданими. Відновлення робочої області повертає схему, а не відтворює роботу рушія.");
        const auto s = c.context.value("session").toObject();
        summary += QStringLiteral("\n%1: %2\n%3: %4\n%5\n\n")
            .arg(text("name", "Name", "Назва"), s.value("name").toString(),
                 text("experiment_snapshots", "Snapshots", "Знімки"), QString::number(s.value("snapshots").toArray().size()),
                 s.value("notes").toString());
        if (s.value("omittedSnapshotCount").toDouble() > 0)
            summary += text("experiment_subset", "Latest %1 of %2 snapshots; %3 earlier snapshots omitted.",
                "Останні %1 із %2 знімків; пропущено %3 попередніх знімків.")
                .arg(s.value("snapshots").toArray().size()).arg(s.value("totalSnapshotCount").toDouble(), 0, 'f', 0)
                .arg(s.value("omittedSnapshotCount").toDouble(), 0, 'f', 0) + "\n";
        summary += QString::fromUtf8(QJsonDocument(c.manifest).toJson(QJsonDocument::Indented)).left(131072);
        preview->setPlainText(summary);
    }
    void run(bool prepareReplay = false) {
        if (busy) return;
        if (prepareReplay && !verified) return;
        const QString input = source->text().trimmed(), output = destination ? destination->text().trimmed() : QString();
        if (!prepareReplay && (input.isEmpty() || (!importing &&
            (!completed->isChecked() || output.isEmpty() || QFileInfo::exists(output))))) {
            status->setText(text("experiment_choose_paths", "Choose a completed recording and a new, nonexistent destination folder.",
                "Оберіть завершений запис і нову папку призначення, якої ще не існує."));
            return;
        }
        QJsonObject context;
        if (!importing && !prepareReplay) {
            if (QMessageBox::warning(owner, owner->windowTitle(), text("experiment_privacy_confirm",
                "The bundle includes received IQ content, available sidecars, session name and notes. Review them before sharing. Export?",
                "Пакет містить прийнятий IQ-вміст, доступні супровідні файли, назву сесії та нотатки. Перевірте їх перед поширенням. Експортувати?"),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
            context = snapshot ? snapshot() : QJsonObject();
            const auto session = context.value("session").toObject();
            if (session.value("omittedSnapshotCount").toDouble() > 0 && QMessageBox::question(owner, owner->windowTitle(),
                text("experiment_subset_confirm", "Only the latest 4096 of %1 snapshots will be exported. Continue?", "Буде експортовано лише останні 4096 із %1 знімків. Продовжити?")
                    .arg(session.value("totalSnapshotCount").toDouble(), 0, 'f', 0), QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
        }
        if (!prepareReplay) { verified.reset(); preview->clear(); }
        job = std::make_shared<Job>();
        const auto task = job, previous = verified;
        const bool reading = importing;
        const int replayMiB = memoryMiB ? memoryMiB->value() : 128;
        setBusy(true); progress->setValue(0);
        status->setText(text("experiment_working", "Working...", "Обробка..."));
        auto *worker = QThread::create([task, previous, prepareReplay, reading, input, output, context, replayMiB]() mutable {
            try {
                const auto report = [task](qint64 done, qint64 total) {
                    task->progress.store(total > 0 ? int(std::clamp(100.0L*done/total, 0.0L, 100.0L)) : 0);
                };
                if (prepareReplay) {
                    if (!ExperimentBundle::read(previous->manifestPath, task->contents, &task->error, &task->cancel, report)) return;
                    if (task->contents.manifest != previous->contents.manifest || task->contents.context != previous->contents.context ||
                        task->contents.recordingPath != previous->contents.recordingPath) {
                        task->error = "Bundle changed; verify again and review the new preview."; return;
                    }
                    SpectrumFrameReader reader;
                    if (!reader.open(task->contents.recordingPath, &task->error)) return;
                    task->recording.metadata = reader.metadata();
                    std::size_t values = 0;
                    while (!reader.atEnd()) {
                        if (task->cancel.load()) return;
                        SpectrumFrameRecord frame;
                        if (!reader.readNext(frame, &task->error)) return;
                        if (frame.elapsedMs < 0 || frame.elapsedMs > 3155760000000LL ||
                            (!task->recording.frames.empty() && frame.elapsedMs < task->recording.frames.back().elapsedMs)) {
                            task->error = "Unsupported non-monotonic or out-of-range replay timeline"; return;
                        }
                        values += frame.magnitudes.size();
                        // Explicit viewer bounds; the verified bundle itself is never truncated.
                        if (values > std::size_t(replayMiB)*1024U*1024U/sizeof(float) || task->recording.frames.size() >= 65536) {
                            task->error = QString("Replay exceeds %1 MiB / 65536 frames; increase the replay budget or use a smaller recording. No frames were truncated.").arg(replayMiB);
                            return;
                        }
                        const auto width = task->recording.frames.empty() ? frame.magnitudes.size() : task->recording.frames.front().magnitudes.size();
                        if (frame.magnitudes.empty() || width * (task->recording.frames.size()+1) > std::size_t(replayMiB)*1024U*1024U/sizeof(float)) {
                            task->error = "Replay raster exceeds the selected memory budget. Increase the budget or use a smaller recording; no frames were truncated."; return;
                        }
                        for (float v : frame.magnitudes) if (!std::isfinite(v)) { task->error = "Non-finite spectrum value"; return; }
                        task->recording.frames.push_back(std::move(frame));
                        report(reader.position(), reader.size());
                    }
                    task->success = !task->recording.frames.empty();
                    if (!task->success) task->error = "Recording has no spectrum frames";
                } else if (reading) {
                    task->manifestPath = input;
                    task->success = ExperimentBundle::read(input, task->contents, &task->error, &task->cancel, report);
                    if (task->success) {
                        SpectrumFrameReader reader;
                        task->success = reader.open(task->contents.recordingPath, &task->error);
                        task->header = reader.metadata();
                    }
                } else {
                    SpectrumFrameReader reader;
                    if (!reader.open(input, &task->error)) return;
                    const auto recorded = reader.metadata().value("scientificMetadata").toObject().value("experimentContext").toObject();
                    if (!recorded.isEmpty()) {
                        const auto session = context.value("session");
                        context = recorded;
                        context["session"] = session;
                    }
                    const auto sourceSession = context.value("session").toObject();
                    const auto safeSession = ExperimentBundle::sanitizeContext(context).value("session").toObject();
                    if (safeSession.value("snapshots").toArray().size() != sourceSession.value("snapshots").toArray().size() ||
                        (sourceSession.value("omittedSnapshotCount").toDouble() > 0 &&
                         safeSession.value("omittedSnapshotCount") != sourceSession.value("omittedSnapshotCount"))) {
                        task->error = "Session data cannot be exported without losing snapshot/subset information."; return;
                    }
                    task->success = ExperimentBundle::write(output, input, context, &task->error, &task->cancel, report);
                }
            } catch (const std::exception &e) { task->error = QString::fromUtf8(e.what()); }
        });
        QObject::connect(worker, &QThread::finished, owner, [this, task, prepareReplay, output] {
            setBusy(false);
            if (task->cancel.load()) { status->setText(text("experiment_cancelled", "Cancelled", "Скасовано")); return; }
            if (!task->success) { status->setText(task->error); return; }
            progress->setValue(100);
            if (prepareReplay) {
                auto *viewer = new SpectrumFrameReplayDialog(owner->parentWidget(), true);
                QString error;
                bool loaded = false;
                try {
                    QStringList verifiedAssetPaths;
                    const QDir recordingDirectory = QFileInfo(task->contents.recordingPath).absoluteDir();
                    for (const auto &entry : task->contents.manifest.value("files").toArray()) {
                        verifiedAssetPaths.append(recordingDirectory.absoluteFilePath(entry.toObject().value("path").toString()));
                    }
                    loaded = viewer->loadExperimentRecording(task->contents.recordingPath,
                        std::move(task->recording), task->contents.context, &error, verifiedAssetPaths);
                }
                catch (const std::exception &e) { error = QString::fromUtf8(e.what()); }
                if (!loaded) {
                    delete viewer; status->setText(error); return;
                }
                viewer->setAttribute(Qt::WA_DeleteOnClose); viewer->show();
                status->setText(text("experiment_replay_paused", "Replay opened paused", "Відтворення відкрито на паузі"));
            } else if (importing) {
                verified = task; showPreview(); setBusy(false);
                status->setText(text("experiment_verified", "Verified experiment", "Перевірений експеримент"));
            } else status->setText(text("experiment_exported", "Export complete", "Експорт завершено") + ": " + output);
        });
        QObject::connect(owner, &QObject::destroyed, worker, [task] { task->cancel.store(true); });
        QObject::connect(qApp, &QCoreApplication::aboutToQuit, worker, [task, worker] { task->cancel.store(true); worker->wait(); });
        QObject::connect(worker, &QThread::finished, worker, &QObject::deleteLater);
        worker->start(QThread::LowPriority);
    }
};

ExperimentBundleDialog::ExperimentBundleDialog(bool importing, Snapshot snapshot, ApplyWorkspace applyWorkspace,
    ApplyWorkspace applySession, Translator translator, QWidget *parent) : QDialog(parent), impl(std::make_unique<Impl>()) {
    auto &p = *impl;
    p.owner = this; p.importing = importing; p.snapshot = std::move(snapshot); p.applyWorkspace = std::move(applyWorkspace);
    p.applySession = std::move(applySession); p.translator = std::move(translator);
    p.uk = QSettings(persistentSettingsFilePath(), QSettings::IniFormat).value("ui/language", "en").toString().startsWith("uk");
    setWindowTitle(importing ? p.text("experiment_open", "Open experiment", "Відкрити експеримент")
                            : p.text("experiment_export", "Export experiment", "Експортувати експеримент"));
    resize(720, 520);
    auto *root = new QVBoxLayout(this);
    auto *form = new QFormLayout(); root->addLayout(form);
    const auto pathRow = [&](QLineEdit *&field, const QString &label, bool folder) {
        auto *row = new QWidget(this); auto *layout = new QHBoxLayout(row); layout->setContentsMargins(0, 0, 0, 0);
        field = new QLineEdit(row); auto *browse = new QPushButton(style()->standardIcon(QStyle::SP_DirOpenIcon), {}, row);
        browse->setToolTip(label); layout->addWidget(field, 1); layout->addWidget(browse);
        auto *edit = field;
        connect(browse, &QPushButton::clicked, this, [this, edit, folder] {
            if (impl->busy) return;
            const QString path = folder ? QFileDialog::getSaveFileName(this,
                impl->text("experiment_new_folder", "New destination folder (must not exist)", "Нова папка призначення (не має існувати)"), {}, {}, nullptr, QFileDialog::DontConfirmOverwrite)
                : QFileDialog::getOpenFileName(this, windowTitle(), {}, impl->importing ? "Experiment manifest (*.json)" : "Spectrum (*.fbspec)");
            if (!path.isEmpty()) edit->setText(path);
        });
        form->addRow(label, row);
    };
    pathRow(p.source, importing ? p.text("experiment_manifest", "Manifest", "Маніфест") : p.text("experiment_recording", "Completed .fbspec", "Завершений .fbspec"), false);
    if (!importing) {
        pathRow(p.destination, p.text("experiment_destination", "New folder", "Нова папка"), true);
        p.completed = new QCheckBox(p.text("experiment_completed", "Recording and linked IQ files are closed and complete", "Запис і пов'язані IQ-файли закрито та завершено"), this);
        root->addWidget(p.completed);
    }
    auto *warning = new QLabel(p.text("experiment_privacy", "IQ contains received content. Session names and notes are included. Review before sharing.",
        "IQ містить прийнятий вміст. Назви сесій та нотатки включено. Перевірте їх перед поширенням."), this);
    warning->setWordWrap(true); root->addWidget(warning);
    p.preview = new QPlainTextEdit(this); p.preview->setReadOnly(true); root->addWidget(p.preview, 1);
    p.progress = new QProgressBar(this); p.progress->setRange(0, 100); root->addWidget(p.progress);
    p.status = new QLabel(this); p.status->setWordWrap(true); p.status->setTextFormat(Qt::PlainText); root->addWidget(p.status);
    auto *buttons = new QHBoxLayout(); root->addLayout(buttons);
    p.action = new QPushButton(importing ? p.text("experiment_verify", "Verify bundle", "Перевірити пакет") : p.text("experiment_export", "Export experiment", "Експортувати експеримент"), this);
    buttons->addWidget(p.action); connect(p.action, &QPushButton::clicked, this, [this] { impl->run(); });
    if (importing) {
        p.memoryMiB = new QSpinBox(this); p.memoryMiB->setRange(32, 512); p.memoryMiB->setValue(128); p.memoryMiB->setSuffix(" MiB");
        form->addRow(p.text("experiment_memory", "Replay data budget (65536 frames maximum)", "Пам'ять даних відтворення (до 65536 кадрів)"), p.memoryMiB);
        auto *restores = new QHBoxLayout(); root->insertLayout(root->count()-1, restores);
        p.replay = new QPushButton(p.text("experiment_replay", "Open paused replay", "Відкрити на паузі"), this);
        p.workspace = new QPushButton(p.text("experiment_workspace", "Restore DSP workspace", "Відновити DSP-схему"), this);
        p.session = new QPushButton(p.text("experiment_session", "Restore session", "Відновити сесію"), this);
        restores->addWidget(p.replay); restores->addWidget(p.session); restores->addWidget(p.workspace);
        connect(p.replay, &QPushButton::clicked, this, [this] { impl->run(true); });
        const auto restore = [this](bool workspace) {
            auto &p = *impl;
            if (!p.verified || p.busy) return;
            const QString prompt = workspace ? p.text("experiment_restore_workspace_confirm", "Replace the DSP layout after saving a backup? Reception must be stopped. No RF settings will be applied.",
                "Замінити DSP-схему після збереження резервної копії? Приймання має бути зупинено. RF-налаштування не застосовуватимуться.")
                : p.text("experiment_restore_session_confirm", "Replace session rows, name and notes? The imported session stays paused; the live cursor is unchanged.",
                "Замінити рядки сесії, назву та нотатки? Імпортована сесія залишиться на паузі; поточний курсор не зміниться.");
            if (QMessageBox::question(this, windowTitle(), prompt, QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
            QString error;
            const auto &apply = workspace ? p.applyWorkspace : p.applySession;
            if (apply && apply(p.verified->contents.context, &error)) p.status->setText(p.text("experiment_restored", "Restored", "Відновлено"));
            else p.status->setText(error);
        };
        connect(p.workspace, &QPushButton::clicked, this, [restore] { restore(true); });
        connect(p.session, &QPushButton::clicked, this, [restore] { restore(false); });
        connect(p.source, &QLineEdit::textChanged, this, [this] { impl->verified.reset(); impl->preview->clear(); impl->setBusy(false); });
    }
    p.close = new QPushButton(this); buttons->addStretch(); buttons->addWidget(p.close);
    connect(p.close, &QPushButton::clicked, this, &ExperimentBundleDialog::reject);
    auto *timer = new QTimer(this); timer->setInterval(100);
    connect(timer, &QTimer::timeout, this, [this] { if (impl->busy && impl->job) impl->progress->setValue(impl->job->progress.load()); });
    timer->start(); p.setBusy(false);
}
ExperimentBundleDialog::~ExperimentBundleDialog() { if (impl->job) impl->job->cancel.store(true); }
void ExperimentBundleDialog::reject() {
    if (impl->busy) { impl->job->cancel.store(true); impl->status->setText(impl->text("experiment_cancelling", "Cancelling...", "Скасування...")); return; }
    QDialog::reject();
}
void ExperimentBundleDialog::closeEvent(QCloseEvent *event) {
    if (impl->busy) { reject(); event->ignore(); return; }
    QDialog::closeEvent(event);
}

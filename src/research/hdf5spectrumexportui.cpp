#include "hdf5spectrumexportui.h"
#include "hdf5spectrumexport.h"
#include <QApplication>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QPointer>
#include <QProgressDialog>
#include <QThread>
#include <QTimer>
#include <memory>

void showSpectrumHdf5Export(QWidget *parent, const std::function<QString(const QString &, const QString &)> &translate) {
    const auto text = [&](const char *key, const char *fallback) { return translate ? translate(key, fallback) : QString::fromUtf8(fallback); };
    if (!hdf5SpectrumExportBuilt()) {
        QMessageBox::information(parent, "HDF5", text("hdf5_not_built", "HDF5 export requires libhdf5-dev when building Linux. Windows packages include the runtime."));
        return;
    }
    const QString input = QFileDialog::getOpenFileName(parent, text("hdf5_select_input", "Spectrum recording to export"), {}, "Obrii spectrum (*.fbspec)");
    if (input.isEmpty()) return;
    QString output = QFileDialog::getSaveFileName(parent, text("hdf5_select_output", "Export spectrum series to HDF5"),
        QFileInfo(input).absolutePath()+"/"+QFileInfo(input).completeBaseName()+".h5", "HDF5 (*.h5 *.hdf5)");
    if (output.isEmpty()) return;
    if (!output.endsWith(".h5", Qt::CaseInsensitive) && !output.endsWith(".hdf5", Qt::CaseInsensitive)) output += ".h5";
    struct Job { std::atomic_bool cancel{false}; std::atomic_int progress{0}; bool success = false; QString error; };
    auto job = std::make_shared<Job>();
    auto *dialog = new QProgressDialog(text("hdf5_exporting", "Exporting spectrum frames to HDF5..."), text("cancel", "Cancel"), 0, 100, parent);
    dialog->setAttribute(Qt::WA_DeleteOnClose); dialog->setWindowModality(Qt::NonModal); dialog->setAutoClose(false);
    dialog->setMinimumDuration(0); dialog->setValue(0); dialog->show();
    const QPointer<QProgressDialog> guard(dialog);
    auto *worker = QThread::create([job, input, output]() {
        job->success = exportSpectrumHdf5(input, output, &job->error, &job->cancel, [job](int value) { job->progress.store(value); });
    });
    QObject::connect(dialog, &QProgressDialog::canceled, dialog, [job]() { job->cancel.store(true); });
    QObject::connect(dialog, &QObject::destroyed, worker, [job]() { job->cancel.store(true); });
    auto *timer = new QTimer(dialog); timer->setInterval(100);
    QObject::connect(timer, &QTimer::timeout, dialog, [guard, job]() { if (guard) guard->setValue(job->progress.load()); }); timer->start();
    QObject::connect(qApp, &QCoreApplication::aboutToQuit, worker, [job, worker]() { job->cancel.store(true); worker->wait(); });
    QObject::connect(worker, &QThread::finished, parent, [guard, job, output, translate, parent]() {
        const bool cancelled = job->cancel.load();
        if (guard) { guard->blockSignals(true); guard->close(); }
        if (cancelled) return;
        const QString title = translate ? translate("hdf5_export", "HDF5 export") : "HDF5 export";
        if (job->success) QMessageBox::information(parent, title, output);
        else QMessageBox::warning(parent, title, job->error);
    });
    QObject::connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start(QThread::LowPriority);
}

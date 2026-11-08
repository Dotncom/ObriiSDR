#pragma once

#include <QJsonObject>
#include <QString>
#include <atomic>
#include <functional>

namespace ExperimentBundle {

struct Contents {
    QString recordingPath;
    QJsonObject context;
    QJsonObject manifest;
};

// Call on a worker with a completed recording. Progress runs on that worker;
// callbacks must not throw. No receiver, playback, or settings are activated.
// Destination must not already exist. A failed export remains a partial folder
// without a usable manifest and is never overwritten by a subsequent export.
bool write(const QString &destinationDirectory, const QString &recordingPath,
           const QJsonObject &context, QString *error,
           const std::atomic_bool *cancel = nullptr,
           std::function<void(qint64, qint64)> progress = {});

// Verifies all declared files before returning any contents. Returned paths are
// local absolute paths; all on-disk manifest paths are fixed relative basenames.
bool read(const QString &manifestPath, Contents &contents, QString *error,
          const std::atomic_bool *cancel = nullptr,
          std::function<void(qint64, qint64)> progress = {});

// Strict allowlist, including disarmed DSP workspace and explicitly selected
// session/notes. Never read preferences or private data from the filesystem.
QJsonObject sanitizeContext(QJsonObject context);

} // namespace ExperimentBundle

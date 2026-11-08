#ifndef PUBLICDATAFETCH_H
#define PUBLICDATAFETCH_H
#include <QByteArray>
#include <QUrl>
#include <atomic>
// Error messages never include the URL, which may contain credentials or coordinates.
QByteArray fetchPublicData(const QUrl &url, const std::atomic_bool &cancel, QString *error,
                         int *httpStatus = nullptr);
#endif

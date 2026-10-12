#ifndef PUBLICDATAFETCH_H
#define PUBLICDATAFETCH_H
#include <QByteArray>
#include <QUrl>
#include <atomic>
QByteArray fetchPublicData(const QUrl &url, const std::atomic_bool &cancel, QString *error);
#endif

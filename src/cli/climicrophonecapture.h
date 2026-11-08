#pragma once
#include <QJsonArray>
#include <QObject>
#include <QByteArray>
#include <QString>
#include <functional>
#include <memory>

class CliMicrophoneCapture : public QObject {
public:
    explicit CliMicrophoneCapture(QObject *parent = nullptr);
    ~CliMicrophoneCapture() override;
    static QJsonArray devices();
    bool start(const QString &device, int rate, QString *error);
    void stop();
    bool running() const;
    std::function<void(const QByteArray &)> onPcm;
    std::function<void(const QString &)> onError;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

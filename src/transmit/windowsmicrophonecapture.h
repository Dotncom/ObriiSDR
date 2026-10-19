#ifndef WINDOWSMICROPHONECAPTURE_H
#define WINDOWSMICROPHONECAPTURE_H

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>

#include <memory>

class WindowsMicrophoneCapture final : public QObject {
    Q_OBJECT

public:
    explicit WindowsMicrophoneCapture(QObject *parent = nullptr);
    ~WindowsMicrophoneCapture() override;

    static QStringList deviceNames();
    static bool setDeviceLevel(const QString &requestedDevice, float level,
                               QString *error = nullptr, QString *details = nullptr);
    bool start(const QString &requestedDevice, int sampleRate, float inputLevel,
               QString *error = nullptr);
    void stop();
    bool isRunning() const;
    QString backendName() const;

signals:
    void pcmReady(const QByteArray &pcm);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

#endif // WINDOWSMICROPHONECAPTURE_H

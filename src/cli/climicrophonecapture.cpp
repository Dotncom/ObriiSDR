#include "climicrophonecapture.h"
#include "windowsmicrophonecapture.h"
#include <QJsonObject>
#include <QTimer>
#include <QPointer>
#ifdef OBRII_CLI_QT_MICROPHONE
#include <QAudioDeviceInfo>
#include <QAudioInput>
#endif

struct CliMicrophoneCapture::Impl {
    std::unique_ptr<WindowsMicrophoneCapture> native;
#ifdef OBRII_CLI_QT_MICROPHONE
    std::unique_ptr<QAudioInput> qt;
    QPointer<QIODevice> input;
#endif
};
CliMicrophoneCapture::CliMicrophoneCapture(QObject *parent) : QObject(parent), impl(new Impl) {}
CliMicrophoneCapture::~CliMicrophoneCapture() { stop(); }
QJsonArray CliMicrophoneCapture::devices() {
    QJsonArray result;
#ifdef Q_OS_WIN
    for (const auto &name : WindowsMicrophoneCapture::deviceNames())
        result.append(QJsonObject{{"name", name}, {"backend", "windows-native"}});
#elif defined(OBRII_CLI_QT_MICROPHONE)
    const auto selected = QAudioDeviceInfo::defaultInputDevice();
    for (const auto &device : QAudioDeviceInfo::availableDevices(QAudio::AudioInput))
        result.append(QJsonObject{{"name", device.deviceName()}, {"default", device == selected},
            {"preferredRate", device.preferredFormat().sampleRate()}, {"backend", "qt-multimedia"}});
#endif
    return result;
}
bool CliMicrophoneCapture::start(const QString &device, int rate, QString *error) {
    stop();
    if (!device.isEmpty()) {
        bool found = false;
        for (const auto &entry : devices()) if (entry.toObject().value("name").toString() == device) found = true;
        if (!found) { if (error) *error = QStringLiteral("Selected microphone is not in the current device list"); return false; }
    }
#ifdef Q_OS_WIN
    impl->native.reset(new WindowsMicrophoneCapture);
    connect(impl->native.get(), &WindowsMicrophoneCapture::pcmReady, this, [this](const QByteArray &pcm) {
        if (onPcm) onPcm(pcm);
    });
    const bool okay = impl->native->start(device, rate, 1.0f, error, false);
    if (!okay) impl->native.reset();
    return okay;
#elif defined(OBRII_CLI_QT_MICROPHONE)
    auto selected = QAudioDeviceInfo::defaultInputDevice();
    for (const auto &candidate : QAudioDeviceInfo::availableDevices(QAudio::AudioInput))
        if (!device.isEmpty() && candidate.deviceName() == device) selected = candidate;
    QAudioFormat format;
    format.setCodec(QStringLiteral("audio/pcm")); format.setSampleRate(rate);
    format.setChannelCount(1); format.setSampleSize(16);
    format.setSampleType(QAudioFormat::SignedInt); format.setByteOrder(QAudioFormat::LittleEndian);
    if (selected.isNull() || !selected.isFormatSupported(format)) {
        if (error) *error = QStringLiteral("Microphone does not support requested mono PCM16 rate; choose a supported --rate");
        return false;
    }
    impl->qt.reset(new QAudioInput(selected, format));
    impl->qt->setBufferSize(rate * 2 / 5);
    connect(impl->qt.get(), &QAudioInput::stateChanged, this, [this](QAudio::State state) {
        if (state == QAudio::StoppedState && impl->qt && impl->qt->error() != QAudio::NoError)
            if (onError) onError(QStringLiteral("Microphone capture stopped with an audio-device error"));
    });
    impl->input = impl->qt->start();
    if (!impl->input) {
        stop(); if (error) *error = QStringLiteral("Cannot start microphone capture"); return false;
    }
    connect(impl->input, &QIODevice::readyRead, this, [this] {
        if (impl->input && onPcm) {
            const auto bytes = impl->input->read(65536);
            if (!bytes.isEmpty()) onPcm(bytes);
        }
    });
    return true;
#else
    Q_UNUSED(rate)
    if (error) *error = QStringLiteral("This CLI build has no microphone capture backend");
    return false;
#endif
}
void CliMicrophoneCapture::stop() {
    if (impl->native) { impl->native->disconnect(this); impl->native->stop(); impl->native.reset(); }
#ifdef OBRII_CLI_QT_MICROPHONE
    if (impl->qt) { impl->qt->disconnect(this); impl->qt->stop(); impl->qt.reset(); }
    impl->input.clear();
#endif
}
bool CliMicrophoneCapture::running() const {
#ifdef Q_OS_WIN
    return impl->native && impl->native->isRunning();
#elif defined(OBRII_CLI_QT_MICROPHONE)
    return impl->qt && impl->qt->state() != QAudio::StoppedState;
#else
    return false;
#endif
}

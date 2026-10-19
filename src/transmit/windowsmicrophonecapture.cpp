#include "windowsmicrophonecapture.h"

#include <QMetaObject>
#include <QPointer>
#include <QDebug>
#include <QSettings>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <propsys.h>
#include <functiondiscoverykeys_devpkey.h>
#endif

struct WindowsMicrophoneCapture::Impl {
#ifdef _WIN32
    struct CaptureBuffer {
        std::vector<char> storage;
        WAVEHDR header{};
        bool prepared = false;
    };

    WindowsMicrophoneCapture *owner = nullptr;
    HWAVEIN handle = nullptr;
    std::array<CaptureBuffer, 8> buffers;
    std::atomic<bool> running{false};
    std::thread wasapiThread;
    HANDLE wasapiStopEvent = nullptr;
    std::mutex startupMutex;
    std::condition_variable startupCondition;
    bool startupFinished = false;
    bool startupSucceeded = false;
    QString startupError;
    QString activeBackend;

    static QString friendlyName(IMMDevice *device) {
        if (!device) return {};
        IPropertyStore *properties = nullptr;
        if (FAILED(device->OpenPropertyStore(STGM_READ, &properties)) || !properties) {
            return {};
        }
        PROPVARIANT value;
        PropVariantInit(&value);
        QString name;
        if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &value)) &&
            value.vt == VT_LPWSTR && value.pwszVal) {
            name = QString::fromWCharArray(value.pwszVal);
        }
        PropVariantClear(&value);
        properties->Release();
        return name;
    }

    static bool deviceNamesMatch(const QString &requested, const QString &candidate) {
        const QString left = requested.simplified();
        const QString right = candidate.simplified();
        if (left.compare(right, Qt::CaseInsensitive) == 0) return true;
        if (left.startsWith(right, Qt::CaseInsensitive) ||
            right.startsWith(left, Qt::CaseInsensitive)) {
            return true;
        }
        const QString leftCompact = QString(left).remove(QLatin1Char(' '));
        const QString rightCompact = QString(right).remove(QLatin1Char(' '));
        return leftCompact.contains(rightCompact, Qt::CaseInsensitive) ||
               rightCompact.contains(leftCompact, Qt::CaseInsensitive);
    }

    static HRESULT findCaptureEndpoint(IMMDeviceEnumerator *enumerator,
                                       const QString &requestedDevice,
                                       IMMDevice **selectedDevice,
                                       QString *selectedName,
                                       bool *usedFallback) {
        if (!enumerator || !selectedDevice) return E_POINTER;
        *selectedDevice = nullptr;
        if (selectedName) selectedName->clear();
        if (usedFallback) *usedFallback = false;

        const bool wantsDefault = requestedDevice.isEmpty() ||
            requestedDevice.compare(QStringLiteral("Default Input Device"),
                                    Qt::CaseInsensitive) == 0;
        if (!wantsDefault) {
            IMMDeviceCollection *collection = nullptr;
            HRESULT result = enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE,
                                                             &collection);
            if (SUCCEEDED(result) && collection) {
                UINT count = 0;
                collection->GetCount(&count);
                for (UINT index = 0; index < count; ++index) {
                    IMMDevice *candidate = nullptr;
                    if (FAILED(collection->Item(index, &candidate)) || !candidate) continue;
                    const QString candidateName = friendlyName(candidate);
                    if (deviceNamesMatch(requestedDevice, candidateName)) {
                        *selectedDevice = candidate;
                        if (selectedName) *selectedName = candidateName;
                        collection->Release();
                        return S_OK;
                    }
                    candidate->Release();
                }
                collection->Release();
            }
        }

        if (usedFallback) *usedFallback = !wantsDefault;
        const HRESULT result = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole,
                                                                   selectedDevice);
        if (SUCCEEDED(result) && selectedName) *selectedName = friendlyName(*selectedDevice);
        return result;
    }

    static bool configureEndpoint(const QString &requestedDevice, float requestedLevel,
                                  QString *error, QString *details) {
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const bool uninitialize = SUCCEEDED(initialized);
        if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) {
            if (error) {
                *error = QStringLiteral("CoInitializeEx failed (0x%1)")
                             .arg(static_cast<quint32>(initialized), 8, 16,
                                  QLatin1Char('0'));
            }
            return false;
        }
        IMMDeviceEnumerator *enumerator = nullptr;
        IMMDevice *device = nullptr;
        IAudioEndpointVolume *endpoint = nullptr;
        QString endpointName;
        bool usedFallback = false;
        HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                          CLSCTX_INPROC_SERVER,
                                          __uuidof(IMMDeviceEnumerator),
                                          reinterpret_cast<void **>(&enumerator));
        if (SUCCEEDED(result)) {
            result = findCaptureEndpoint(enumerator, requestedDevice, &device,
                                         &endpointName, &usedFallback);
        }
        if (SUCCEEDED(result)) {
            result = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL,
                                      nullptr, reinterpret_cast<void **>(&endpoint));
        }

        BOOL muted = FALSE;
        float level = -1.0f;
        bool unmuted = false;
        bool levelChanged = false;
        if (SUCCEEDED(result)) {
            endpoint->GetMute(&muted);
            endpoint->GetMasterVolumeLevelScalar(&level);
            if (muted && requestedLevel > 0.0f) {
                unmuted = SUCCEEDED(endpoint->SetMute(FALSE, nullptr));
            }
            const float targetLevel = (std::clamp)(requestedLevel, 0.0f, 1.0f);
            levelChanged = SUCCEEDED(
                endpoint->SetMasterVolumeLevelScalar(targetLevel, nullptr));
            endpoint->GetMute(&muted);
            endpoint->GetMasterVolumeLevelScalar(&level);
        }

        if (endpoint) endpoint->Release();
        if (device) device->Release();
        if (enumerator) enumerator->Release();
        if (uninitialize) CoUninitialize();

        QSettings privacy(
            QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\microphone"),
            QSettings::NativeFormat);
        const QString consent = privacy.value(QStringLiteral("Value"),
                                              QStringLiteral("unknown")).toString();
        const QString report =
            QStringLiteral("endpoint=\"%1\" requested=\"%2\" fallback=%3 HRESULT=0x%4 mute=%5 volume=%6 privacy=%7 unmuted=%8 levelSet=%9")
            .arg(endpointName.isEmpty() ? QStringLiteral("unknown") : endpointName)
            .arg(requestedDevice.isEmpty() ? QStringLiteral("default") : requestedDevice)
            .arg(usedFallback ? QStringLiteral("yes") : QStringLiteral("no"))
            .arg(static_cast<quint32>(result), 8, 16, QLatin1Char('0'))
            .arg(muted ? QStringLiteral("yes") : QStringLiteral("no"))
            .arg(level, 0, 'f', 3)
            .arg(consent)
            .arg(unmuted ? QStringLiteral("yes") : QStringLiteral("no"))
            .arg(levelChanged ? QStringLiteral("yes") : QStringLiteral("no"));
        if (details) *details = report;
        if ((!levelChanged || FAILED(result)) && error) *error = report;
        else if (error) error->clear();
        return SUCCEEDED(result) && levelChanged;
    }

    static QString errorText(MMRESULT result) {
        wchar_t text[MAXERRORLENGTH]{};
        if (waveInGetErrorTextW(result, text, MAXERRORLENGTH) == MMSYSERR_NOERROR) {
            return QStringLiteral("%1 (WinMM code %2)")
                .arg(QString::fromWCharArray(text))
                .arg(result);
        }
        return QStringLiteral("WinMM error %1").arg(result);
    }

    static QString hresultText(const QString &operation, HRESULT result) {
        return QStringLiteral("%1 failed (HRESULT 0x%2)")
            .arg(operation)
            .arg(static_cast<quint32>(result), 8, 16, QLatin1Char('0'));
    }

    enum class WasapiEncoding {
        Pcm16,
        Pcm24,
        Pcm32,
        Float32,
        Unsupported
    };

    static WasapiEncoding wasapiEncoding(const WAVEFORMATEX *format) {
        if (!format) return WasapiEncoding::Unsupported;
        WORD tag = format->wFormatTag;
        GUID subFormat{};
        if (tag == WAVE_FORMAT_EXTENSIBLE &&
            format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
            const auto *extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(format);
            subFormat = extensible->SubFormat;
            if (IsEqualGUID(subFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
                tag = WAVE_FORMAT_IEEE_FLOAT;
            } else if (IsEqualGUID(subFormat, KSDATAFORMAT_SUBTYPE_PCM)) {
                tag = WAVE_FORMAT_PCM;
            }
        }
        if (tag == WAVE_FORMAT_IEEE_FLOAT && format->wBitsPerSample == 32) {
            return WasapiEncoding::Float32;
        }
        if (tag != WAVE_FORMAT_PCM) return WasapiEncoding::Unsupported;
        if (format->wBitsPerSample == 16) return WasapiEncoding::Pcm16;
        if (format->wBitsPerSample == 24) return WasapiEncoding::Pcm24;
        if (format->wBitsPerSample == 32) return WasapiEncoding::Pcm32;
        return WasapiEncoding::Unsupported;
    }

    static QString wasapiFormatText(const WAVEFORMATEX *format,
                                    WasapiEncoding encoding) {
        if (!format) return QStringLiteral("unknown");
        QString encodingName;
        switch (encoding) {
        case WasapiEncoding::Pcm16: encodingName = QStringLiteral("PCM16"); break;
        case WasapiEncoding::Pcm24: encodingName = QStringLiteral("PCM24"); break;
        case WasapiEncoding::Pcm32: encodingName = QStringLiteral("PCM32"); break;
        case WasapiEncoding::Float32: encodingName = QStringLiteral("float32"); break;
        default: encodingName = QStringLiteral("unsupported"); break;
        }
        return QStringLiteral("%1 Hz, %2 ch, %3, block %4")
            .arg(format->nSamplesPerSec)
            .arg(format->nChannels)
            .arg(encodingName)
            .arg(format->nBlockAlign);
    }

    static float readWasapiSample(const BYTE *sample, WasapiEncoding encoding) {
        if (!sample) return 0.0f;
        switch (encoding) {
        case WasapiEncoding::Pcm16: {
            qint16 value = 0;
            std::memcpy(&value, sample, sizeof(value));
            return static_cast<float>(value) / 32768.0f;
        }
        case WasapiEncoding::Pcm24: {
            qint32 value = static_cast<qint32>(sample[0]) |
                           (static_cast<qint32>(sample[1]) << 8) |
                           (static_cast<qint32>(sample[2]) << 16);
            if (value & 0x00800000) value |= static_cast<qint32>(0xff000000);
            return static_cast<float>(value) / 8388608.0f;
        }
        case WasapiEncoding::Pcm32: {
            qint32 value = 0;
            std::memcpy(&value, sample, sizeof(value));
            return static_cast<float>(static_cast<double>(value) / 2147483648.0);
        }
        case WasapiEncoding::Float32: {
            float value = 0.0f;
            std::memcpy(&value, sample, sizeof(value));
            return std::isfinite(value) ? (std::clamp)(value, -1.0f, 1.0f) : 0.0f;
        }
        default:
            return 0.0f;
        }
    }

    void deliverPcm(const QByteArray &pcm) {
        const QPointer<WindowsMicrophoneCapture> target(owner);
        QMetaObject::invokeMethod(
            owner,
            [target, pcm]() {
                if (target) emit target->pcmReady(pcm);
            },
            Qt::QueuedConnection);
    }

    void finishWasapiStartup(bool succeeded, const QString &message = QString()) {
        {
            std::lock_guard<std::mutex> lock(startupMutex);
            startupSucceeded = succeeded;
            startupError = message;
            startupFinished = true;
        }
        startupCondition.notify_one();
    }

    void runWasapi(const QString requestedDevice, int sampleRate) {
        HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool uninitialize = SUCCEEDED(result);
        IMMDeviceEnumerator *enumerator = nullptr;
        IMMDevice *device = nullptr;
        IAudioClient *audioClient = nullptr;
        IAudioCaptureClient *captureClient = nullptr;
        WAVEFORMATEX *mixFormat = nullptr;
        QString endpointName;
        bool usedFallback = false;

        auto fail = [this](const QString &message) {
            running.store(false, std::memory_order_release);
            finishWasapiStartup(false, message);
        };

        auto cleanup = [&]() {
            if (captureClient) captureClient->Release();
            if (audioClient) audioClient->Release();
            if (device) device->Release();
            if (enumerator) enumerator->Release();
            if (mixFormat) CoTaskMemFree(mixFormat);
            captureClient = nullptr;
            audioClient = nullptr;
            device = nullptr;
            enumerator = nullptr;
            mixFormat = nullptr;
            if (uninitialize) CoUninitialize();
        };

        auto failAt = [&](const QString &operation, HRESULT failure) {
            const QString message = hresultText(operation, failure);
            cleanup();
            fail(message);
        };

        if (FAILED(result) && result != RPC_E_CHANGED_MODE) {
            fail(hresultText(QStringLiteral("WASAPI CoInitializeEx"), result));
            return;
        }
        result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void **>(&enumerator));
        if (FAILED(result)) {
            failAt(QStringLiteral("WASAPI CoCreateInstance(MMDeviceEnumerator)"), result);
            return;
        }
        result = findCaptureEndpoint(enumerator, requestedDevice, &device,
                                     &endpointName, &usedFallback);
        if (FAILED(result)) {
            failAt(QStringLiteral("WASAPI find capture endpoint"), result);
            return;
        }
        result = device->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER, nullptr,
                                  reinterpret_cast<void **>(&audioClient));
        if (FAILED(result)) {
            failAt(QStringLiteral("WASAPI IMMDevice::Activate(IAudioClient)"), result);
            return;
        }
        result = audioClient->GetMixFormat(&mixFormat);
        if (FAILED(result)) {
            failAt(QStringLiteral("WASAPI IAudioClient::GetMixFormat"), result);
            return;
        }
        const WasapiEncoding encoding = wasapiEncoding(mixFormat);
        if (encoding == WasapiEncoding::Unsupported || mixFormat->nChannels == 0 ||
            mixFormat->nBlockAlign == 0 || mixFormat->nSamplesPerSec == 0) {
            const QString message = QStringLiteral("WASAPI mix format unsupported: %1")
                                        .arg(wasapiFormatText(mixFormat, encoding));
            cleanup();
            fail(message);
            return;
        }

        // Use the driver's native shared-mode format and convert in Obrii. This
        // avoids driver-specific failures in the Windows auto-conversion path.
        result = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0,
                                         1000000, 0, mixFormat, nullptr);
        if (FAILED(result)) {
            failAt(QStringLiteral("WASAPI IAudioClient::Initialize(native shared format)"), result);
            return;
        }
        result = audioClient->GetService(__uuidof(IAudioCaptureClient),
                                         reinterpret_cast<void **>(&captureClient));
        if (FAILED(result)) {
            failAt(QStringLiteral("WASAPI IAudioClient::GetService(IAudioCaptureClient)"), result);
            return;
        }
        UINT32 bufferFrames = 0;
        result = audioClient->GetBufferSize(&bufferFrames);
        if (FAILED(result)) {
            failAt(QStringLiteral("WASAPI IAudioClient::GetBufferSize"), result);
            return;
        }
        result = audioClient->Start();
        if (FAILED(result)) {
            failAt(QStringLiteral("WASAPI IAudioClient::Start"), result);
            return;
        }

        qInfo().noquote() << "[TX audio] WASAPI shared capture"
                          << "endpoint" << endpointName
                          << "requested" << requestedDevice
                          << "fallback" << usedFallback
                          << "nativeFormat" << wasapiFormatText(mixFormat, encoding)
                          << "outputFormat" << QStringLiteral("%1 Hz mono PCM16").arg(sampleRate)
                          << "bufferFrames" << bufferFrames;
        finishWasapiStartup(true);

        const int bytesPerChannel = mixFormat->nBlockAlign / mixFormat->nChannels;
        const double sourceFramesPerOutputFrame =
            static_cast<double>(mixFormat->nSamplesPerSec) /
            static_cast<double>((std::max)(1, sampleRate));
        std::vector<float> resampleBuffer;
        double resamplePosition = 0.0;
        while (running.load(std::memory_order_acquire)) {
            if (WaitForSingleObject(wasapiStopEvent, 5) == WAIT_OBJECT_0) break;

            UINT32 packetFrames = 0;
            while (running.load(std::memory_order_acquire) &&
                   SUCCEEDED(captureClient->GetNextPacketSize(&packetFrames)) &&
                   packetFrames > 0) {
                BYTE *data = nullptr;
                DWORD flags = 0;
                UINT64 devicePosition = 0;
                UINT64 qpcPosition = 0;
                result = captureClient->GetBuffer(&data, &packetFrames, &flags,
                                                  &devicePosition, &qpcPosition);
                if (FAILED(result)) break;
                const size_t previousFrames = resampleBuffer.size();
                resampleBuffer.resize(previousFrames + packetFrames, 0.0f);
                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && data) {
                    for (UINT32 frameIndex = 0; frameIndex < packetFrames; ++frameIndex) {
                        const BYTE *frame = data +
                            static_cast<size_t>(frameIndex) * mixFormat->nBlockAlign;
                        float sum = 0.0f;
                        for (WORD channel = 0; channel < mixFormat->nChannels; ++channel) {
                            sum += readWasapiSample(frame + channel * bytesPerChannel,
                                                   encoding);
                        }
                        resampleBuffer[previousFrames + frameIndex] =
                            sum / static_cast<float>(mixFormat->nChannels);
                    }
                }
                if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) {
                    qWarning() << "[TX audio] WASAPI capture discontinuity";
                }
                captureClient->ReleaseBuffer(packetFrames);

                QByteArray pcm;
                const size_t estimatedFrames = static_cast<size_t>(
                    static_cast<double>(resampleBuffer.size()) /
                    (std::max)(sourceFramesPerOutputFrame, 0.000001));
                pcm.reserve(static_cast<int>(estimatedFrames * sizeof(qint16)));
                while (resamplePosition + 1.0 <
                       static_cast<double>(resampleBuffer.size())) {
                    const size_t left = static_cast<size_t>(resamplePosition);
                    const double fraction = resamplePosition - static_cast<double>(left);
                    const float value = static_cast<float>(
                        resampleBuffer[left] * (1.0 - fraction) +
                        resampleBuffer[left + 1] * fraction);
                    const qint16 output = static_cast<qint16>(std::lround(
                        (std::clamp)(value, -1.0f, 1.0f) * 32767.0f));
                    pcm.append(reinterpret_cast<const char *>(&output), sizeof(output));
                    resamplePosition += sourceFramesPerOutputFrame;
                }
                const size_t consumed = (std::min)(
                    static_cast<size_t>(resamplePosition),
                    resampleBuffer.empty() ? size_t(0) : resampleBuffer.size() - 1);
                if (consumed > 0) {
                    resampleBuffer.erase(resampleBuffer.begin(),
                                         resampleBuffer.begin() + consumed);
                    resamplePosition -= static_cast<double>(consumed);
                }
                if (!pcm.isEmpty()) deliverPcm(pcm);
            }
        }

        audioClient->Stop();
        cleanup();
        running.store(false, std::memory_order_release);
    }

    bool startWasapi(const QString &requestedDevice, int sampleRate, QString *error) {
        wasapiStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!wasapiStopEvent) {
            if (error) *error = QStringLiteral("Could not create WASAPI stop event");
            if (wasapiStopEvent) CloseHandle(wasapiStopEvent);
            wasapiStopEvent = nullptr;
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(startupMutex);
            startupFinished = false;
            startupSucceeded = false;
            startupError.clear();
        }
        running.store(true, std::memory_order_release);
        wasapiThread = std::thread([this, requestedDevice, sampleRate]() {
            runWasapi(requestedDevice, sampleRate);
        });

        std::unique_lock<std::mutex> lock(startupMutex);
        const bool signalled = startupCondition.wait_for(
            lock, std::chrono::seconds(3), [this]() { return startupFinished; });
        const bool succeeded = signalled && startupSucceeded;
        const QString message = signalled ? startupError
                                          : QStringLiteral("WASAPI capture startup timed out");
        lock.unlock();
        if (succeeded) {
            activeBackend = QStringLiteral("WASAPI shared");
            return true;
        }

        running.store(false, std::memory_order_release);
        SetEvent(wasapiStopEvent);
        if (wasapiThread.joinable()) wasapiThread.join();
        CloseHandle(wasapiStopEvent);
        wasapiStopEvent = nullptr;
        if (error) *error = message;
        return false;
    }

    void stopWasapi() {
        if (!wasapiThread.joinable()) return;
        running.store(false, std::memory_order_release);
        if (wasapiStopEvent) SetEvent(wasapiStopEvent);
        wasapiThread.join();
        if (wasapiStopEvent) CloseHandle(wasapiStopEvent);
        wasapiStopEvent = nullptr;
    }

    static void CALLBACK callback(HWAVEIN input, UINT message, DWORD_PTR instance,
                                  DWORD_PTR parameter1, DWORD_PTR) {
        if (message != WIM_DATA || !instance || !parameter1) return;
        auto *self = reinterpret_cast<Impl *>(instance);
        auto *header = reinterpret_cast<WAVEHDR *>(parameter1);
        if (!self->running.load(std::memory_order_acquire)) return;

        const QByteArray pcm(header->lpData, static_cast<int>(header->dwBytesRecorded));
        const QPointer<WindowsMicrophoneCapture> target(self->owner);
        QMetaObject::invokeMethod(
            self->owner,
            [target, pcm]() {
                if (target) emit target->pcmReady(pcm);
            },
            Qt::QueuedConnection);

        header->dwBytesRecorded = 0;
        if (self->running.load(std::memory_order_acquire)) {
            waveInAddBuffer(input, header, sizeof(WAVEHDR));
        }
    }

    static UINT deviceId(const QString &requestedDevice) {
        if (requestedDevice.isEmpty() ||
            requestedDevice.compare(QStringLiteral("Default Input Device"),
                                    Qt::CaseInsensitive) == 0) {
            return WAVE_MAPPER;
        }
        const QString requested = requestedDevice.simplified();
        const UINT count = waveInGetNumDevs();
        for (UINT index = 0; index < count; ++index) {
            WAVEINCAPSW caps{};
            if (waveInGetDevCapsW(index, &caps, sizeof(caps)) != MMSYSERR_NOERROR) continue;
            const QString candidate = QString::fromWCharArray(caps.szPname).simplified();
            if (candidate.compare(requested, Qt::CaseInsensitive) == 0 ||
                requested.startsWith(candidate, Qt::CaseInsensitive) ||
                candidate.startsWith(requested.left(24), Qt::CaseInsensitive)) {
                return index;
            }
        }
        return WAVE_MAPPER;
    }
#endif
};

WindowsMicrophoneCapture::WindowsMicrophoneCapture(QObject *parent)
    : QObject(parent), impl(std::make_unique<Impl>()) {
#ifdef _WIN32
    impl->owner = this;
#endif
}

WindowsMicrophoneCapture::~WindowsMicrophoneCapture() {
    stop();
}

QStringList WindowsMicrophoneCapture::deviceNames() {
    QStringList names;
#ifdef _WIN32
    const UINT count = waveInGetNumDevs();
    for (UINT index = 0; index < count; ++index) {
        WAVEINCAPSW caps{};
        if (waveInGetDevCapsW(index, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
            names.append(QString::fromWCharArray(caps.szPname));
        }
    }
#endif
    return names;
}

bool WindowsMicrophoneCapture::setDeviceLevel(const QString &requestedDevice, float level,
                                               QString *error, QString *details) {
#ifdef _WIN32
    return Impl::configureEndpoint(requestedDevice, level, error, details);
#else
    Q_UNUSED(requestedDevice)
    Q_UNUSED(level)
    if (error) *error = QStringLiteral("Windows microphone level control is unavailable");
    if (details) details->clear();
    return false;
#endif
}

bool WindowsMicrophoneCapture::start(const QString &requestedDevice, int sampleRate,
                                     float inputLevel,
                                     QString *error) {
    stop();
#ifdef _WIN32
    QString wasapiError;
    if (impl->startWasapi(requestedDevice, sampleRate, &wasapiError)) {
        QString endpointError;
        QString endpointDetails;
        const bool endpointAdjusted = setDeviceLevel(requestedDevice, inputLevel,
                                                     &endpointError, &endpointDetails);
        qInfo().noquote() << "[TX audio] Windows capture endpoint"
                          << endpointDetails;
        if (!endpointAdjusted && !endpointError.isEmpty()) {
            qWarning().noquote() << "[TX audio] Windows input-level control failed"
                                 << endpointError;
        }
        if (error) error->clear();
        return true;
    }
    qWarning().noquote() << "[TX audio] WASAPI capture unavailable; trying WinMM"
                         << wasapiError;

    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 1;
    format.nSamplesPerSec = static_cast<DWORD>(sampleRate);
    format.wBitsPerSample = 16;
    format.nBlockAlign = static_cast<WORD>(format.nChannels * format.wBitsPerSample / 8);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

    const UINT selectedDeviceId = Impl::deviceId(requestedDevice);
    MMRESULT result = waveInOpen(&impl->handle, selectedDeviceId, &format,
                                 reinterpret_cast<DWORD_PTR>(&Impl::callback),
                                 reinterpret_cast<DWORD_PTR>(impl.get()), CALLBACK_FUNCTION);
    if (result != MMSYSERR_NOERROR) {
        impl->handle = nullptr;
        if (error) {
            *error = QStringLiteral("%1; requested=\"%2\" deviceId=%3")
                         .arg(Impl::errorText(result))
                         .arg(requestedDevice)
                         .arg(selectedDeviceId);
        }
        return false;
    }

    const int bytesPerBuffer = static_cast<int>((std::max<DWORD>)(
        static_cast<DWORD>(format.nBlockAlign) * 64u,
        format.nAvgBytesPerSec / 50u));
    for (auto &buffer : impl->buffers) {
        buffer.storage.resize(static_cast<size_t>(bytesPerBuffer));
        std::memset(&buffer.header, 0, sizeof(buffer.header));
        buffer.header.lpData = buffer.storage.data();
        buffer.header.dwBufferLength = static_cast<DWORD>(buffer.storage.size());
        result = waveInPrepareHeader(impl->handle, &buffer.header, sizeof(WAVEHDR));
        if (result != MMSYSERR_NOERROR) {
            if (error) *error = Impl::errorText(result);
            stop();
            return false;
        }
        buffer.prepared = true;
        result = waveInAddBuffer(impl->handle, &buffer.header, sizeof(WAVEHDR));
        if (result != MMSYSERR_NOERROR) {
            if (error) *error = Impl::errorText(result);
            stop();
            return false;
        }
    }

    impl->running.store(true, std::memory_order_release);
    result = waveInStart(impl->handle);
    if (result != MMSYSERR_NOERROR) {
        if (error) *error = Impl::errorText(result);
        stop();
        return false;
    }
    impl->activeBackend = QStringLiteral("WinMM waveIn");

    QString endpointError;
    QString endpointDetails;
    const bool endpointAdjusted = setDeviceLevel(requestedDevice, inputLevel,
                                                 &endpointError, &endpointDetails);
    qInfo().noquote() << "[TX audio] Windows capture endpoint"
                      << endpointDetails;
    if (!endpointAdjusted && !endpointError.isEmpty()) {
        qWarning().noquote() << "[TX audio] Windows input-level control failed"
                             << endpointError;
    }
    return true;
#else
    Q_UNUSED(requestedDevice)
    Q_UNUSED(sampleRate)
    Q_UNUSED(inputLevel)
    if (error) *error = QStringLiteral("Native Windows microphone capture is unavailable");
    return false;
#endif
}

void WindowsMicrophoneCapture::stop() {
#ifdef _WIN32
    impl->running.store(false, std::memory_order_release);
    impl->stopWasapi();
    if (!impl->handle) {
        impl->activeBackend.clear();
        return;
    }
    waveInStop(impl->handle);
    waveInReset(impl->handle);
    for (auto &buffer : impl->buffers) {
        if (buffer.prepared) {
            waveInUnprepareHeader(impl->handle, &buffer.header, sizeof(WAVEHDR));
            buffer.prepared = false;
        }
        buffer.storage.clear();
    }
    waveInClose(impl->handle);
    impl->handle = nullptr;
    impl->activeBackend.clear();
#endif
}

bool WindowsMicrophoneCapture::isRunning() const {
#ifdef _WIN32
    return impl->running.load(std::memory_order_acquire);
#else
    return false;
#endif
}

QString WindowsMicrophoneCapture::backendName() const {
#ifdef _WIN32
    return impl->activeBackend;
#else
    return {};
#endif
}

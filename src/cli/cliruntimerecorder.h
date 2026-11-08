#ifndef CLIRUNTIMERECORDER_H
#define CLIRUNTIMERECORDER_H

#include "radiosettings.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <cstddef>
#include <memory>
#include <vector>

// No QObject, event loop, GUI, demodulator, or implicit logging is required.
class CliRuntimeRecorder {
public:
    explicit CliRuntimeRecorder(std::size_t queueBudgetBytes = 32U * 1024U * 1024U,
                                std::size_t maxQueueBlocks = 64,
                                std::size_t maxActiveTypes = 5);
    ~CliRuntimeRecorder();
    CliRuntimeRecorder(const CliRuntimeRecorder &) = delete;
    CliRuntimeRecorder &operator=(const CliRuntimeRecorder &) = delete;

    // start/stop are asynchronous. Poll status or takeNotifications for completion.
    // path is the exact data filename; sidecars are path + .json/.sigmf-meta.
    QJsonObject command(const QJsonObject &request, const RadioSettings &settings);
    QJsonObject status() const;
    bool active() const; // Includes starting and stopping, including failed cleanup.
    // True for any accepting slot: source validation needs IQ metadata even for
    // audio/spectrum-only sessions, but only raw/channel slots copy IQ payloads.
    bool needsIq() const;
    bool needsAudio() const;
    bool needsSpectrum() const;
    QJsonArray takeNotifications(); // At most one latest completion per type.
    void shutdown(); // Blocking stop/drain/join. Call only after RX has stopped.

    // count is COMPLEX samples (2 * count interleaved I,Q floats).
    // true means payload accepted or metadata-only observation succeeded.
    // false means inactive or rejected; rejection while active is visible in status.
    bool submitIq(const float *samples, std::size_t count,
                  const RadioSettings &actualSettings, bool discontinuity = false);
    // Existing demodulator's 48000 Hz mono signed 16-bit little-endian PCM only.
    bool submitAudio(const QByteArray &pcm48kMono16);
    // Exact levels, no reduction; first/last bin CENTERS, positive Hz spacing.
    // utcMs == 0 uses submission wall time. fftLength comes from actualSettings.
    bool submitSpectrum(const std::vector<float> &levels, double firstHz, double binHz,
                        const RadioSettings &actualSettings, bool discontinuity = false,
                        qint64 utcMs = 0);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

#endif // CLIRUNTIMERECORDER_H

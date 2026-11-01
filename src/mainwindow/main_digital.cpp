#include "main.h"
#include "appconstants.h"
#include "dmrbackendpaths.h"
#include "dmrprivacyutils.h"
#include "dsdneobridge.h"
#include "gophertrunkbridge.h"
#include "iqbuffer.h"
#include "tuningutils.h"
#include "openradiolabdialog.h"

#include <QAbstractItemView>
#include <QCoreApplication>
#include <QDebug>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPushButton>
#include <QScopeGuard>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <utility>
#include <vector>

void YourClassName::openOpenRadioLab() {
    if (openRadioLabDialog) {
        openRadioLabDialog->show();
        openRadioLabDialog->raise();
        openRadioLabDialog->activateWindow();
        return;
    }
    openRadioLabDialog = new OpenRadioLabDialog(
        normalizedUiLanguage(uiLanguage) == QStringLiteral("uk"), this);
    connect(openRadioLabDialog, &QObject::destroyed, this, [this]() {
        openRadioLabDialog = nullptr;
    });
    openRadioLabDialog->show();
}

void YourClassName::processSstvAudioFrame(const QByteArray &pcmData) {
    const bool suppressServerLocalOutput =
        networkMode == NetworkMode::Server &&
        serverDisableLocalVisualAudio &&
        networkController &&
        networkController->isControlReady();
    if (!videoProcessor ||
        !videoProcessorThread ||
        !videoDecodeEnabled ||
        !isAnyVideoOutputVisible() ||
        !videoDecodeCheckbox ||
        !videoDecodeCheckbox->isChecked() ||
        pendingSettings.modulationType != MOD_SSTV ||
        pcmData.isEmpty() ||
        suppressServerLocalOutput ||
        (videoTestPatternCheckbox && videoTestPatternCheckbox->isChecked())) {
        return;
    }

    QMetaObject::invokeMethod(videoProcessor,
                              [processor = videoProcessor, pcmData]() {
                                  processor->processSstvPcmFrame(pcmData, 48000);
                              },
                              Qt::QueuedConnection);
}

void YourClassName::processAptAudioFrame(const QByteArray &pcmData) {
    const bool suppressServerLocalOutput =
        networkMode == NetworkMode::Server &&
        serverDisableLocalVisualAudio &&
        networkController &&
        networkController->isControlReady();
    if (!videoProcessor ||
        !videoProcessorThread ||
        !videoDecodeEnabled ||
        !isAnyVideoOutputVisible() ||
        !videoDecodeCheckbox ||
        !videoDecodeCheckbox->isChecked() ||
        pendingSettings.modulationType != MOD_APT ||
        pcmData.isEmpty() ||
        suppressServerLocalOutput ||
        (videoTestPatternCheckbox && videoTestPatternCheckbox->isChecked())) {
        return;
    }

    QMetaObject::invokeMethod(videoProcessor,
                              [processor = videoProcessor, pcmData]() {
                                  processor->processAptPcmFrame(pcmData, 48000);
                              },
                              Qt::QueuedConnection);
}

void YourClassName::processWefaxAudioFrame(const QByteArray &pcmData) {
    const bool suppressServerLocalOutput =
        networkMode == NetworkMode::Server &&
        serverDisableLocalVisualAudio &&
        networkController &&
        networkController->isControlReady();
    if (!videoProcessor ||
        !videoProcessorThread ||
        !videoDecodeEnabled ||
        !isAnyVideoOutputVisible() ||
        !videoDecodeCheckbox ||
        !videoDecodeCheckbox->isChecked() ||
        pendingSettings.modulationType != MOD_WEFAX ||
        pcmData.isEmpty() ||
        suppressServerLocalOutput ||
        (videoTestPatternCheckbox && videoTestPatternCheckbox->isChecked())) {
        return;
    }

    QMetaObject::invokeMethod(videoProcessor,
                              [processor = videoProcessor, pcmData]() {
                                  processor->processWefaxPcmFrame(pcmData, 48000);
                              },
                              Qt::QueuedConnection);
}

bool YourClassName::isVideoDecodeActive() const {
    const bool suppressServerLocalOutput =
        networkMode == NetworkMode::Server &&
        serverDisableLocalVisualAudio &&
        networkController &&
        networkController->isControlReady();
    return videoDecodeEnabled &&
           isAnyVideoOutputVisible() &&
           videoDecodeCheckbox &&
           videoDecodeCheckbox->isChecked() &&
           (pendingSettings.modulationType == MOD_ATV ||
            pendingSettings.modulationType == MOD_LRPT) &&
           (!videoTestPatternCheckbox || !videoTestPatternCheckbox->isChecked()) &&
           !suppressServerLocalOutput;
}

bool YourClassName::isAnyVideoOutputVisible() const {
    if (videoDock && videoDock->isVisible()) {
        return true;
    }
    for (auto it = dspBlockEditors.constBegin(); it != dspBlockEditors.constEnd(); ++it) {
        if (it.key().startsWith(QStringLiteral("digital_image_output")) &&
            it.value() && it.value()->isVisible()) {
            return true;
        }
    }
    return false;
}

void YourClassName::processVideoIqFrame(const QByteArray &iqData, double sampleRate, int sampleCount) {
    if (!isVideoDecodeActive() ||
        !videoProcessor ||
        !videoProcessorThread ||
        iqData.isEmpty() ||
        sampleRate <= 0.0 ||
        sampleCount <= 0) {
        return;
    }
    if (videoTestPatternCheckbox && videoTestPatternCheckbox->isChecked()) {
        return;
    }
    if (pendingSettings.modulationType == MOD_LRPT) {
        if (videoIqFramePending.exchange(true)) {
            return;
        }
        const int bytesPerIq = iqData.size() >= sampleCount * 4 ? 4 : 2;
        std::vector<float> floatSamples;
        floatSamples.reserve(static_cast<std::size_t>(sampleCount) * 2);
        const auto *src = reinterpret_cast<const uchar *>(iqData.constData());
        for (int i = 0; i < sampleCount; ++i) {
            float iSample = 0.0f;
            float qSample = 0.0f;
            if (bytesPerIq == 4) {
                const int offset = i * 4;
                if (offset + 3 >= iqData.size()) {
                    break;
                }
                const qint16 rawI = static_cast<qint16>(src[offset] | (src[offset + 1] << 8));
                const qint16 rawQ = static_cast<qint16>(src[offset + 2] | (src[offset + 3] << 8));
                iSample = rawI / 32768.0f;
                qSample = rawQ / 32768.0f;
            } else {
                const int offset = i * 2;
                if (offset + 1 >= iqData.size()) {
                    break;
                }
                iSample = (static_cast<int>(src[offset]) - 128) / 128.0f;
                qSample = (static_cast<int>(src[offset + 1]) - 128) / 128.0f;
            }
            floatSamples.push_back(iSample);
            floatSamples.push_back(qSample);
        }
        if (floatSamples.size() < 8) {
            videoIqFramePending.store(false);
            return;
        }
        RadioSettings settings = spectrumProcessingSettings();
        settings.sampleRate = sampleRate;
        QMetaObject::invokeMethod(videoProcessor,
                                  [this, processor = videoProcessor, samples = std::move(floatSamples), settings]() {
                                      processor->processFloatIqSnapshot(samples, settings);
                                      QMetaObject::invokeMethod(this,
                                                                [this]() {
                                                                    videoIqFramePending.store(false);
                                                                },
                                                                Qt::QueuedConnection);
                                  },
                                  Qt::QueuedConnection);
        return;
    }

    if (iqData.size() == sampleCount * 2 && sampleRate > 10000000.0) {
        if (videoStatusLabel) {
            videoStatusLabel->setText(QStringLiteral("Video: switch to ATV/channel IQ for wide signals"));
        }
        videoIqFramePending.store(false);
        return;
    }

    // Analog television needs every channel-IQ block in order. Dropping a block
    // while the previous one is pending destroys raster timing even when HSync
    // remains locally detectable inside each surviving block.
    QMetaObject::invokeMethod(videoProcessor,
                              [processor = videoProcessor, iqData, sampleRate, sampleCount]() {
                                  processor->processIqFrame(iqData, sampleRate, sampleCount);
                              },
                              Qt::QueuedConnection);
}

void YourClassName::processVideoSnapshotFrame() {
    const bool channelIqStreamMode =
        networkMode != NetworkMode::Disabled &&
        isChannelIqProcessingMode();
    if (!isVideoDecodeActive() ||
        channelIqStreamMode ||
        !videoProcessor ||
        !videoProcessorThread) {
        return;
    }
    if (videoIqFramePending.exchange(true)) {
        return;
    }

    std::vector<float> snapshot;
    std::uint64_t sequence = 0;
    if (!IqBuffer::snapshotRecent(snapshot, VIDEO_SNAPSHOT_MAX_FLOATS, &sequence) ||
        snapshot.size() < 4) {
        videoIqFramePending.store(false);
        return;
    }

    if (snapshot.size() > VIDEO_SNAPSHOT_MAX_FLOATS) {
        std::vector<float> tail(snapshot.end() - static_cast<std::ptrdiff_t>(VIDEO_SNAPSHOT_MAX_FLOATS),
                                snapshot.end());
        snapshot.swap(tail);
    }

    RadioSettings settings = spectrumProcessingSettings();
    QMetaObject::invokeMethod(videoProcessor,
                              [this, processor = videoProcessor, samples = std::move(snapshot), settings]() {
                                  processor->processFloatIqSnapshot(samples, settings);
                                  QMetaObject::invokeMethod(this,
                                                            [this]() {
                                                                videoIqFramePending.store(false);
                                                            },
                                                            Qt::QueuedConnection);
                              },
                              Qt::QueuedConnection);
}

void YourClassName::updateVideoProcessorMode() {
    if (!videoProcessor || !videoProcessorThread) {
        return;
    }

    const bool iqVideoEnabled = isVideoDecodeActive();
    if (sstvDemodulationCombo) {
        sstvDemodulationCombo->setEnabled(pendingSettings.modulationType == MOD_SSTV);
    }
    const bool analogVideoEnabled = iqVideoEnabled && pendingSettings.modulationType == MOD_ATV;
    videoIqFramePending.store(false);
    const bool testPatternEnabled = videoTestPatternCheckbox && videoTestPatternCheckbox->isChecked();
    const bool analogVideoTest = testPatternEnabled && pendingSettings.modulationType == MOD_ATV;
    const bool sstvTest = testPatternEnabled && pendingSettings.modulationType == MOD_SSTV;
    const bool aptTest = testPatternEnabled && pendingSettings.modulationType == MOD_APT;
    const bool wefaxTest = testPatternEnabled && pendingSettings.modulationType == MOD_WEFAX;
    const bool lrptTest = testPatternEnabled && pendingSettings.modulationType == MOD_LRPT;
    const bool suppressServerLocalOutput =
        networkMode == NetworkMode::Server &&
        serverDisableLocalVisualAudio &&
        networkController &&
        networkController->isControlReady();
    const bool sstvEnabled =
        isAnyVideoOutputVisible() &&
        pendingSettings.modulationType == MOD_SSTV &&
        (sstvTest ||
         (videoDecodeEnabled &&
          videoDecodeCheckbox &&
          videoDecodeCheckbox->isChecked())) &&
        !suppressServerLocalOutput;
    const bool aptEnabled =
        isAnyVideoOutputVisible() &&
        pendingSettings.modulationType == MOD_APT &&
        (aptTest ||
         (videoDecodeEnabled &&
          videoDecodeCheckbox &&
          videoDecodeCheckbox->isChecked())) &&
        !suppressServerLocalOutput;
    const bool wefaxEnabled =
        isAnyVideoOutputVisible() &&
        pendingSettings.modulationType == MOD_WEFAX &&
        (wefaxTest ||
         (videoDecodeEnabled &&
          videoDecodeCheckbox &&
          videoDecodeCheckbox->isChecked())) &&
        !suppressServerLocalOutput;
    const bool lrptEnabled =
        isAnyVideoOutputVisible() &&
        pendingSettings.modulationType == MOD_LRPT &&
        (lrptTest ||
         (videoDecodeEnabled &&
          videoDecodeCheckbox &&
          videoDecodeCheckbox->isChecked())) &&
        !suppressServerLocalOutput;
    QMetaObject::invokeMethod(videoProcessor,
                              [processor = videoProcessor, analogVideoTest]() {
                                  processor->setTestPatternEnabled(analogVideoTest);
                              },
                              Qt::QueuedConnection);
    QMetaObject::invokeMethod(videoProcessor,
                              [processor = videoProcessor, sstvEnabled]() {
                                  processor->configureSstv(sstvEnabled);
                              },
                              Qt::QueuedConnection);
    QMetaObject::invokeMethod(videoProcessor,
                              [processor = videoProcessor, sstvTest]() {
                                  processor->setSstvTestPatternEnabled(sstvTest);
                              },
                              Qt::QueuedConnection);
    QMetaObject::invokeMethod(videoProcessor,
                              [processor = videoProcessor, aptEnabled]() {
                                  processor->configureApt(aptEnabled);
                              },
                              Qt::QueuedConnection);
    QMetaObject::invokeMethod(videoProcessor,
                              [processor = videoProcessor, aptTest]() {
                                  processor->setAptTestPatternEnabled(aptTest);
                              },
                              Qt::QueuedConnection);
    QMetaObject::invokeMethod(videoProcessor,
                              [processor = videoProcessor, wefaxEnabled]() {
                                  processor->configureWefax(wefaxEnabled);
                              },
                              Qt::QueuedConnection);
    QMetaObject::invokeMethod(videoProcessor,
                              [processor = videoProcessor, wefaxTest]() {
                                  processor->setWefaxTestPatternEnabled(wefaxTest);
                              },
                              Qt::QueuedConnection);
    QMetaObject::invokeMethod(videoProcessor,
                              [processor = videoProcessor, lrptEnabled]() {
                                  processor->configureLrpt(lrptEnabled);
                              },
                              Qt::QueuedConnection);

    const bool channelIqStreamMode =
        networkMode != NetworkMode::Disabled &&
        isChannelIqProcessingMode();
    // LRPT still uses the exploratory full-band snapshot path. ATV is fed by
    // the continuous channel-IQ producer configured in updateIqFrameProducerSettings().
    const bool snapshotVideoEnabled = iqVideoEnabled &&
                                      pendingSettings.modulationType == MOD_LRPT &&
                                      !channelIqStreamMode;
    if (videoSnapshotTimer) {
        if (snapshotVideoEnabled && !videoSnapshotTimer->isActive()) {
            videoSnapshotTimer->start();
        } else if (!snapshotVideoEnabled && videoSnapshotTimer->isActive()) {
            videoSnapshotTimer->stop();
        }
    }
    if (sstvTest) {
        if (videoStatusLabel) {
            videoStatusLabel->setText(QStringLiteral("SSTV Robot36 test stream"));
        }
    }
    if (aptTest) {
        if (videoStatusLabel) {
            videoStatusLabel->setText(QStringLiteral("NOAA APT test stream"));
        }
    }
    if (wefaxTest) {
        if (videoStatusLabel) {
            videoStatusLabel->setText(QStringLiteral("WEFAX test stream"));
        }
    }
    if (lrptTest) {
        if (videoStatusLabel) {
            videoStatusLabel->setText(QStringLiteral("Meteor LRPT beta: QPSK monitor test"));
        }
    }
    if (!iqVideoEnabled && !sstvTest && !aptTest && !wefaxTest && !lrptEnabled && videoWidget) {
        videoWidget->clearFrame();
    }
    if (pendingSettings.modulationType == MOD_SSTV && !sstvTest && videoStatusLabel) {
        videoStatusLabel->setText(QStringLiteral("SSTV: image decoder setup ready"));
    }
    if (pendingSettings.modulationType == MOD_APT && !aptTest && videoStatusLabel) {
        videoStatusLabel->setText(QStringLiteral("NOAA APT: image decoder setup ready"));
    }
    if (pendingSettings.modulationType == MOD_WEFAX && !wefaxTest && videoStatusLabel) {
        videoStatusLabel->setText(QStringLiteral("WEFAX: image decoder setup ready"));
    }
    if (pendingSettings.modulationType == MOD_LRPT && !lrptTest && videoStatusLabel) {
        videoStatusLabel->setText(QStringLiteral("Meteor LRPT beta: QPSK IQ monitor ready"));
    }
    const int demodMode = videoDemodCombo ? videoDemodCombo->currentData().toInt()
                                          : VideoProcessor::FmVideo;
    const double lineRate = videoStandardCombo ? videoStandardCombo->currentData().toDouble()
                                               : 15625.0;
    const bool invertVideo = videoInvertCheckbox && videoInvertCheckbox->isChecked();
    const bool hSyncEnabled = !videoHSyncCheckbox || videoHSyncCheckbox->isChecked();
    const bool vSyncEnabled = !videoVSyncCheckbox || videoVSyncCheckbox->isChecked();
    QMetaObject::invokeMethod(videoProcessor,
                              [processor = videoProcessor,
                               analogVideoEnabled,
                               demodMode,
                               lineRate,
                               invertVideo,
                               hSyncEnabled,
                               vSyncEnabled]() {
                                  processor->configure(analogVideoEnabled,
                                                       demodMode,
                                                       lineRate,
                                                       384,
                                                       288,
                                                       invertVideo,
                                                       hSyncEnabled,
                                                       vSyncEnabled);
                              },
                              Qt::QueuedConnection);
}

RadioSettings YourClassName::audioProcessorSettings() const {
    RadioSettings settings = pendingSettings;
    if (dmrBasebandRateCombo) {
        settings.dmrBasebandSampleRate =
            normalizedDmrBasebandSampleRate(dmrBasebandRateCombo->currentData().toInt());
    } else {
        settings.dmrBasebandSampleRate =
            normalizedDmrBasebandSampleRate(settings.dmrBasebandSampleRate);
    }
    if (dmrChannelRateCombo) {
        settings.dmrChannelSampleRate = dmrChannelRateCombo->currentData().toInt();
    }
    if (dmrAmbeLayoutCombo) {
        settings.dmrAmbeLayout =
            normalizedDmrAmbeLayout(dmrAmbeLayoutCombo->currentData().toInt());
    } else {
        settings.dmrAmbeLayout =
            normalizedDmrAmbeLayout(settings.dmrAmbeLayout);
    }
    if (offlineIqPlaybackActive && offlineIqPlaybackSampleRate > 0.0) {
        settings.sampleRate = offlineIqPlaybackSampleRate;
        settings.centerFrequency = pendingSettings.listeningFrequency;
        settings.actualFrequency = pendingSettings.listeningFrequency;
        settings.inputMode = INPUT_RF;
    }
    return settings;
}

RadioSettings YourClassName::spectrumProcessingSettings() const {
    RadioSettings settings = pendingSettings;
    if (dmrBasebandRateCombo) {
        settings.dmrBasebandSampleRate =
            normalizedDmrBasebandSampleRate(dmrBasebandRateCombo->currentData().toInt());
    } else {
        settings.dmrBasebandSampleRate =
            normalizedDmrBasebandSampleRate(settings.dmrBasebandSampleRate);
    }
    if (dmrChannelRateCombo) {
        settings.dmrChannelSampleRate = dmrChannelRateCombo->currentData().toInt();
    }
    if (dmrAmbeLayoutCombo) {
        settings.dmrAmbeLayout =
            normalizedDmrAmbeLayout(dmrAmbeLayoutCombo->currentData().toInt());
    } else {
        settings.dmrAmbeLayout =
            normalizedDmrAmbeLayout(settings.dmrAmbeLayout);
    }
    if (offlineIqPlaybackActive && offlineIqPlaybackSampleRate > 0.0) {
        settings.sampleRate = offlineIqPlaybackSampleRate;
        settings.centerFrequency = pendingSettings.listeningFrequency;
        settings.actualFrequency = pendingSettings.listeningFrequency;
        settings.inputMode = INPUT_RF;
    }
    return settings;
}

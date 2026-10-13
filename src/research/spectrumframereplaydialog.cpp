#include "spectrumframereplaydialog.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "audioprocessor.h"
#include "appconstants.h"
#include "frequencycontrol.h"
#include "iqbuffer.h"
#include "MyGraphWidget.h"
#include "playbackmanager.h"
#include "samplefileutils.h"
#include "scalewidget.h"
#include "appsettingsutils.h"
#include "waterfall3dview.h"
#include "dspbranchaudiooutput.h"
#include "iqchannelizer.h"
#include "experimentbundle.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QFileDialog>
#include <QFontMetrics>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>
#include <QJsonDocument>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QMessageBox>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QScopedValueRollback>
#include <QSettings>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <thread>

namespace {
constexpr double kReplayAudioPrebufferSeconds = 0.24;
constexpr double kReplayRawFullIqPrebufferSeconds = 0.08;
constexpr double kReplayIqLeadSeconds = 0.45;
constexpr double kReplayIqMaxQueuedSeconds = 0.80;
constexpr int kReplayIqPumpIntervalMs = 4;
constexpr int kReplayAudioDrainAfterIqMs = 900;
constexpr int kReplayIqMaxBlocksPerPump = 12;
constexpr int kChannelIqReplaySamplesPerFrame = 4096;
constexpr int kFullIqReplaySamplesPerFrame = 262144;

QString replayDialogLanguage() {
    QSettings settings(persistentSettingsFilePath(), QSettings::IniFormat);
    const QString language = settings.value(QStringLiteral("ui/language"), QStringLiteral("en"))
                                 .toString()
                                 .trimmed();
    return language.isEmpty() ? QStringLiteral("en") : language;
}

QJsonObject replayDialogTranslations(const QString &language) {
    static QString cachedLanguage;
    static QJsonObject cachedObject;
    if (cachedLanguage == language && !cachedObject.isEmpty()) {
        return cachedObject;
    }

    QJsonObject root;
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        QDir(appDir).absoluteFilePath(QStringLiteral("translations.json")),
        QDir::current().absoluteFilePath(QStringLiteral("translations.json")),
        QDir(appDir).absoluteFilePath(QStringLiteral("../../translations.json"))
    };
    for (const QString &path : candidates) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            continue;
        }
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
        if (parseError.error == QJsonParseError::NoError && document.isObject()) {
            root = document.object();
            break;
        }
    }

    QJsonObject languageObject = root.value(language).toObject();
    if (languageObject.isEmpty() && language.size() > 2) {
        languageObject = root.value(language.left(2)).toObject();
    }
    if (languageObject.isEmpty()) {
        languageObject = root.value(QStringLiteral("en")).toObject();
    }

    cachedLanguage = language;
    cachedObject = languageObject;
    return cachedObject;
}

QString replayDialogTranslationKey(const QString &id) {
    if (id == QStringLiteral("title")) return QStringLiteral("spectrum_replay_dialog_title");
    if (id == QStringLiteral("open")) return QStringLiteral("spectrum_replay_dialog_open");
    if (id == QStringLiteral("open_tooltip")) return QStringLiteral("spectrum_replay_dialog_open_tooltip");
    if (id == QStringLiteral("play")) return QStringLiteral("spectrum_replay_dialog_play");
    if (id == QStringLiteral("stop")) return QStringLiteral("spectrum_replay_dialog_stop");
    if (id == QStringLiteral("zoom")) return QStringLiteral("spectrum_replay_dialog_zoom");
    if (id == QStringLiteral("zoom_tooltip")) return QStringLiteral("spectrum_replay_dialog_zoom_tooltip");
    if (id == QStringLiteral("loupe")) return QStringLiteral("spectrum_replay_dialog_loupe");
    if (id == QStringLiteral("loupe_tooltip")) return QStringLiteral("spectrum_replay_dialog_loupe_tooltip");
    if (id == QStringLiteral("contrast")) return QStringLiteral("spectrum_replay_dialog_contrast");
    if (id == QStringLiteral("contrast_tooltip")) return QStringLiteral("spectrum_replay_dialog_contrast_tooltip");
    if (id == QStringLiteral("sensitivity")) return QStringLiteral("spectrum_replay_dialog_sensitivity");
    if (id == QStringLiteral("sensitivity_tooltip")) return QStringLiteral("spectrum_replay_dialog_sensitivity_tooltip");
    if (id == QStringLiteral("min")) return QStringLiteral("spectrum_replay_dialog_min");
    if (id == QStringLiteral("max")) return QStringLiteral("spectrum_replay_dialog_max");
    if (id == QStringLiteral("replay_audio")) return QStringLiteral("spectrum_replay_dialog_replay_audio");
    if (id == QStringLiteral("audio")) return QStringLiteral("spectrum_replay_dialog_audio");
    if (id == QStringLiteral("channel_iq")) return QStringLiteral("spectrum_replay_dialog_channel_iq");
    if (id == QStringLiteral("full_iq")) return QStringLiteral("spectrum_replay_dialog_full_iq");
    if (id == QStringLiteral("iq_source_tooltip")) return QStringLiteral("spectrum_replay_dialog_iq_source_tooltip");
    if (id == QStringLiteral("listen_frequency_tooltip")) return QStringLiteral("spectrum_replay_dialog_listen_frequency_tooltip");
    if (id == QStringLiteral("audio_checkbox_tooltip")) return QStringLiteral("spectrum_replay_dialog_audio_checkbox_tooltip");
    if (id == QStringLiteral("select_iq")) return QStringLiteral("spectrum_replay_dialog_select_iq");
    if (id == QStringLiteral("linked_iq_not_found")) return QStringLiteral("spectrum_replay_dialog_linked_iq_not_found");
    if (id == QStringLiteral("full_iq_bad_rate")) return QStringLiteral("spectrum_replay_dialog_full_iq_bad_rate");
    if (id == QStringLiteral("display_mode")) return QStringLiteral("spectrum_replay_dialog_display_mode");
    if (id == QStringLiteral("display_2d")) return QStringLiteral("spectrum_replay_dialog_display_2d");
    if (id == QStringLiteral("display_3d")) return QStringLiteral("spectrum_replay_dialog_display_3d");
    if (id == QStringLiteral("display_3d_mini")) return QStringLiteral("spectrum_replay_dialog_display_3d_mini");
    if (id == QStringLiteral("resolution")) return QStringLiteral("spectrum_replay_dialog_resolution");
    if (id == QStringLiteral("area_ruler")) return QStringLiteral("spectrum_replay_dialog_area_ruler");
    if (id == QStringLiteral("area_ruler_tooltip")) return QStringLiteral("spectrum_replay_dialog_area_ruler_tooltip");
    if (id == QStringLiteral("frequency_span")) return QStringLiteral("spectrum_replay_dialog_frequency_span");
    if (id == QStringLiteral("time_span")) return QStringLiteral("spectrum_replay_dialog_time_span");
    if (id == QStringLiteral("slice_step")) return QStringLiteral("spectrum_replay_dialog_slice_step");
    if (id == QStringLiteral("slice_width")) return QStringLiteral("spectrum_replay_dialog_slice_width");
    if (id == QStringLiteral("spectrum_slice_step")) return QStringLiteral("waterfall_3d_spectrum_slice_step");
    if (id == QStringLiteral("spectrum_slice_rows")) return QStringLiteral("waterfall_3d_spectrum_slice_rows");
    if (id == QStringLiteral("spectrum_slice_capture")) return QStringLiteral("waterfall_3d_spectrum_slice_capture");
    if (id == QStringLiteral("spectrum_slice_capture_fixed")) return QStringLiteral("waterfall_3d_spectrum_slice_capture_fixed");
    if (id == QStringLiteral("vnc_slice_input")) return QStringLiteral("waterfall_3d_vnc_slice_input");
    if (id == QStringLiteral("vnc_slice_input_tooltip")) return QStringLiteral("waterfall_3d_vnc_slice_input_tooltip");
    return QString();
}

QString replayText(const char *key, const char *fallback) {
    const QString id = QString::fromLatin1(key);
    if (replayDialogLanguage().startsWith("uk")) {
        if (id == "experiment_audio_limit") return QString::fromUtf8("Ізольоване аудіо: AM/NFM/SAM/USB/LSB/DSB/CW/WFM; канальний IQ 48 кГц..2 МГц.");
        if (id == "experiment_image_failed") return QString::fromUtf8("Не вдалося виділити пам'ять зображення. Зменште обсяг даних відтворення.");
        if (id == "experiment_display_limited") return QString::fromUtf8("Вигляд адаптовано до меж відтворення");
    }
    const QString translationKey = replayDialogTranslationKey(id);
    if (!translationKey.isEmpty()) {
        const QJsonObject languageObject = replayDialogTranslations(replayDialogLanguage());
        const QString translated = languageObject.value(translationKey).toString();
        if (!translated.isEmpty()) {
            return translated;
        }
    }
    return QString::fromLatin1(fallback);
}
}

SpectrumFrameReplayDialog::SpectrumFrameReplayDialog(QWidget *parent, bool isolated)
    : QDialog(parent), isolatedExperiment(isolated) {
    setWindowTitle(replayText("title", "Spectrum replay"));
    resize(1100, 720);
    if (isolatedExperiment) setMaximumSize(4096, 2304);

    graph = new MyGraphWidget(this);
    graph->setMinimumHeight(220);
    graph->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    graph->setLevelRange(levelMin, levelMax);
    graph->setPauseControlVisible(true);
    graph->installEventFilter(this);
    graphScroll = new QScrollArea(this);
    graphScroll->setWidget(graph);
    graphScroll->setWidgetResizable(false);
    graphScroll->setMinimumHeight(240);

    scaleWidget = new ScaleWidget(this);
    scaleWidget->setFixedHeight(52);
    scaleWidget->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    scaleScroll = new QScrollArea(this);
    scaleScroll->setWidget(scaleWidget);
    scaleScroll->setWidgetResizable(false);
    scaleScroll->setFixedHeight(58);
    scaleScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scaleScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    connect(scaleWidget, &ScaleWidget::frequencyChanged, this, [this]() {
        if (selectedReplayIqSource() == ReplayIqSource::FullIq) {
            setReplayListenFrequency(scaleWidget ? scaleWidget->currentListeningFrequency() : replayListenFrequencyHz,
                                     true);
        } else {
            updateScaleWidget();
        }
    });
    connect(scaleWidget, &ScaleWidget::tuningChanged, this, [this](double listeningHz, double) {
        if (selectedReplayIqSource() == ReplayIqSource::FullIq) {
            setReplayListenFrequency(listeningHz, true);
        } else {
            updateScaleWidget();
        }
    });
    connect(scaleWidget, &ScaleWidget::centralFrequencyChanged, this, [this]() {
        updateScaleWidget();
    });

    openButton = new QPushButton(replayText("open", "Open"), this);
    openButton->setToolTip(replayText("open_tooltip", "Open another spectrum replay file"));
    connect(openButton, &QPushButton::clicked, this, &SpectrumFrameReplayDialog::openRecordingFromDialog);

    playButton = new QPushButton(replayText("play", "Play"), this);
    playButton->setCheckable(true);
    connect(playButton, &QPushButton::toggled, this, [this](bool checked) {
        setPlaybackEnabled(checked);
    });

    speedCombo = new QComboBox(this);
    speedCombo->addItem(QStringLiteral("0.25x"), 0.25);
    speedCombo->addItem(QStringLiteral("0.5x"), 0.5);
    speedCombo->addItem(QStringLiteral("1x"), 1.0);
    speedCombo->addItem(QStringLiteral("2x"), 2.0);
    speedCombo->addItem(QStringLiteral("4x"), 4.0);
    speedCombo->setCurrentIndex(2);
    connect(speedCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        schedulePersistentUiSettingsSave();
        if (playbackTimer.isActive()) {
            scheduleNextPlaybackFrame();
        }
    });

    zoomSlider = new QSlider(Qt::Horizontal, this);
    zoomSlider->setRange(100, 400);
    zoomSlider->setValue(waterfallZoomPercent);
    zoomSlider->setToolTip(replayText("zoom_tooltip", "Waterfall and spectrum horizontal zoom; 100% fits the window"));
    zoomValueLabel = new QLabel(this);
    zoomValueLabel->setMinimumWidth(42);
    zoomValueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    connect(zoomSlider, &QSlider::valueChanged, this, [this](int value) {
        waterfallZoomPercent = std::clamp(value, 100, 400);
        updateSliderValueLabels();
        scaledWaterfallDirty = true;
        schedulePersistentUiSettingsSave();
        renderWaterfallPixmap();
    });

    rowHeightSlider = new QSlider(Qt::Horizontal, this);
    rowHeightSlider->setRange(1, 16);
    rowHeightSlider->setValue(waterfallRowHeight);
    rowHeightSlider->setToolTip(replayText("loupe_tooltip", "Selected waterfall row loupe height"));
    rowHeightValueLabel = new QLabel(this);
    rowHeightValueLabel->setMinimumWidth(24);
    rowHeightValueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    connect(rowHeightSlider, &QSlider::valueChanged, this, [this](int value) {
        waterfallRowHeight = std::clamp(value, 1, 16);
        updateSliderValueLabels();
        schedulePersistentUiSettingsSave();
        renderWaterfallPixmap();
    });

    contrastSlider = new QSlider(Qt::Horizontal, this);
    contrastSlider->setRange(1, 20);
    contrastSlider->setValue(static_cast<int>(std::lround(replayContrast)));
    contrastSlider->setToolTip(replayText("contrast_tooltip", "Replay waterfall palette contrast"));
    contrastValueLabel = new QLabel(this);
    contrastValueLabel->setMinimumWidth(24);
    contrastValueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    connect(contrastSlider, &QSlider::valueChanged, this, [this](int value) {
        replayContrast = static_cast<float>(std::clamp(value, 1, 20));
        updateSliderValueLabels();
        schedulePersistentUiSettingsSave();
        rebuildWaterfallImage();
        updateFrameSelection(selectedFrame);
    });

    sensitivitySlider = new QSlider(Qt::Horizontal, this);
    sensitivitySlider->setRange(1, 30);
    sensitivitySlider->setValue(static_cast<int>(std::lround(replaySensitivity)));
    sensitivitySlider->setToolTip(replayText("sensitivity_tooltip", "Replay waterfall palette sensitivity"));
    sensitivityValueLabel = new QLabel(this);
    sensitivityValueLabel->setMinimumWidth(24);
    sensitivityValueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    connect(sensitivitySlider, &QSlider::valueChanged, this, [this](int value) {
        replaySensitivity = static_cast<float>(std::clamp(value, 1, 30));
        updateSliderValueLabels();
        schedulePersistentUiSettingsSave();
        rebuildWaterfallImage();
        updateFrameSelection(selectedFrame);
    });

    levelMinSpin = new QSpinBox(this);
    levelMinSpin->setRange(-180, 20);
    levelMinSpin->setValue(static_cast<int>(levelMin));
    levelMinSpin->setSuffix(QStringLiteral(" dB"));
    connect(levelMinSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int value) {
        levelMin = static_cast<float>(value);
        if (levelMin >= levelMax) {
            levelMax = levelMin + 1.0f;
            if (levelMaxSpin) {
                levelMaxSpin->setValue(static_cast<int>(levelMax));
            }
        }
        schedulePersistentUiSettingsSave();
        rebuildWaterfallImage();
        updateFrameSelection(selectedFrame);
    });

    levelMaxSpin = new QSpinBox(this);
    levelMaxSpin->setRange(-180, 20);
    levelMaxSpin->setValue(static_cast<int>(levelMax));
    levelMaxSpin->setSuffix(QStringLiteral(" dB"));
    connect(levelMaxSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int value) {
        levelMax = static_cast<float>(value);
        if (levelMax <= levelMin) {
            levelMin = levelMax - 1.0f;
            if (levelMinSpin) {
                levelMinSpin->setValue(static_cast<int>(levelMin));
            }
        }
        schedulePersistentUiSettingsSave();
        rebuildWaterfallImage();
        updateFrameSelection(selectedFrame);
    });

    replayIqSourceCombo = new QComboBox(this);
    replayIqSourceCombo->addItem(replayText("channel_iq", "Channel IQ"), static_cast<int>(ReplayIqSource::ChannelIq));
    replayIqSourceCombo->addItem(replayText("full_iq", "Full IQ"), static_cast<int>(ReplayIqSource::FullIq));
    replayIqSourceCombo->setToolTip(replayText("iq_source_tooltip", "IQ source used by local replay audio"));
    connect(replayIqSourceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        updateLinkedIqControls();
        updateReplayMarker();
        if (localIqPlaybackActive) {
            stopLocalIqPlayback();
        }
    });

    listenFrequencyControl = new FrequencyControl(this);
    listenFrequencyControl->setRangeHz(0.0, 14000.0e6);
    listenFrequencyControl->setValueHz(100.0e6);
    listenFrequencyControl->setStepPresets({
        {QStringLiteral("1 Hz"), 1.0},
        {QStringLiteral("10 Hz"), 10.0},
        {QStringLiteral("100 Hz"), 100.0},
        {QStringLiteral("1 kHz"), 1000.0},
        {QStringLiteral("5 kHz"), 5000.0},
        {QStringLiteral("10 kHz"), 10000.0},
        {QStringLiteral("25 kHz"), 25000.0},
        {QStringLiteral("100 kHz"), 100000.0},
        {QStringLiteral("1 MHz"), 1000000.0},
    });
    listenFrequencyControl->setToolTip(replayText("listen_frequency_tooltip", "Listening frequency for linked Full IQ replay"));
    connect(listenFrequencyControl, &FrequencyControl::valueCommitted, this, [this](double valueHz) {
        setReplayListenFrequency(valueHz, true);
    });

    replayAudioCheckbox = new QCheckBox(replayText("audio", "Audio"), this);
    replayAudioCheckbox->setToolTip(replayText("audio_checkbox_tooltip", "Play replay audio together with the main replay Play button"));
    connect(replayAudioCheckbox, &QCheckBox::toggled, this, [this](bool checked) {
        if (!checked) {
            stopLocalIqPlayback();
            return;
        }
        if (speedCombo) {
            const int oneXIndex = speedCombo->findData(1.0);
            if (oneXIndex >= 0 && speedCombo->currentIndex() != oneXIndex) {
                QSignalBlocker blocker(speedCombo);
                speedCombo->setCurrentIndex(oneXIndex);
            }
        }
        if (playButton && playButton->isChecked()) {
            startLocalIqPlayback();
        }
    });

    linkedIqLabel = new QLabel(this);
    linkedIqLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

    localAudioProcessor = new AudioProcessor(this);
    localAudioProcessor->setLocalPlaybackEnabled(true);
    connect(localAudioProcessor,
            &AudioProcessor::audioFrameReady,
            this,
            &SpectrumFrameReplayDialog::replayAudioFrameReady,
            Qt::QueuedConnection);

    saveSettingsTimer.setSingleShot(true);
    saveSettingsTimer.setInterval(750);
    connect(&saveSettingsTimer, &QTimer::timeout, this, [this]() {
        savePersistentUiSettings();
    });

    connect(&playbackTimer, &QTimer::timeout, this, [this]() {
        if (recording->frames.empty()) {
            setPlaybackEnabled(false);
            return;
        }
        const double speed = replaySpeedMultiplier();
        const qint64 targetElapsedMs =
            playbackStartElapsedMs +
            static_cast<qint64>(std::llround(static_cast<double>(playbackClock.elapsed()) * speed));
        const qint64 lastElapsedMs = recording->frames.back().elapsedMs;
        if (targetElapsedMs >= lastElapsedMs) {
            qDebug() << "[SpectrumReplay] visual end"
                     << "clockMs" << playbackClock.elapsed()
                     << "targetElapsedMs" << targetElapsedMs
                     << "lastElapsedMs" << lastElapsedMs
                     << "selectedFrame" << selectedFrame
                     << "speed" << speed;
            playbackTimer.stop();
            if (playButton) {
            QSignalBlocker blocker(playButton);
            playButton->setChecked(false);
            playButton->setText(replayText("play", "Play"));
            }
            stopLocalIqPlayback();
            if (timelineSlider) {
                timelineSlider->setValue(0);
            } else {
                updateFrameSelection(0);
            }
            return;
        }

        int low = playbackStartFrame;
        int high = static_cast<int>(recording->frames.size()) - 1;
        while (low < high) {
            const int mid = low + (high - low + 1) / 2;
            if (recording->frames[static_cast<std::size_t>(mid)].elapsedMs <= targetElapsedMs) {
                low = mid;
            } else {
                high = mid - 1;
            }
        }
        const int next = std::max(playbackStartFrame, low);
        if (next != selectedFrame) {
            if (timelineSlider) {
                timelineSlider->setValue(next);
            } else {
                updateFrameSelection(next);
            }
        }
        playbackTimer.start(15);
    });

    waterfallLabel = new QLabel(this);
    waterfallLabel->setMinimumHeight(260);
    waterfallLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    waterfallLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    waterfallLabel->installEventFilter(this);

    waterfallScroll = new QScrollArea(this);
    waterfallScroll->setWidget(waterfallLabel);
    waterfallScroll->setWidgetResizable(false);
    if (isolatedExperiment) {
        experimentWaterfallTile = new QLabel(waterfallLabel);
        experimentWaterfallTile->setAttribute(Qt::WA_TransparentForMouseEvents);
        experimentWaterfallTile->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        for (auto *bar : {waterfallScroll->verticalScrollBar(), waterfallScroll->horizontalScrollBar()})
            connect(bar, &QScrollBar::valueChanged, this, [this] { renderWaterfallPixmap(); });
    }
    waterfallScroll->setMinimumHeight(300);
    waterfallPauseButton = new QToolButton(waterfallScroll->viewport());
    waterfallPauseButton->setAutoRaise(true);
    waterfallPauseButton->setCheckable(true);
    waterfallPauseButton->setFixedSize(24, 24);
    waterfallPauseButton->setIcon(style()->standardIcon(QStyle::SP_MediaPause));
    waterfallPauseButton->setToolTip(replayText("pause_waterfall", "Pause only the replay waterfall"));
    connect(waterfallPauseButton, &QToolButton::toggled, this, [this](bool paused) {
        replayWaterfallPaused = paused;
        waterfallPauseButton->setIcon(style()->standardIcon(paused
                                                                 ? QStyle::SP_MediaPlay
                                                                 : QStyle::SP_MediaPause));
        waterfallPauseButton->setToolTip(paused
                                             ? replayText("resume_waterfall", "Resume replay waterfall")
                                             : replayText("pause_waterfall", "Pause only the replay waterfall"));
        if (!paused) {
            renderWaterfallPixmap();
            if (replayWaterfallDisplayMode != 0) rebuild3DWindow(true);
        }
    });

    waterfall3DView = new Waterfall3DView(this);
    waterfall3DView->setHistoryCapacity(128);
    waterfall3DView->hide();
    connect(waterfall3DView,
            &Waterfall3DView::frequencySliceSelected,
            this,
            [this](double ratio) {
                if (recording->frames.empty()) {
                    return;
                }
                const SpectrumFrameRecord &frame =
                    recording->frames[static_cast<std::size_t>(selectedFrame)];
                setReplayListenFrequency(frame.minFrequency +
                                             std::clamp(ratio, 0.0, 1.0) *
                                                 (frame.maxFrequency - frame.minFrequency),
                                         true);
            });
    connect(graphScroll->horizontalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
        if (syncingHorizontalScroll || !waterfallScroll) {
            return;
        }
        syncingHorizontalScroll = true;
        if (scaleScroll) {
            scaleScroll->horizontalScrollBar()->setValue(value);
        }
        waterfallScroll->horizontalScrollBar()->setValue(value);
        syncingHorizontalScroll = false;
    });
    connect(scaleScroll->horizontalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
        if (syncingHorizontalScroll || !graphScroll || !waterfallScroll) {
            return;
        }
        syncingHorizontalScroll = true;
        graphScroll->horizontalScrollBar()->setValue(value);
        waterfallScroll->horizontalScrollBar()->setValue(value);
        syncingHorizontalScroll = false;
    });
    connect(waterfallScroll->horizontalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
        if (syncingHorizontalScroll || !graphScroll) {
            return;
        }
        syncingHorizontalScroll = true;
        if (scaleScroll) {
            scaleScroll->horizontalScrollBar()->setValue(value);
        }
        graphScroll->horizontalScrollBar()->setValue(value);
        syncingHorizontalScroll = false;
    });

    timelineSlider = new QSlider(Qt::Horizontal, this);
    timelineSlider->setRange(0, 0);
    connect(timelineSlider, &QSlider::valueChanged, this, [this](int value) {
        updateFrameSelection(value);
    });

    infoLabel = new QLabel(this);
    infoLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

    waterfallDisplayModeCombo = new QComboBox(this);
    waterfallDisplayModeCombo->addItem(replayText("display_2d", "2D"), 0);
    waterfallDisplayModeCombo->addItem(replayText("display_3d", "3D"), 1);
    waterfallDisplayModeCombo->addItem(replayText("display_3d_mini", "3D + mini"), 2);
    connect(waterfallDisplayModeCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this,
            [this](int) {
                replayWaterfallDisplayMode = waterfallDisplayModeCombo->currentData().toInt();
                updateReplayDisplayMode();
                schedulePersistentUiSettingsSave();
            });

    waterfall3DResolutionCombo = new QComboBox(this);
    for (const int divisor : {1, 2, 4, 8, 16, 32, 64}) {
        waterfall3DResolutionCombo->addItem(QStringLiteral("1/%1").arg(divisor), divisor);
    }
    connect(waterfall3DResolutionCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this,
            [this](int) {
                replay3DResolutionDivisor = waterfall3DResolutionCombo->currentData().toInt();
                replay3DWindowStart = -1;
                rebuild3DWindow(true);
                schedulePersistentUiSettingsSave();
            });

    waterfall3DSliceStepSpin = new QSpinBox(this);
    waterfall3DSliceStepSpin->setRange(1, 256);
    waterfall3DSliceStepSpin->setValue(replay3DSliceStep);
    connect(waterfall3DSliceStepSpin,
            QOverload<int>::of(&QSpinBox::valueChanged),
            this,
            [this](int value) {
                replay3DSliceStep = value;
                if (waterfall3DView) {
                    waterfall3DView->setSliceScrollStep(value);
                }
                schedulePersistentUiSettingsSave();
            });

    waterfall3DSliceWidthSpin = new QSpinBox(this);
    waterfall3DSliceWidthSpin->setRange(1, 4096);
    waterfall3DSliceWidthSpin->setValue(replay3DSliceWidth);
    connect(waterfall3DSliceWidthSpin,
            QOverload<int>::of(&QSpinBox::valueChanged),
            this,
            [this](int value) {
                replay3DSliceWidth = value;
                if (waterfall3DView) {
                    waterfall3DView->setSliceWidth(value);
                }
                schedulePersistentUiSettingsSave();
            });

    waterfall3DSpectrumSliceStepSpin = new QSpinBox(this);
    waterfall3DSpectrumSliceStepSpin->setRange(1, 2048);
    waterfall3DSpectrumSliceStepSpin->setValue(replay3DSpectrumSliceStep);
    connect(waterfall3DSpectrumSliceStepSpin,
            QOverload<int>::of(&QSpinBox::valueChanged),
            this,
            [this](int value) {
                replay3DSpectrumSliceStep = value;
                if (waterfall3DView) {
                    waterfall3DView->setSpectrumSliceScrollStep(value);
                }
                schedulePersistentUiSettingsSave();
            });

    waterfall3DSpectrumSliceRowsSpin = new QSpinBox(this);
    waterfall3DSpectrumSliceRowsSpin->setRange(1, 2048);
    waterfall3DSpectrumSliceRowsSpin->setValue(replay3DSpectrumSliceRows);
    connect(waterfall3DSpectrumSliceRowsSpin,
            QOverload<int>::of(&QSpinBox::valueChanged),
            this,
            [this](int value) {
                replay3DSpectrumSliceRows = value;
                if (waterfall3DView) {
                    waterfall3DView->setSpectrumSliceWidth(value);
                }
                schedulePersistentUiSettingsSave();
            });

    waterfall3DSpectrumSliceCaptureCheckbox = new QCheckBox(
        replayText("spectrum_slice_capture", "Capture"),
        this);
    waterfall3DSpectrumSliceCaptureCheckbox->setChecked(replay3DSpectrumSliceCapture);
    connect(waterfall3DSpectrumSliceCaptureCheckbox,
            &QCheckBox::toggled,
            this,
            [this](bool checked) {
                replay3DSpectrumSliceCapture = checked;
                if (waterfall3DView) {
                    waterfall3DView->setSpectrumSliceCapture(checked);
                }
                if (waterfall3DSpectrumSliceCaptureFixedCheckbox) {
                    waterfall3DSpectrumSliceCaptureFixedCheckbox->setEnabled(checked);
                }
                schedulePersistentUiSettingsSave();
            });
    waterfall3DSpectrumSliceCaptureFixedCheckbox = new QCheckBox(
        replayText("spectrum_slice_capture_fixed", "Fix capture"),
        this);
    waterfall3DSpectrumSliceCaptureFixedCheckbox->setChecked(replay3DSpectrumSliceCaptureFixed);
    waterfall3DSpectrumSliceCaptureFixedCheckbox->setEnabled(replay3DSpectrumSliceCapture);
    connect(waterfall3DSpectrumSliceCaptureFixedCheckbox,
            &QCheckBox::toggled,
            this,
            [this](bool checked) {
                replay3DSpectrumSliceCaptureFixed = checked;
                if (waterfall3DView) {
                    waterfall3DView->setSpectrumSliceCaptureFixed(checked);
                }
                schedulePersistentUiSettingsSave();
            });
    waterfall3DVncSliceInputCheckbox = new QCheckBox(
        replayText("vnc_slice_input", "VNC slice control"),
        this);
    waterfall3DVncSliceInputCheckbox->setChecked(replay3DVncSliceInput);
    waterfall3DVncSliceInputCheckbox->setToolTip(replayText(
        "vnc_slice_input_tooltip",
        "Use left/right mouse buttons for frequency/time slices without keyboard modifiers. Disable it to restore normal mouse actions."));
    connect(waterfall3DVncSliceInputCheckbox,
            &QCheckBox::toggled,
            this,
            [this](bool checked) {
                replay3DVncSliceInput = checked;
                if (waterfall3DView) {
                    waterfall3DView->setModifierFreeSliceInput(checked);
                }
                schedulePersistentUiSettingsSave();
            });

    waterfallAreaRulerCheckbox = new QCheckBox(
        replayText("area_ruler", "Area ruler"), this);
    waterfallAreaRulerCheckbox->setToolTip(replayText(
        "area_ruler_tooltip",
        "Drag over the 2D replay waterfall to measure frequency span and elapsed time."));
    connect(waterfallAreaRulerCheckbox,
            &QCheckBox::toggled,
            this,
            [this](bool checked) {
                replayAreaRulerEnabled = checked;
                if (!checked) replayAreaRulerDragging = false;
                schedulePersistentUiSettingsSave();
                renderWaterfallPixmap();
            });

    QHBoxLayout *controlsLayout = new QHBoxLayout();
    controlsLayout->setContentsMargins(0, 0, 0, 0);
    controlsLayout->setSpacing(4);
    controlsLayout->addWidget(openButton);
    controlsLayout->addWidget(playButton);
    controlsLayout->addWidget(speedCombo);
    controlsLayout->addWidget(new QLabel(replayText("loupe", "Loupe"), this));
    controlsLayout->addWidget(rowHeightSlider, 1);
    controlsLayout->addWidget(rowHeightValueLabel);
    controlsLayout->addWidget(new QLabel(replayText("contrast", "Contrast"), this));
    controlsLayout->addWidget(contrastSlider, 1);
    controlsLayout->addWidget(contrastValueLabel);
    controlsLayout->addWidget(new QLabel(replayText("sensitivity", "Sensitivity"), this));
    controlsLayout->addWidget(sensitivitySlider, 1);
    controlsLayout->addWidget(sensitivityValueLabel);
    controlsLayout->addWidget(new QLabel(replayText("min", "Min"), this));
    controlsLayout->addWidget(levelMinSpin);
    controlsLayout->addWidget(new QLabel(replayText("max", "Max"), this));
    controlsLayout->addWidget(levelMaxSpin);

    QGridLayout *listenLayout = new QGridLayout();
    listenLayout->setContentsMargins(0, 0, 0, 0);
    listenLayout->setHorizontalSpacing(4);
    listenLayout->setVerticalSpacing(2);
    listenLayout->addWidget(new QLabel(replayText("replay_audio", "Replay audio"), this), 0, 0);
    listenLayout->addWidget(replayIqSourceCombo, 0, 1);
    listenLayout->addWidget(listenFrequencyControl, 0, 2, 2, 1);
    listenLayout->addWidget(replayAudioCheckbox, 0, 3);
    listenLayout->addWidget(linkedIqLabel, 0, 4);
    listenLayout->addWidget(new QLabel(replayText("zoom", "Zoom"), this), 1, 3);
    listenLayout->addWidget(zoomSlider, 1, 4);
    listenLayout->addWidget(zoomValueLabel, 1, 5);
    listenLayout->setColumnStretch(2, 2);
    listenLayout->setColumnStretch(4, 3);

    QHBoxLayout *displayLayout = new QHBoxLayout();
    displayLayout->setContentsMargins(0, 0, 0, 0);
    displayLayout->setSpacing(4);
    displayLayout->addWidget(new QLabel(replayText("display_mode", "Display"), this));
    displayLayout->addWidget(waterfallDisplayModeCombo);
    displayLayout->addWidget(new QLabel(replayText("resolution", "3D resolution"), this));
    displayLayout->addWidget(waterfall3DResolutionCombo);
    displayLayout->addWidget(waterfallAreaRulerCheckbox);
    displayLayout->addWidget(new QLabel(replayText("slice_step", "Slice step"), this));
    displayLayout->addWidget(waterfall3DSliceStepSpin);
    displayLayout->addWidget(new QLabel(replayText("slice_width", "Slice width"), this));
    displayLayout->addWidget(waterfall3DSliceWidthSpin);
    displayLayout->addStretch(1);

    QHBoxLayout *spectrumSliceLayout = new QHBoxLayout();
    spectrumSliceLayout->setContentsMargins(0, 0, 0, 0);
    spectrumSliceLayout->setSpacing(4);
    spectrumSliceLayout->addWidget(new QLabel(replayText("spectrum_slice_step", "Spectrum slice step"), this));
    spectrumSliceLayout->addWidget(waterfall3DSpectrumSliceStepSpin);
    spectrumSliceLayout->addWidget(new QLabel(replayText("spectrum_slice_rows", "Spectrum slice rows"), this));
    spectrumSliceLayout->addWidget(waterfall3DSpectrumSliceRowsSpin);
    spectrumSliceLayout->addWidget(waterfall3DSpectrumSliceCaptureCheckbox);
    spectrumSliceLayout->addWidget(waterfall3DSpectrumSliceCaptureFixedCheckbox);
    spectrumSliceLayout->addWidget(waterfall3DVncSliceInputCheckbox);
    spectrumSliceLayout->addStretch(1);

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->addWidget(graphScroll, 2);
    layout->addWidget(scaleScroll);
    layout->addWidget(waterfall3DView, 3);
    layout->addWidget(waterfallScroll, 3);
    layout->addLayout(controlsLayout);
    layout->addLayout(listenLayout);
    layout->addLayout(displayLayout);
    layout->addLayout(spectrumSliceLayout);
    layout->addWidget(timelineSlider);
    layout->addWidget(infoLabel);
    setLayout(layout);
    if (!isolatedExperiment) loadPersistentUiSettings();
    else openButton->setEnabled(false);
    updateSliderValueLabels();
    updateReplayDisplayMode();
}

SpectrumFrameReplayDialog::~SpectrumFrameReplayDialog() {
    if (imageCancel) imageCancel->store(true);
    saveSettingsTimer.stop();
    savePersistentUiSettings();
    resetLocalIqPlaybackState(false);
}

bool SpectrumFrameReplayDialog::eventFilter(QObject *watched, QEvent *event) {
    if (watched == waterfallLabel && event && replayAreaRulerEnabled &&
        (event->type() == QEvent::MouseButtonPress ||
         event->type() == QEvent::MouseMove ||
         event->type() == QEvent::MouseButtonRelease)) {
        auto *mouseEvent = static_cast<QMouseEvent *>(event);
        if (event->type() == QEvent::MouseButtonPress && mouseEvent->button() == Qt::LeftButton) {
            replayAreaRulerDragging = true;
            replayAreaRulerStart = mouseEvent->pos();
            replayAreaRulerEnd = mouseEvent->pos();
            renderWaterfallPixmap();
            return true;
        }
        if (event->type() == QEvent::MouseMove && replayAreaRulerDragging) {
            replayAreaRulerEnd = mouseEvent->pos();
            renderWaterfallPixmap();
            return true;
        }
        if (event->type() == QEvent::MouseButtonRelease &&
            mouseEvent->button() == Qt::LeftButton && replayAreaRulerDragging) {
            replayAreaRulerEnd = mouseEvent->pos();
            replayAreaRulerDragging = false;
            renderWaterfallPixmap();
            return true;
        }
    }
    if ((watched == graph || watched == waterfallLabel) &&
        event &&
        (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseMove)) {
        auto *mouseEvent = static_cast<QMouseEvent *>(event);
        const bool leftPressed =
            event->type() == QEvent::MouseButtonPress
                ? mouseEvent->button() == Qt::LeftButton
                : (mouseEvent->buttons() & Qt::LeftButton);
        if (leftPressed &&
            selectedReplayIqSource() == ReplayIqSource::FullIq) {
            const int localWidth = watched == graph
                                       ? (graph ? graph->width() : 0)
                                       : (waterfallLabel ? waterfallLabel->width() : 0);
            setReplayListenFrequency(frequencyForLocalX(mouseEvent->x(), localWidth), true);
            mouseEvent->accept();
            return true;
        }
    }
    return QDialog::eventFilter(watched, event);
}

void SpectrumFrameReplayDialog::resizeEvent(QResizeEvent *event) {
    QDialog::resizeEvent(event);
    if (waterfallPauseButton && waterfallScroll && waterfallScroll->viewport()) {
        waterfallPauseButton->move((std::max)(2, waterfallScroll->viewport()->width() -
                                                   waterfallPauseButton->width() - 6),
                                   6);
        waterfallPauseButton->raise();
    }
    scheduleDeferredRender();
}

void SpectrumFrameReplayDialog::showEvent(QShowEvent *event) {
    QDialog::showEvent(event);
    scheduleDeferredRender();
}

void SpectrumFrameReplayDialog::openRecordingFromDialog() {
    const QString startDir = !recordingPath.isEmpty()
                                 ? QFileInfo(recordingPath).absolutePath()
                                 : QDir(QCoreApplication::applicationDirPath())
                                       .filePath(QStringLiteral("recordings/spectrum"));
    const QString path =
        QFileDialog::getOpenFileName(this,
                                     replayText("title", "Spectrum replay"),
                                     startDir,
                                     QStringLiteral("Fobos spectrum frames (*.fbspec);;All files (*.*)"));
    if (path.isEmpty()) {
        return;
    }

    QString errorMessage;
    if (!loadRecording(path, &errorMessage)) {
        QMessageBox::warning(this,
                             replayText("title", "Spectrum replay"),
                             QStringLiteral("Could not open spectrum recording:\n%1").arg(errorMessage));
    }
}

void SpectrumFrameReplayDialog::loadPersistentUiSettings() {
    QSettings settings(persistentSettingsFilePath(), QSettings::IniFormat);
    levelMin = static_cast<float>((std::clamp)(
        settings.value(QStringLiteral("spectrumReplay/levelMin"), static_cast<int>(levelMin)).toInt(),
        -180,
        20));
    levelMax = static_cast<float>((std::clamp)(
        settings.value(QStringLiteral("spectrumReplay/levelMax"), static_cast<int>(levelMax)).toInt(),
        -180,
        20));
    if (levelMax <= levelMin) {
        levelMax = levelMin + 1.0f;
    }
    waterfallZoomPercent = (std::clamp)(
        settings.value(QStringLiteral("spectrumReplay/zoomPercent"), waterfallZoomPercent).toInt(),
        100,
        400);
    waterfallRowHeight = (std::clamp)(
        settings.value(QStringLiteral("spectrumReplay/rowHeight"), waterfallRowHeight).toInt(),
        1,
        16);
    replayContrast = static_cast<float>((std::clamp)(
        settings.value(QStringLiteral("spectrumReplay/contrast"), static_cast<int>(replayContrast)).toInt(),
        1,
        20));
    replaySensitivity = static_cast<float>((std::clamp)(
        settings.value(QStringLiteral("spectrumReplay/sensitivity"), static_cast<int>(replaySensitivity)).toInt(),
        1,
        30));
    replayWaterfallDisplayMode = (std::clamp)(
        settings.value(QStringLiteral("spectrumReplay/displayMode"), replayWaterfallDisplayMode).toInt(),
        0,
        2);
    replay3DResolutionDivisor =
        settings.value(QStringLiteral("spectrumReplay/3dResolutionDivisor"),
                       replay3DResolutionDivisor)
            .toInt();
    if (replay3DResolutionDivisor != 1 && replay3DResolutionDivisor != 2 &&
        replay3DResolutionDivisor != 4 && replay3DResolutionDivisor != 8 &&
        replay3DResolutionDivisor != 16 && replay3DResolutionDivisor != 32 &&
        replay3DResolutionDivisor != 64) {
        replay3DResolutionDivisor = 4;
    }
    replay3DSliceStep = (std::clamp)(
        settings.value(QStringLiteral("spectrumReplay/3dSliceStep"), replay3DSliceStep).toInt(),
        1,
        256);
    replay3DSliceWidth = (std::clamp)(
        settings.value(QStringLiteral("spectrumReplay/3dSliceWidth"), replay3DSliceWidth).toInt(),
        1,
        4096);
    replay3DSpectrumSliceStep = (std::clamp)(
        settings.value(QStringLiteral("spectrumReplay/3dSpectrumSliceStep"),
                       replay3DSpectrumSliceStep).toInt(),
        1,
        2048);
    replay3DSpectrumSliceRows = (std::clamp)(
        settings.value(QStringLiteral("spectrumReplay/3dSpectrumSliceRows"),
                       replay3DSpectrumSliceRows).toInt(),
        1,
        2048);
    replay3DSpectrumSliceCapture =
        settings.value(QStringLiteral("spectrumReplay/3dSpectrumSliceCapture"),
                       replay3DSpectrumSliceCapture).toBool();
    replay3DSpectrumSliceCaptureFixed =
        settings.value(QStringLiteral("spectrumReplay/3dSpectrumSliceCaptureFixed"),
                       replay3DSpectrumSliceCaptureFixed).toBool();
    replay3DVncSliceInput =
        settings.value(QStringLiteral("spectrumReplay/3dVncSliceInput"),
                       replay3DVncSliceInput).toBool();
    replayAreaRulerEnabled =
        settings.value(QStringLiteral("spectrumReplay/areaRuler"),
                       replayAreaRulerEnabled).toBool();

    if (levelMinSpin) {
        QSignalBlocker blocker(levelMinSpin);
        levelMinSpin->setValue(static_cast<int>(levelMin));
    }
    if (levelMaxSpin) {
        QSignalBlocker blocker(levelMaxSpin);
        levelMaxSpin->setValue(static_cast<int>(levelMax));
    }
    if (zoomSlider) {
        QSignalBlocker blocker(zoomSlider);
        zoomSlider->setValue(waterfallZoomPercent);
    }
    if (rowHeightSlider) {
        QSignalBlocker blocker(rowHeightSlider);
        rowHeightSlider->setValue(waterfallRowHeight);
    }
    if (contrastSlider) {
        QSignalBlocker blocker(contrastSlider);
        contrastSlider->setValue(static_cast<int>(std::lround(replayContrast)));
    }
    if (sensitivitySlider) {
        QSignalBlocker blocker(sensitivitySlider);
        sensitivitySlider->setValue(static_cast<int>(std::lround(replaySensitivity)));
    }
    if (waterfallDisplayModeCombo) {
        const int index = waterfallDisplayModeCombo->findData(replayWaterfallDisplayMode);
        if (index >= 0) {
            QSignalBlocker blocker(waterfallDisplayModeCombo);
            waterfallDisplayModeCombo->setCurrentIndex(index);
        }
    }
    if (waterfall3DResolutionCombo) {
        const int index = waterfall3DResolutionCombo->findData(replay3DResolutionDivisor);
        if (index >= 0) {
            QSignalBlocker blocker(waterfall3DResolutionCombo);
            waterfall3DResolutionCombo->setCurrentIndex(index);
        }
    }
    if (waterfall3DSliceStepSpin) {
        QSignalBlocker blocker(waterfall3DSliceStepSpin);
        waterfall3DSliceStepSpin->setValue(replay3DSliceStep);
    }
    if (waterfall3DSliceWidthSpin) {
        QSignalBlocker blocker(waterfall3DSliceWidthSpin);
        waterfall3DSliceWidthSpin->setValue(replay3DSliceWidth);
    }
    if (waterfall3DSpectrumSliceStepSpin) {
        QSignalBlocker blocker(waterfall3DSpectrumSliceStepSpin);
        waterfall3DSpectrumSliceStepSpin->setValue(replay3DSpectrumSliceStep);
    }
    if (waterfall3DSpectrumSliceRowsSpin) {
        QSignalBlocker blocker(waterfall3DSpectrumSliceRowsSpin);
        waterfall3DSpectrumSliceRowsSpin->setValue(replay3DSpectrumSliceRows);
    }
    if (waterfall3DSpectrumSliceCaptureCheckbox) {
        QSignalBlocker blocker(waterfall3DSpectrumSliceCaptureCheckbox);
        waterfall3DSpectrumSliceCaptureCheckbox->setChecked(replay3DSpectrumSliceCapture);
    }
    if (waterfall3DSpectrumSliceCaptureFixedCheckbox) {
        QSignalBlocker blocker(waterfall3DSpectrumSliceCaptureFixedCheckbox);
        waterfall3DSpectrumSliceCaptureFixedCheckbox->setChecked(replay3DSpectrumSliceCaptureFixed);
        waterfall3DSpectrumSliceCaptureFixedCheckbox->setEnabled(replay3DSpectrumSliceCapture);
    }
    if (waterfall3DVncSliceInputCheckbox) {
        QSignalBlocker blocker(waterfall3DVncSliceInputCheckbox);
        waterfall3DVncSliceInputCheckbox->setChecked(replay3DVncSliceInput);
    }
    if (waterfallAreaRulerCheckbox) {
        QSignalBlocker blocker(waterfallAreaRulerCheckbox);
        waterfallAreaRulerCheckbox->setChecked(replayAreaRulerEnabled);
    }
    if (waterfall3DView) {
        waterfall3DView->setResolutionDivisor(replay3DResolutionDivisor);
        waterfall3DView->setSliceScrollStep(replay3DSliceStep);
        waterfall3DView->setSliceWidth(replay3DSliceWidth);
        waterfall3DView->setSpectrumSliceScrollStep(replay3DSpectrumSliceStep);
        waterfall3DView->setSpectrumSliceWidth(replay3DSpectrumSliceRows);
        waterfall3DView->setSpectrumSliceCapture(replay3DSpectrumSliceCapture);
        waterfall3DView->setSpectrumSliceCaptureFixed(replay3DSpectrumSliceCaptureFixed);
        waterfall3DView->setModifierFreeSliceInput(replay3DVncSliceInput);
    }
    if (speedCombo) {
        const double savedSpeed =
            settings.value(QStringLiteral("spectrumReplay/speed"), 1.0).toDouble();
        const int speedIndex = speedCombo->findData(savedSpeed);
        if (speedIndex >= 0) {
            QSignalBlocker blocker(speedCombo);
            speedCombo->setCurrentIndex(speedIndex);
        }
    }
    updateSliderValueLabels();
}

void SpectrumFrameReplayDialog::savePersistentUiSettings() const {
    if (isolatedExperiment) return;
    QSettings settings(persistentSettingsFilePath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("spectrumReplay/levelMin"), static_cast<int>(levelMin));
    settings.setValue(QStringLiteral("spectrumReplay/levelMax"), static_cast<int>(levelMax));
    settings.setValue(QStringLiteral("spectrumReplay/zoomPercent"), waterfallZoomPercent);
    settings.setValue(QStringLiteral("spectrumReplay/rowHeight"), waterfallRowHeight);
    settings.setValue(QStringLiteral("spectrumReplay/contrast"), static_cast<int>(std::lround(replayContrast)));
    settings.setValue(QStringLiteral("spectrumReplay/sensitivity"), static_cast<int>(std::lround(replaySensitivity)));
    settings.setValue(QStringLiteral("spectrumReplay/displayMode"), replayWaterfallDisplayMode);
    settings.setValue(QStringLiteral("spectrumReplay/3dResolutionDivisor"), replay3DResolutionDivisor);
    settings.setValue(QStringLiteral("spectrumReplay/3dSliceStep"), replay3DSliceStep);
    settings.setValue(QStringLiteral("spectrumReplay/3dSliceWidth"), replay3DSliceWidth);
    settings.setValue(QStringLiteral("spectrumReplay/3dSpectrumSliceStep"), replay3DSpectrumSliceStep);
    settings.setValue(QStringLiteral("spectrumReplay/3dSpectrumSliceRows"), replay3DSpectrumSliceRows);
    settings.setValue(QStringLiteral("spectrumReplay/3dSpectrumSliceCapture"), replay3DSpectrumSliceCapture);
    settings.setValue(QStringLiteral("spectrumReplay/3dSpectrumSliceCaptureFixed"), replay3DSpectrumSliceCaptureFixed);
    settings.setValue(QStringLiteral("spectrumReplay/3dVncSliceInput"), replay3DVncSliceInput);
    settings.setValue(QStringLiteral("spectrumReplay/areaRuler"), replayAreaRulerEnabled);
    if (speedCombo) {
        settings.setValue(QStringLiteral("spectrumReplay/speed"),
                          speedCombo->currentData().toDouble());
    }
}

void SpectrumFrameReplayDialog::schedulePersistentUiSettingsSave() {
    if (isolatedExperiment) return;
    if (saveSettingsTimer.isActive()) {
        saveSettingsTimer.start();
    } else {
        saveSettingsTimer.start(750);
    }
}

void SpectrumFrameReplayDialog::updateSliderValueLabels() {
    if (zoomValueLabel) {
        zoomValueLabel->setText(QStringLiteral("%1%").arg(waterfallZoomPercent));
    }
    if (rowHeightValueLabel) {
        rowHeightValueLabel->setText(QString::number(waterfallRowHeight));
    }
    if (contrastValueLabel) {
        contrastValueLabel->setText(QString::number(static_cast<int>(std::lround(replayContrast))));
    }
    if (sensitivityValueLabel) {
        sensitivityValueLabel->setText(QString::number(static_cast<int>(std::lround(replaySensitivity))));
    }
}

bool SpectrumFrameReplayDialog::loadRecording(const QString &path, QString *errorMessage) {
    if (isolatedExperiment) {
        if (errorMessage) *errorMessage = "Bundle replay requires verified recording data";
        return false;
    }
    playbackTimer.stop();
    resetLocalIqPlaybackState(false);
    if (playButton) {
        QSignalBlocker blocker(playButton);
        playButton->setChecked(false);
        playButton->setText(replayText("play", "Play"));
    }

    SpectrumFrameRecording loaded;
    if (!SpectrumFrameRecorder::loadFile(path, loaded, errorMessage)) {
        return false;
    }

    adoptRecording(path, std::move(loaded));
    return true;
}

bool SpectrumFrameReplayDialog::loadExperimentRecording(const QString &path, SpectrumFrameRecording loaded,
    const QJsonObject &context, QString *errorMessage, const QStringList &verifiedAssetPaths) {
    if (!isolatedExperiment || loaded.frames.empty() || !recording->frames.empty()) {
        if (errorMessage) *errorMessage = "A fresh isolated replay and a nonempty recording are required";
        return false;
    }
    setPlaybackEnabled(false);
    recording->metadata = loaded.metadata;
    if (!applyExperimentContext(context, errorMessage)) return false;
    verifiedReplayAssets.clear();
    for (const auto &asset : verifiedAssetPaths) {
        const auto canonical = QFileInfo(asset).canonicalFilePath();
        if (!canonical.isEmpty()) verifiedReplayAssets.insert(canonical);
    }
    adoptRecording(path, std::move(loaded));
    return true;
}

bool SpectrumFrameReplayDialog::applyExperimentContext(const QJsonObject &context, QString *errorMessage) {
    if (!isolatedExperiment) {
        if (errorMessage) *errorMessage = "Experiment context requires an isolated replay instance";
        return false;
    }
    const auto scientific = recording->metadata.value("scientificMetadata").toObject();
    const auto recorded = scientific.value("experimentContext").toObject();
    const auto start = scientific.value("start").toObject();
    const auto radio = start.value("radio").toObject(), analysis = start.value("analysis").toObject();
    const auto effective = ExperimentBundle::sanitizeContext(recorded.isEmpty() ? context : recorded);
    auto display = (recorded.isEmpty() ? context : recorded).value("display").toObject();
    // Old files still carry authoritative recording-time display limits.
    if (recorded.isEmpty()) {
        if (analysis.contains("displayLevelMinDb")) display["levelMin"] = analysis.value("displayLevelMinDb");
        if (analysis.contains("displayLevelMaxDb")) display["levelMax"] = analysis.value("displayLevelMaxDb");
    }
    QString invalid;
    const auto number = [&invalid](const QJsonObject &o, const char *key, double fallback, double low, double high, bool integer = false) {
        if (!o.contains(key)) return fallback;
        const auto v = o.value(key); const double n = v.toDouble();
        if (!v.isDouble() || !std::isfinite(n) || n < low || n > high || (integer && n != std::floor(n))) {
            invalid = QStringLiteral("Invalid experiment value: %1").arg(QString::fromLatin1(key)); return fallback;
        }
        return n;
    };
    const double minimum = number(display, "levelMin", -140, -180, 20);
    const double maximum = number(display, "levelMax", -30, -180, 20);
    const double contrast = number(display, "contrast", 10, 1, 20);
    const double sensitivity = number(display, "sensitivity", 10, 1, 30);
    const int mode = int(number(display, "waterfallDisplayMode", 0, 0, 2, true));
    const int requestedRows = int(number(display, "waterfallRowsPerFrame", 4, 1, 100000, true));
    const int rows = std::clamp(requestedRows, rowHeightSlider->minimum(), rowHeightSlider->maximum());
    const int requestedResolution = int(number(display, "waterfall3DResolutionDivisor", 4, 1, 100000, true));
    const int resolution = waterfall3DResolutionCombo->findData(requestedResolution) >= 0 ? requestedResolution : 4;
    if (minimum >= maximum) invalid = "Experiment display minimum must be below maximum";
    if (display.contains("coloredSpectrum") && !display.value("coloredSpectrum").isBool()) invalid = "Invalid experiment spectrum color flag";

    // Export-time audio settings are not treated as the original acquisition.
    auto settings = recorded.isEmpty() ? QJsonObject() : effective.value("settings").toObject();
    const std::pair<const char *, const char *> radioKeys[] = {
        {"sampleRateHz", "sampleRate"}, {"centerFrequencyHz", "centerFrequency"},
        {"actualFrequencyHz", "actualFrequency"}, {"listeningFrequencyHz", "listeningFrequency"},
        {"bandwidthHz", "bandwidth"}, {"inputMode", "inputMode"}, {"modulationType", "modulationType"}};
    for (const auto &key : radioKeys) if (radio.contains(key.first)) settings[key.second] = radio.value(key.first);
    for (const auto *key : {"centerFrequency", "actualFrequency", "listeningFrequency", "bandwidth", "inputMode", "modulationType", "fftLength", "fftWindowType"})
        if (recording->metadata.contains(key)) settings[key] = recording->metadata.value(key);
    if (recording->metadata.contains("sourceSampleRate")) settings["sampleRate"] = recording->metadata.value("sourceSampleRate");
    QJsonObject safeSettings;
    for (const auto *key : {"centerFrequency", "actualFrequency", "listeningFrequency"})
        safeSettings[key] = number(settings, key, 100000000, 0, 1.0e12);
    safeSettings["sampleRate"] = number(settings, "sampleRate", 50000000, 1, 1.0e9);
    safeSettings["bandwidth"] = number(settings, "bandwidth", 200000, 1, 1.0e9);
    safeSettings["inputMode"] = number(settings, "inputMode", 0, 0, 4, true);
    safeSettings["modulationType"] = number(settings, "modulationType", MOD_WFM, MOD_AM, MOD_DMR, true);
    for (const auto *key : {"audioLowPassHz", "audioHighPassHz"}) safeSettings[key] = number(settings, key, 0, 0, 24000);
    if (settings.value("audioFilterChainJson").isString() && settings.value("audioFilterChainJson").toString().size() <= 65536)
        safeSettings["audioFilterChainJson"] = settings.value("audioFilterChainJson");
    if (!invalid.isEmpty()) { if (errorMessage) *errorMessage = invalid; return false; }

    setPlaybackEnabled(false);
    saveSettingsTimer.stop();
    experimentSettings = safeSettings;
    experimentDisplayAdjusted = requestedRows != rows || requestedResolution != resolution;
    experimentCalibration = analysis;
    experimentContextSource = recorded.isEmpty() ? "export-time-fallback; recording header authoritative" : "recording-start";
    levelMin = float(minimum); levelMax = float(maximum); replayContrast = float(contrast); replaySensitivity = float(sensitivity);
    replayColoredSpectrum = display.value("coloredSpectrum").toBool(true);
    replayWaterfallDisplayMode = mode; replay3DResolutionDivisor = resolution; waterfallRowHeight = rows;
    replayListenFrequencyHz = safeSettings.value("listeningFrequency").toDouble();
    const auto setSpin = [](QSpinBox *control, int v) { QSignalBlocker blocker(control); control->setValue(v); };
    const auto setSlider = [](QSlider *control, int v) { QSignalBlocker blocker(control); control->setValue(v); };
    setSpin(levelMinSpin, int(std::lround(minimum))); setSpin(levelMaxSpin, int(std::lround(maximum)));
    setSlider(contrastSlider, int(std::lround(contrast))); setSlider(sensitivitySlider, int(std::lround(sensitivity))); setSlider(rowHeightSlider, rows);
    { QSignalBlocker b(waterfallDisplayModeCombo); waterfallDisplayModeCombo->setCurrentIndex(waterfallDisplayModeCombo->findData(mode)); }
    { QSignalBlocker b(waterfall3DResolutionCombo); waterfall3DResolutionCombo->setCurrentIndex(waterfall3DResolutionCombo->findData(resolution)); }
    { QSignalBlocker b(replayAudioCheckbox); replayAudioCheckbox->setChecked(false); }
    updateSliderValueLabels(); updateReplayDisplayMode(); rebuildWaterfallImage(); updateFrameSelection(selectedFrame);
    return true;
}

QJsonObject SpectrumFrameReplayDialog::experimentReplayState() const {
    return {{"display", QJsonObject{{"levelMin", levelMin}, {"levelMax", levelMax}, {"contrast", replayContrast},
        {"sensitivity", replaySensitivity}, {"coloredSpectrum", replayColoredSpectrum}, {"waterfallDisplayMode", replayWaterfallDisplayMode},
        {"waterfallRowsPerFrame", waterfallRowHeight}, {"waterfall3DResolutionDivisor", replay3DResolutionDivisor}}},
        {"settings", experimentSettings}, {"calibrationProvenance", experimentCalibration}, {"contextSource", experimentContextSource},
        {"paused", !playbackTimer.isActive()}, {"persistentSettingsEnabled", !isolatedExperiment},
        {"storedAmplitudesUnchanged", true}, {"displayAdjustedToReplayLimits", experimentDisplayAdjusted},
        {"firstStoredMagnitude", recording->frames.empty() || recording->frames.front().magnitudes.empty() ? QJsonValue() : QJsonValue(recording->frames.front().magnitudes.front())},
        {"frameCount", double(recording->frames.size())}, {"viewportRasterPixels", experimentViewportPixels},
        {"sharedRecordingBacking", experimentFrames && experimentFrames.get() == recording.get()},
        {"channelIqAvailable", replayIqSourceAvailable(ReplayIqSource::ChannelIq)},
        {"fullIqAvailable", replayIqSourceAvailable(ReplayIqSource::FullIq)}};
}

void SpectrumFrameReplayDialog::rebuildExperimentWaterfall() {
    if (!experimentFrames || experimentFrames->frames.empty()) return;
    if (imageWatcher) { experimentImageDirty = true; imageCancel->store(true); return; }
    experimentImageDirty = false;
    imageCancel = std::make_shared<std::atomic_bool>(false);
    const auto cancel = imageCancel;
    const auto frames = experimentFrames;
    const float minimum = levelMin, maximum = levelMax, contrast = replayContrast, sensitivity = replaySensitivity;
    auto *watcher = new QFutureWatcher<QImage>(this); imageWatcher = watcher;
    connect(watcher, &QFutureWatcher<QImage>::finished, this, [this, watcher, cancel] {
        QImage image;
        try { image = watcher->result(); } catch (...) { image = QImage(); }
        imageWatcher = nullptr; watcher->deleteLater();
        if (!cancel->load()) {
            waterfallImage = image; scaledWaterfallDirty = true;
            if (image.isNull()) linkedIqLabel->setText(replayText("experiment_image_failed", "Replay image allocation failed; reduce the replay budget."));
            renderWaterfallPixmap();
            if (replayWaterfallDisplayMode != 0) rebuild3DWindow(true);
        }
        if (experimentImageDirty) rebuildExperimentWaterfall();
    });
    watcher->setFuture(QtConcurrent::run([frames, cancel, minimum, maximum, contrast, sensitivity] {
        try {
        const int width = int(frames->frames.front().magnitudes.size()), height = int(frames->frames.size());
        QImage image(width, height, QImage::Format_RGB888);
        if (image.isNull()) return image;
        for (int y = 0; y < height; ++y) {
            if (cancel->load()) return QImage();
            const auto &values = frames->frames[std::size_t(y)].magnitudes;
            auto *row = image.scanLine(y);
            for (int x = 0; x < width; ++x) {
                const auto shifted = values.empty() ? 0 : (std::size_t(x)+values.size()/2)%values.size();
                const auto color = colorForLevel(values.empty() ? minimum : values[shifted], minimum, maximum, contrast, sensitivity);
                row[x*3] = uchar(color.red()); row[x*3+1] = uchar(color.green()); row[x*3+2] = uchar(color.blue());
            }
        }
        return image;
        } catch (...) { return QImage(); }
    }));
}

void SpectrumFrameReplayDialog::adoptRecording(const QString &path, SpectrumFrameRecording loaded) {
    recording = std::make_shared<SpectrumFrameRecording>(std::move(loaded));
    if (isolatedExperiment) experimentFrames = recording;
    recordingPath = path;
    linkedChannelIqPath = recordingPath;
    if (linkedChannelIqPath.endsWith(QStringLiteral("_spectrum.fbspec"))) {
        linkedChannelIqPath.chop(QStringLiteral("_spectrum.fbspec").size());
        linkedChannelIqPath += QStringLiteral("_channel_iq.wav");
    } else if (linkedChannelIqPath.endsWith(QStringLiteral(".fbspec"))) {
        linkedChannelIqPath.chop(QStringLiteral(".fbspec").size());
        linkedChannelIqPath += QStringLiteral("_channel_iq.wav");
    }
    linkedFullIqPath = recordingPath;
    if (linkedFullIqPath.endsWith(QStringLiteral("_spectrum.fbspec"))) {
        linkedFullIqPath.chop(QStringLiteral("_spectrum.fbspec").size());
        linkedFullIqPath += QStringLiteral("_full_iq_s8.iq8");
    } else if (linkedFullIqPath.endsWith(QStringLiteral(".fbspec"))) {
        linkedFullIqPath.chop(QStringLiteral(".fbspec").size());
        linkedFullIqPath += QStringLiteral("_full_iq_s8.iq8");
    }
    replayListenFrequencyHz = isolatedExperiment ? experimentSettings.value("listeningFrequency").toDouble()
        : recording->metadata.value(QStringLiteral("listeningFrequency")).toDouble(0.0);
    if (replayListenFrequencyHz <= 0.0 && !recording->frames.empty()) {
        replayListenFrequencyHz = recording->frames.front().centerFrequency;
    }
    selectedFrame = 0;
    replay3DWindowStart = -1;
    replay3DWindowEnd = -1;
    if (timelineSlider) {
        timelineSlider->setRange(0, static_cast<int>(recording->frames.size()) - 1);
        timelineSlider->setValue(0);
    }
    updateLinkedIqControls();
    rebuildWaterfallImage();
    updateFrameSelection(0);
    updateReplayMarker();
    setWindowTitle(QStringLiteral("%1 - %2")
                       .arg(replayText("title", "Spectrum replay"),
                            QFileInfo(recordingPath).fileName()));
    qDebug() << "[SpectrumReplay] loaded"
             << QFileInfo(recordingPath).fileName()
             << "frames" << static_cast<int>(recording->frames.size())
             << "firstUtc" << (recording->frames.empty() ? 0 : recording->frames.front().utcMs)
             << "lastElapsedMs" << (recording->frames.empty() ? 0 : recording->frames.back().elapsedMs)
             << "channelIq" << QFileInfo(linkedChannelIqPath).exists()
             << "fullIq" << QFileInfo(linkedFullIqPath).exists();
}

void SpectrumFrameReplayDialog::rebuildWaterfallImage() {
    if (isolatedExperiment) { rebuildExperimentWaterfall(); return; }
    if (recording->frames.empty()) {
        waterfallImage = QImage();
        scaledWaterfallCache = QImage();
        scaledWaterfallCacheWidth = 0;
        scaledWaterfallDirty = true;
        waterfallLabel->clear();
        if (waterfall3DView) {
            waterfall3DView->clearHistory();
        }
        replay3DWindowStart = -1;
        replay3DWindowEnd = -1;
        return;
    }

    const int width = static_cast<int>(recording->frames.front().magnitudes.size());
    const int height = static_cast<int>(recording->frames.size());
    if (width <= 0 || height <= 0) {
        return;
    }

    waterfallImage = QImage(width, height, QImage::Format_RGB888);
    for (int y = 0; y < height; ++y) {
        const SpectrumFrameRecord &frame = recording->frames[static_cast<std::size_t>(y)];
        const int frameBins = static_cast<int>(frame.magnitudes.size());
        for (int x = 0; x < width; ++x) {
            const int shiftedIndex = frameBins > 0 ? (x + frameBins / 2) % frameBins : x;
            const float value = shiftedIndex < frameBins
                                    ? frame.magnitudes[static_cast<std::size_t>(shiftedIndex)]
                                    : levelMin;
            waterfallImage.setPixelColor(x,
                                         y,
                                         colorForLevel(value,
                                                       levelMin,
                                                       levelMax,
                                                       replayContrast,
                                                       replaySensitivity));
        }
    }

    waterfallLabel->setPixmap(QPixmap::fromImage(waterfallImage));
    scaledWaterfallDirty = true;
    renderWaterfallPixmap();
    if (replayWaterfallDisplayMode != 0) {
        rebuild3DWindow(true);
    }
}

void SpectrumFrameReplayDialog::rebuild3DWindow(bool force) {
    if (!waterfall3DView || recording->frames.empty()) {
        return;
    }

    constexpr int kWindowCapacity = 128;
    const int frameCount = static_cast<int>(recording->frames.size());
    const bool cached = replay3DWindowStart >= 0 && replay3DWindowEnd >= replay3DWindowStart;
    const int edgeMargin = 16;
    if (!force && cached && selectedFrame >= replay3DWindowStart &&
        selectedFrame <= replay3DWindowEnd) {
        const bool nearLeadingEdge = selectedFrame < replay3DWindowStart + edgeMargin &&
                                     replay3DWindowStart > 0;
        const bool nearTrailingEdge = selectedFrame > replay3DWindowEnd - edgeMargin &&
                                      replay3DWindowEnd + 1 < frameCount;
        if (!nearLeadingEdge && !nearTrailingEdge) {
            waterfall3DView->setHighlightedHistoryRow(selectedFrame - replay3DWindowStart);
            waterfall3DView->setOverview(waterfallImage,
                                         replay3DWindowStart,
                                         replay3DWindowEnd,
                                         selectedFrame,
                                         frameCount);
            return;
        }
    }

    const int windowRows = std::min(kWindowCapacity, frameCount);
    replay3DWindowStart = std::clamp(selectedFrame - windowRows / 2,
                                     0,
                                     std::max(0, frameCount - windowRows));
    replay3DWindowEnd = std::min(frameCount - 1, replay3DWindowStart + windowRows - 1);

    waterfall3DView->clearHistory();
    waterfall3DView->setHistoryCapacity(windowRows);
    waterfall3DView->setResolutionDivisor(replay3DResolutionDivisor);
    waterfall3DView->setSliceScrollStep(replay3DSliceStep);
    waterfall3DView->setSliceWidth(replay3DSliceWidth);
    waterfall3DView->setSpectrumSliceScrollStep(replay3DSpectrumSliceStep);
    waterfall3DView->setSpectrumSliceWidth(replay3DSpectrumSliceRows);
    waterfall3DView->setSpectrumSliceCapture(replay3DSpectrumSliceCapture);
    waterfall3DView->setSpectrumSliceCaptureFixed(replay3DSpectrumSliceCaptureFixed);

    const int visibleWidth = std::max(320, waterfall3DView->width());
    const int baseColumns = std::clamp(visibleWidth * 2, 512, 4096);
    const int fixedOutputColumns = std::min(
        baseColumns,
        static_cast<int>(recording->frames.front().magnitudes.size()));
    if (fixedOutputColumns <= 0) {
        waterfall3DView->clearHistory();
        return;
    }
    for (int frameIndex = replay3DWindowStart;
         frameIndex <= replay3DWindowEnd;
         ++frameIndex) {
        const SpectrumFrameRecord &frame = recording->frames[static_cast<std::size_t>(frameIndex)];
        const int sourceBins = static_cast<int>(frame.magnitudes.size());
        const int outputColumns = fixedOutputColumns;
        replay3DLevels.assign(static_cast<std::size_t>(outputColumns), levelMin);
        replay3DColors.resize(static_cast<std::size_t>(outputColumns) * 3U);

        for (int column = 0; column < outputColumns; ++column) {
            const int begin = sourceBins > 0 ? column * sourceBins / outputColumns : 0;
            const int end = sourceBins > 0
                                ? std::max(begin + 1, (column + 1) * sourceBins / outputColumns)
                                : 0;
            double sum = 0.0;
            int count = 0;
            for (int source = begin; source < end && source < sourceBins; ++source) {
                const int shiftedIndex = (source + sourceBins / 2) % sourceBins;
                const float value = frame.magnitudes[static_cast<std::size_t>(shiftedIndex)];
                if (std::isfinite(value)) {
                    sum += value;
                    ++count;
                }
            }
            const float average = count > 0
                                      ? static_cast<float>(sum / static_cast<double>(count))
                                      : levelMin;
            replay3DLevels[static_cast<std::size_t>(column)] = average;
            const QColor color = colorForLevel(average,
                                               levelMin,
                                               levelMax,
                                               replayContrast,
                                               replaySensitivity);
            const std::size_t colorOffset = static_cast<std::size_t>(column) * 3U;
            replay3DColors[colorOffset] = static_cast<unsigned char>(color.red());
            replay3DColors[colorOffset + 1U] = static_cast<unsigned char>(color.green());
            replay3DColors[colorOffset + 2U] = static_cast<unsigned char>(color.blue());
        }
        waterfall3DView->appendHistoryRow(replay3DLevels,
                                          replay3DColors,
                                          levelMin,
                                          levelMax);
    }

    waterfall3DView->setHighlightedHistoryRow(selectedFrame - replay3DWindowStart);
    waterfall3DView->setOverview(waterfallImage,
                                 replay3DWindowStart,
                                 replay3DWindowEnd,
                                 selectedFrame,
                                 frameCount);
    waterfall3DView->update();
}

void SpectrumFrameReplayDialog::updateReplayDisplayMode() {
    const bool show3D = replayWaterfallDisplayMode != 0;
    if (waterfallScroll) {
        waterfallScroll->setVisible(!show3D);
    }
    if (waterfall3DView) {
        waterfall3DView->setVisible(show3D);
        waterfall3DView->setOverviewVisible(replayWaterfallDisplayMode == 2);
    }
    if (waterfall3DResolutionCombo) {
        waterfall3DResolutionCombo->setEnabled(show3D);
    }
    if (waterfall3DSliceStepSpin) {
        waterfall3DSliceStepSpin->setEnabled(show3D);
    }
    if (waterfall3DSliceWidthSpin) {
        waterfall3DSliceWidthSpin->setEnabled(show3D);
    }
    if (waterfall3DSpectrumSliceStepSpin) {
        waterfall3DSpectrumSliceStepSpin->setEnabled(show3D);
    }
    if (waterfall3DSpectrumSliceRowsSpin) {
        waterfall3DSpectrumSliceRowsSpin->setEnabled(show3D);
    }
    if (waterfall3DSpectrumSliceCaptureCheckbox) {
        waterfall3DSpectrumSliceCaptureCheckbox->setEnabled(show3D);
    }
    if (waterfall3DSpectrumSliceCaptureFixedCheckbox) {
        waterfall3DSpectrumSliceCaptureFixedCheckbox->setEnabled(
            show3D && replay3DSpectrumSliceCapture);
    }
    if (waterfall3DVncSliceInputCheckbox) {
        waterfall3DVncSliceInputCheckbox->setEnabled(show3D);
    }
    if (show3D) {
        replay3DWindowStart = -1;
        rebuild3DWindow(true);
    }
}

void SpectrumFrameReplayDialog::renderWaterfallPixmap() {
    if (isolatedExperiment) { renderExperimentWaterfallViewport(); return; }
    if (replayWaterfallPaused) return;
    if (waterfallImage.isNull()) {
        waterfallLabel->clear();
        return;
    }

    int viewportWidth =
        graphScroll && graphScroll->viewport()
            ? std::max(1, graphScroll->viewport()->width() - 2)
            : std::max(1, width() - 48);
    if (viewportWidth < 160 && width() > 320) {
        viewportWidth = std::max(1, width() - 48);
    }
    const int zoomedWidth =
        std::max(1, viewportWidth * std::max(100, waterfallZoomPercent) / 100);
    if (graph) {
        graph->setFixedWidth(zoomedWidth);
        graph->update();
    }
    if (scaleWidget) {
        scaleWidget->setFixedWidth(zoomedWidth);
        scaleWidget->update();
    }

    if (scaledWaterfallDirty ||
        scaledWaterfallCache.isNull() ||
        scaledWaterfallCacheWidth != zoomedWidth ||
        scaledWaterfallCache.height() != waterfallImage.height()) {
        scaledWaterfallCache = waterfallImage.scaled(zoomedWidth,
                                                     waterfallImage.height(),
                                                     Qt::IgnoreAspectRatio,
                                                     Qt::FastTransformation);
        scaledWaterfallCacheWidth = zoomedWidth;
        scaledWaterfallDirty = false;
    }
    const QImage &scaled = scaledWaterfallCache;
    const int loupeHeight = std::max(1, waterfallRowHeight);
    const int selectedY = std::clamp(selectedFrame, 0, std::max(0, scaled.height() - 1));
    QImage composed(zoomedWidth,
                    scaled.height() + loupeHeight - 1,
                    QImage::Format_RGB888);
    composed.fill(Qt::black);
    QPainter painter(&composed);
    if (selectedY > 0) {
        painter.drawImage(QRect(0, 0, zoomedWidth, selectedY),
                          scaled,
                          QRect(0, 0, zoomedWidth, selectedY));
    }
    painter.drawImage(QRect(0, selectedY, zoomedWidth, loupeHeight),
                      scaled,
                      QRect(0, selectedY, zoomedWidth, 1));
    if (selectedY + 1 < scaled.height()) {
        const int tailHeight = scaled.height() - selectedY - 1;
        painter.drawImage(QRect(0, selectedY + loupeHeight, zoomedWidth, tailHeight),
                          scaled,
                          QRect(0, selectedY + 1, zoomedWidth, tailHeight));
    }
    if (replayListenFrequencyHz > 0.0 && !recording->frames.empty()) {
        const SpectrumFrameRecord &frame = recording->frames[static_cast<std::size_t>(selectedFrame)];
        if (frame.maxFrequency > frame.minFrequency) {
            const double markerRatio =
                std::clamp((replayListenFrequencyHz - frame.minFrequency) /
                               (frame.maxFrequency - frame.minFrequency),
                           0.0,
                           1.0);
            const int markerX = static_cast<int>(std::round(markerRatio * (zoomedWidth - 1)));
            painter.setPen(QPen(QColor(255, 72, 72, 230), 2));
            painter.drawLine(markerX, 0, markerX, composed.height());
        }
    }
    if (replayAreaRulerEnabled && !recording->frames.empty()) {
        const QPoint first(std::clamp(replayAreaRulerStart.x(), 0, composed.width() - 1),
                           std::clamp(replayAreaRulerStart.y(), 0, composed.height() - 1));
        const QPoint last(std::clamp(replayAreaRulerEnd.x(), 0, composed.width() - 1),
                          std::clamp(replayAreaRulerEnd.y(), 0, composed.height() - 1));
        const QRect selection = QRect(first, last).normalized();
        if (selection.width() > 1 || selection.height() > 1) {
            auto imageYToFrame = [selectedY, loupeHeight, this](int imageY) {
                int sourceY = imageY;
                if (imageY > selectedY + loupeHeight - 1) {
                    sourceY -= loupeHeight - 1;
                } else if (imageY >= selectedY) {
                    sourceY = selectedY;
                }
                return std::clamp(sourceY, 0, static_cast<int>(recording->frames.size()) - 1);
            };
            const SpectrumFrameRecord &frequencyFrame = recording->frames.front();
            const double spanHz = frequencyFrame.maxFrequency - frequencyFrame.minFrequency;
            const double firstHz = frequencyFrame.minFrequency +
                                   static_cast<double>(selection.left()) * spanHz /
                                       static_cast<double>((std::max)(1, composed.width() - 1));
            const double lastHz = frequencyFrame.minFrequency +
                                  static_cast<double>(selection.right()) * spanHz /
                                      static_cast<double>((std::max)(1, composed.width() - 1));
            const int firstFrame = imageYToFrame(selection.top());
            const int lastFrame = imageYToFrame(selection.bottom());
            const qint64 elapsedMs = std::llabs(
                recording->frames[static_cast<std::size_t>(lastFrame)].elapsedMs -
                recording->frames[static_cast<std::size_t>(firstFrame)].elapsedMs);
            const QString label = QStringLiteral("%1: %2   %3: %4")
                                      .arg(replayText("frequency_span", "Delta f"),
                                           formatFrequency(std::abs(lastHz - firstHz)),
                                           replayText("time_span", "Delta t"),
                                           QString::number(static_cast<double>(elapsedMs) / 1000.0, 'f', 3) + QStringLiteral(" s"));
            painter.setPen(QPen(QColor(255, 230, 64), 1, Qt::DashLine));
            painter.setBrush(QColor(255, 230, 64, 24));
            painter.drawRect(selection);
            const QFontMetrics metrics(painter.font());
            const QSize labelSize = metrics.boundingRect(label).adjusted(-5, -3, 5, 3).size();
            QPoint textTopLeft(selection.left(), selection.top() - labelSize.height() - 2);
            if (textTopLeft.y() < 0) textTopLeft.setY(selection.bottom() + 2);
            if (textTopLeft.x() + labelSize.width() > composed.width()) {
                textTopLeft.setX((std::max)(0, composed.width() - labelSize.width()));
            }
            const QRect placed(textTopLeft, labelSize);
            painter.fillRect(placed, QColor(10, 10, 10, 220));
            painter.setPen(QColor(255, 244, 150));
            painter.drawText(placed, Qt::AlignCenter, label);
        }
    }
    painter.setPen(QPen(Qt::red, 2));
    painter.drawRect(0,
                     std::max(0, selectedY - 1),
                     std::max(0, composed.width() - 1),
                     std::min(std::max(3, loupeHeight + 2),
                              composed.height() - std::max(0, selectedY - 1)) - 1);
    painter.end();

    waterfallLabel->setPixmap(QPixmap::fromImage(composed));
    waterfallLabel->resize(composed.size());
    if (graphScroll && graphScroll->horizontalScrollBar()) {
        graphScroll->horizontalScrollBar()->setValue(
            std::min(graphScroll->horizontalScrollBar()->value(),
                     graphScroll->horizontalScrollBar()->maximum()));
    }
    if (scaleScroll && scaleScroll->horizontalScrollBar()) {
        scaleScroll->horizontalScrollBar()->setValue(
            std::min(scaleScroll->horizontalScrollBar()->value(),
                     scaleScroll->horizontalScrollBar()->maximum()));
    }
    if (waterfallScroll && waterfallScroll->horizontalScrollBar()) {
        waterfallScroll->horizontalScrollBar()->setValue(
            std::min(waterfallScroll->horizontalScrollBar()->value(),
                     waterfallScroll->horizontalScrollBar()->maximum()));
    }
    if (playbackTimer.isActive() && waterfallScroll && waterfallScroll->verticalScrollBar()) {
        const int target = std::clamp(selectedY - waterfallScroll->viewport()->height() / 2,
                                      0,
                                      waterfallScroll->verticalScrollBar()->maximum());
        waterfallScroll->verticalScrollBar()->setValue(target);
    }
}

void SpectrumFrameReplayDialog::renderExperimentWaterfallViewport() {
    if (renderingExperimentViewport || replayWaterfallPaused || !experimentWaterfallTile || waterfallImage.isNull()) return;
    QScopedValueRollback<bool> rendering(renderingExperimentViewport, true);
    const int viewportWidth = std::clamp(graphScroll->viewport()->width()-2, 1, 4096);
    const int virtualWidth = viewportWidth*std::clamp(waterfallZoomPercent, 100, 400)/100;
    const int loupe = std::clamp(waterfallRowHeight, 1, 16);
    const int virtualHeight = waterfallImage.height()+loupe-1;
    graph->setFixedWidth(virtualWidth); scaleWidget->setFixedWidth(virtualWidth);
    waterfallLabel->resize(virtualWidth, virtualHeight);
    if (playbackTimer.isActive()) waterfallScroll->verticalScrollBar()->setValue(
        std::clamp(selectedFrame-waterfallScroll->viewport()->height()/2, 0, waterfallScroll->verticalScrollBar()->maximum()));
    const int left = waterfallScroll->horizontalScrollBar()->value(), top = waterfallScroll->verticalScrollBar()->value();
    const int width = std::clamp(std::min(virtualWidth-left, waterfallScroll->viewport()->width()), 1, 4096);
    const int height = std::clamp(std::min(virtualHeight-top, waterfallScroll->viewport()->height()), 1, 2048);
    QImage tile(width, height, QImage::Format_RGB888);
    if (tile.isNull()) { experimentWaterfallTile->clear(); return; }
    tile.fill(Qt::black);
    QPainter painter(&tile);
    // Only the visible portion is rasterized; the QLabel supplies virtual scroll geometry.
    const auto segment = [&](int sourceY, int sourceRows, int targetY, int targetRows) {
        if (targetRows <= 0) return;
        const int low = std::max(top, targetY), high = std::min(top+height, targetY+targetRows);
        if (high <= low) return;
        const double scale = double(sourceRows)/targetRows;
        painter.drawImage(QRect(0, low-top, width, high-low), waterfallImage,
            QRectF(double(left)*waterfallImage.width()/virtualWidth, sourceY+(low-targetY)*scale,
                   double(width)*waterfallImage.width()/virtualWidth, (high-low)*scale));
    };
    segment(0, selectedFrame, 0, selectedFrame);
    segment(selectedFrame, 1, selectedFrame, loupe);
    segment(selectedFrame+1, waterfallImage.height()-selectedFrame-1, selectedFrame+loupe, waterfallImage.height()-selectedFrame-1);
    painter.setPen(QPen(Qt::red, 2));
    painter.drawRect(-left, selectedFrame-top, virtualWidth-1, loupe);
    if (replayAreaRulerEnabled) {
        painter.setPen(QPen(Qt::yellow, 1, Qt::DashLine));
        painter.drawRect(QRect(replayAreaRulerStart, replayAreaRulerEnd).normalized().translated(-left, -top));
    }
    painter.end();
    experimentViewportPixels = width*height;
    experimentWaterfallTile->setPixmap(QPixmap::fromImage(tile));
    experimentWaterfallTile->resize(width, height); experimentWaterfallTile->move(left, top); experimentWaterfallTile->show();
}

void SpectrumFrameReplayDialog::updateFrameSelection(int index) {
    if (recording->frames.empty()) {
        return;
    }
    selectedFrame = std::clamp(index, 0, static_cast<int>(recording->frames.size()) - 1);
    const SpectrumFrameRecord &frame = recording->frames[static_cast<std::size_t>(selectedFrame)];
    const int count = static_cast<int>(frame.magnitudes.size());
    frequencyScratch.resize(static_cast<std::size_t>(count));
    const double span = frame.maxFrequency - frame.minFrequency;
    for (int i = 0; i < count; ++i) {
        const double ratio = count > 1 ? static_cast<double>(i) / static_cast<double>(count - 1) : 0.0;
        frequencyScratch[static_cast<std::size_t>(i)] = static_cast<float>(frame.minFrequency + span * ratio);
    }
    graph->setLevelRange(levelMin, levelMax);
    graph->setData(frequencyScratch,
                   frame.magnitudes,
                   frame.minFrequency,
                   frame.maxFrequency,
                   frame.fftLength,
                   replayColoredSpectrum);
    updateScaleWidget();
    if (listenFrequencyControl) {
        listenFrequencyControl->setRangeHz(frame.minFrequency, frame.maxFrequency);
        if (replayListenFrequencyHz <= 0.0) {
            replayListenFrequencyHz = frame.centerFrequency;
        }
        {
            QSignalBlocker blocker(listenFrequencyControl);
            listenFrequencyControl->setValueHz(replayListenFrequencyHz);
        }
    }
    updateReplayMarker();
    renderWaterfallPixmap();
    if (!replayWaterfallPaused && replayWaterfallDisplayMode != 0) {
        rebuild3DWindow(false);
    }
    updateLabels();
}

void SpectrumFrameReplayDialog::updateLabels() {
    if (recording->frames.empty()) {
        infoLabel->setText(QStringLiteral("No frames"));
        return;
    }

    const SpectrumFrameRecord &frame = recording->frames[static_cast<std::size_t>(selectedFrame)];
    const QString fileName = QFileInfo(recordingPath).fileName();
    const QString utc = QDateTime::fromMSecsSinceEpoch(frame.utcMs, Qt::UTC)
                            .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz 'UTC'"));
    infoLabel->setText(QStringLiteral("%1 | frame %2/%3 | t=%4 ms | %5 - %6 | center %7 | %8")
                           .arg(fileName)
                           .arg(selectedFrame + 1)
                           .arg(recording->frames.size())
                           .arg(frame.elapsedMs)
                           .arg(formatFrequency(frame.minFrequency))
                           .arg(formatFrequency(frame.maxFrequency))
                           .arg(formatFrequency(frame.centerFrequency))
                           .arg(utc));
    if (isolatedExperiment && experimentDisplayAdjusted)
        infoLabel->setText(infoLabel->text() + " | " + replayText("experiment_display_limited", "Display adapted to replay limits"));
}

void SpectrumFrameReplayDialog::setPlaybackEnabled(bool enabled) {
    if (enabled && recording->frames.empty()) {
        enabled = false;
    }
    if (playButton && playButton->isChecked() != enabled) {
        playButton->blockSignals(true);
        playButton->setChecked(enabled);
        playButton->blockSignals(false);
    }
    if (playButton) {
        playButton->setText(enabled ? replayText("stop", "Stop") : replayText("play", "Play"));
    }
    if (enabled) {
        if (replayAudioCheckbox && replayAudioCheckbox->isChecked() && speedCombo) {
            const int oneXIndex = speedCombo->findData(1.0);
            if (oneXIndex >= 0 && speedCombo->currentIndex() != oneXIndex) {
                QSignalBlocker blocker(speedCombo);
                speedCombo->setCurrentIndex(oneXIndex);
            }
        }
        playbackStartFrame = selectedFrame;
        playbackStartElapsedMs = recording->frames[static_cast<std::size_t>(selectedFrame)].elapsedMs;
        playbackClock.restart();
        if (replayAudioCheckbox && replayAudioCheckbox->isChecked()) {
            startLocalIqPlayback();
        }
        scheduleNextPlaybackFrame();
    } else {
        playbackTimer.stop();
        stopLocalIqPlayback();
    }
}

double SpectrumFrameReplayDialog::replaySpeedMultiplier() const {
    if (!speedCombo) {
        return 1.0;
    }
    bool ok = false;
    const double speed = speedCombo->currentData().toDouble(&ok);
    if (!ok || !std::isfinite(speed) || speed <= 0.0) {
        return 1.0;
    }
    return speed;
}

void SpectrumFrameReplayDialog::scheduleNextPlaybackFrame() {
    if (recording->frames.empty()) {
        playbackTimer.stop();
        return;
    }
    playbackTimer.start(15);
}

void SpectrumFrameReplayDialog::updateLinkedIqControls() {
    const bool hasChannelIq = replayIqSourceAvailable(ReplayIqSource::ChannelIq);
    const bool hasFullIq = replayIqSourceAvailable(ReplayIqSource::FullIq);
    if (replayIqSourceCombo) {
        const QSignalBlocker blocker(replayIqSourceCombo);
        const int channelIndex = replayIqSourceCombo->findData(static_cast<int>(ReplayIqSource::ChannelIq));
        const int fullIndex = replayIqSourceCombo->findData(static_cast<int>(ReplayIqSource::FullIq));
        if (channelIndex >= 0) {
            replayIqSourceCombo->setItemData(channelIndex, hasChannelIq ? QVariant() : QVariant(0), Qt::UserRole - 1);
        }
        if (fullIndex >= 0) {
            replayIqSourceCombo->setItemData(fullIndex, hasFullIq ? QVariant() : QVariant(0), Qt::UserRole - 1);
        }
        if (!replayIqSourceAvailable(selectedReplayIqSource())) {
            if (hasChannelIq && channelIndex >= 0) {
                replayIqSourceCombo->setCurrentIndex(channelIndex);
            } else if (hasFullIq && fullIndex >= 0) {
                replayIqSourceCombo->setCurrentIndex(fullIndex);
            }
        }
    }
    const ReplayIqSource source = selectedReplayIqSource();
    const bool hasSelectedIq = replayIqSourceAvailable(source);
    const bool fullIq = source == ReplayIqSource::FullIq;
    if (replayAudioCheckbox) {
        replayAudioCheckbox->setEnabled(hasSelectedIq);
    }
    if (listenFrequencyControl && !recording->frames.empty()) {
        const SpectrumFrameRecord &frame = recording->frames[static_cast<std::size_t>(selectedFrame)];
        listenFrequencyControl->setEnabled(fullIq);
        listenFrequencyControl->setRangeHz(frame.minFrequency, frame.maxFrequency);
        if (!fullIq) {
            const double fixedFrequency =
                recording->metadata.value(QStringLiteral("listeningFrequency")).toDouble(frame.centerFrequency);
            replayListenFrequencyHz = fixedFrequency > 0.0 ? fixedFrequency : frame.centerFrequency;
        }
        QSignalBlocker blocker(listenFrequencyControl);
        listenFrequencyControl->setValueHz(replayListenFrequencyHz);
    }
    if (linkedIqLabel) {
        if (hasSelectedIq) {
            linkedIqLabel->setText(QStringLiteral("Linked IQ: %1")
                                       .arg(QFileInfo(replayIqPathForSource(source)).fileName()));
        } else if (hasChannelIq || hasFullIq) {
            linkedIqLabel->setText(replayText("select_iq", "Select an available IQ source"));
        } else {
            linkedIqLabel->setText(replayText("linked_iq_not_found", "Linked IQ not found"));
        }
    }
    updateReplayMarker();
}

void SpectrumFrameReplayDialog::updateReplayMarker() {
    if (graph) {
        graph->setTuningMarker(replayListenFrequencyHz, replayListenFrequencyHz > 0.0);
    }
    updateScaleWidget();
    renderWaterfallPixmap();
}

void SpectrumFrameReplayDialog::updateScaleWidget() {
    if (!scaleWidget || recording->frames.empty()) {
        return;
    }
    const SpectrumFrameRecord &frame = recording->frames[static_cast<std::size_t>(selectedFrame)];
    if (frame.maxFrequency <= frame.minFrequency) {
        return;
    }
    const double bandwidth = recording->metadata.value(QStringLiteral("bandwidth")).toDouble(0.0);
    const int modulationType = recording->metadata.value(QStringLiteral("modulationType")).toInt(MOD_WFM);
    const QSignalBlocker blocker(scaleWidget);
    scaleWidget->setRange(frame.minFrequency, frame.maxFrequency);
    scaleWidget->setTuning(replayListenFrequencyHz > 0.0 ? replayListenFrequencyHz : frame.centerFrequency,
                           frame.centerFrequency,
                           bandwidth > 0.0 ? bandwidth : std::max(1.0, (frame.maxFrequency - frame.minFrequency) / 100.0),
                           modulationType);
}

void SpectrumFrameReplayDialog::scheduleDeferredRender() {
    if (deferredRenderQueued) {
        return;
    }
    deferredRenderQueued = true;
    QTimer::singleShot(0, this, [this]() {
        deferredRenderQueued = false;
        if (recording->frames.empty()) {
            return;
        }
        updateScaleWidget();
        renderWaterfallPixmap();
        if (replayWaterfallDisplayMode != 0) {
            rebuild3DWindow(true);
        }
        QTimer::singleShot(40, this, [this]() {
            if (!recording->frames.empty()) {
                updateScaleWidget();
                renderWaterfallPixmap();
            }
        });
    });
}

void SpectrumFrameReplayDialog::setReplayListenFrequency(double frequencyHz, bool fromUser) {
    if (frequencyHz <= 0.0 || !std::isfinite(frequencyHz)) {
        return;
    }
    if (fromUser && selectedReplayIqSource() == ReplayIqSource::ChannelIq) {
        updateLinkedIqControls();
        return;
    }
    replayListenFrequencyHz = frequencyHz;
    if (listenFrequencyControl) {
        QSignalBlocker blocker(listenFrequencyControl);
        listenFrequencyControl->setValueHz(replayListenFrequencyHz);
    }
    updateReplayMarker();
    if (localIqPlaybackActive) {
        const bool restartPlayback =
            selectedReplayIqSource() == ReplayIqSource::FullIq &&
            playButton &&
            playButton->isChecked() &&
            replayAudioCheckbox &&
            replayAudioCheckbox->isChecked();
        stopLocalIqPlayback();
        if (restartPlayback) {
            startLocalIqPlayback();
        }
    }
}

void SpectrumFrameReplayDialog::startLocalIqPlayback() {
    const ReplayIqSource source = selectedReplayIqSource();
    const QString path = replayIqPathForSource(source);
    if (path.isEmpty() || !QFileInfo::exists(path)) {
        updateLinkedIqControls();
        return;
    }

    resetLocalIqPlaybackState(false);
    if (!isolatedExperiment) IqBuffer::clear();
    localIqPlaybackActive = true;
    localIqPlaybackSampleRate = 0.0;
    localIqPlaybackAudioSampleRate = 0.0;
    localIqPlaybackSeekSampleRate = 0.0;
    localIqPlaybackFeedSampleRate = 0.0;
    localIqPlaybackDataSize = 0;
    localIqPlaybackBytesRead = 0;
    localIqPlaybackStartByteOffset = 0;
    localIqPlaybackBytesPerSample = 0;
    localIqPlaybackS16 = false;
    localIqPlaybackDurationSeconds = 0.0;
    localIqPlaybackTimelineOffsetSeconds = 0.0;
    localIqPlaybackStartDelaySeconds = 0.0;

    const qint64 selectedElapsedMs = recording->frames.empty()
                                         ? 0
                                         : recording->frames[static_cast<std::size_t>(selectedFrame)].elapsedMs;
    const double selectedSeconds = std::max(0.0, selectedElapsedMs / 1000.0);
    quint64 dataOffset = 0;

    if (source == ReplayIqSource::ChannelIq) {
        PlaybackManager::WavInfo info;
        QString errorMessage;
        if (!PlaybackManager::readWavInfo(path, info, &errorMessage) ||
            info.mode != PlaybackManager::Mode::ChannelIqWav) {
            linkedIqLabel->setText(QStringLiteral("Channel IQ error: %1").arg(errorMessage));
            localIqPlaybackActive = false;
            updateLinkedIqControls();
            return;
        }
        replayListenFrequencyHz =
            recording->metadata.value(QStringLiteral("listeningFrequency")).toDouble(replayListenFrequencyHz);
        if (replayListenFrequencyHz <= 0.0 && !recording->frames.empty()) {
            replayListenFrequencyHz = recording->frames.front().centerFrequency;
        }
        localIqPlaybackSampleRate = replayIqNominalSampleRate(source, info.sampleRate);
        localIqPlaybackBytesPerSample = info.channels * (info.bitsPerSample / 8);
        localIqPlaybackDataSize = info.dataSize;
        localIqPlaybackS16 = true;
        dataOffset = info.dataOffset;
    } else {
        const double sampleRate =
            recording->metadata.value(QStringLiteral("sourceSampleRate")).toDouble(0.0);
        if (sampleRate <= 0.0 || !std::isfinite(sampleRate)) {
            linkedIqLabel->setText(replayText("full_iq_bad_rate", "Full IQ error: invalid sample rate"));
            localIqPlaybackActive = false;
            updateLinkedIqControls();
            return;
        }
        localIqPlaybackSampleRate = replayIqNominalSampleRate(source, sampleRate);
        localIqPlaybackBytesPerSample = 2;
        localIqPlaybackDataSize = static_cast<quint64>(QFileInfo(path).size());
        localIqPlaybackS16 = false;
        dataOffset = 0;
    }
    if (localIqPlaybackS16) {
        localIqPlaybackAudioSampleRate = localIqPlaybackSampleRate;
    } else {
        localIqPlaybackAudioSampleRate = localIqPlaybackSampleRate;
    }
    localIqPlaybackSeekSampleRate =
        replayIqEffectiveSampleRate(source,
                                    localIqPlaybackSampleRate,
                                    localIqPlaybackDataSize,
                                    localIqPlaybackBytesPerSample);
    if (localIqPlaybackSeekSampleRate <= 0.0 || !std::isfinite(localIqPlaybackSeekSampleRate)) {
        localIqPlaybackSeekSampleRate = localIqPlaybackSampleRate;
    }
    localIqPlaybackFeedSampleRate = localIqPlaybackSampleRate;

    const double iqDurationSeconds =
        localIqPlaybackDataSize /
        static_cast<double>(std::max(1, localIqPlaybackBytesPerSample)) /
        std::max(1.0, localIqPlaybackSeekSampleRate);
    const double iqTimelineOffsetSeconds = replayIqTimelineOffsetSeconds(source, iqDurationSeconds);
    localIqPlaybackDurationSeconds = iqDurationSeconds;
    localIqPlaybackTimelineOffsetSeconds = iqTimelineOffsetSeconds;
    const double iqFileSeconds = selectedSeconds - iqTimelineOffsetSeconds;
    localIqPlaybackStartDelaySeconds = iqFileSeconds < 0.0 ? -iqFileSeconds : 0.0;
    quint64 sampleOffset =
        static_cast<quint64>(std::floor(std::max(0.0, iqFileSeconds) * localIqPlaybackSeekSampleRate));
    quint64 byteOffset = sampleOffset * static_cast<quint64>(std::max(1, localIqPlaybackBytesPerSample));
    byteOffset -= byteOffset % static_cast<quint64>(std::max(1, localIqPlaybackBytesPerSample));
    byteOffset = std::min(byteOffset, localIqPlaybackDataSize);

    QFile validationFile(path);
    if (!validationFile.open(QIODevice::ReadOnly)) {
        linkedIqLabel->setText(QStringLiteral("Replay IQ error: %1").arg(validationFile.errorString()));
        localIqPlaybackActive = false;
        updateLinkedIqControls();
        return;
    }
    if (!validationFile.seek(static_cast<qint64>(dataOffset + byteOffset))) {
        linkedIqLabel->setText(QStringLiteral("Replay IQ error: cannot seek"));
        validationFile.close();
        localIqPlaybackActive = false;
        updateLinkedIqControls();
        return;
    }
    validationFile.close();

    localIqPlaybackBytesRead = byteOffset;
    localIqPlaybackStartByteOffset = byteOffset;
    if (isolatedExperiment) {
        startExperimentIqPlayback(path, dataOffset, byteOffset);
        return;
    }
    RadioSettings audioSettings = replaySettingsForSource(localIqPlaybackAudioSampleRate);
    localAudioProcessor->configure(audioSettings);

    const QString workerPath = path;
    const quint64 workerDataOffset = dataOffset;
    const quint64 workerStartByteOffset = byteOffset;
    const quint64 workerDataSize = localIqPlaybackDataSize;
    const int workerBytesPerSample = localIqPlaybackBytesPerSample;
    const bool workerS16 = localIqPlaybackS16;
    const double workerRawSampleRate = localIqPlaybackSampleRate;
    const double workerSeekSampleRate = localIqPlaybackSeekSampleRate;
    const double workerFeedSampleRate = localIqPlaybackFeedSampleRate;
    const double workerStartDelaySeconds = localIqPlaybackStartDelaySeconds;
    const double workerInitialAudioRate = localIqPlaybackAudioSampleRate;
    const RadioSettings workerReplaySettings = replaySettingsForSource(localIqPlaybackSampleRate);
    AudioProcessor *workerAudioProcessor = localAudioProcessor;

    localIqWorkerStop.store(false);
    localIqWorkerThread = std::thread([this,
                                       workerPath,
                                       workerDataOffset,
                                       workerStartByteOffset,
                                       workerDataSize,
                                       workerBytesPerSample,
                                       workerS16,
                                       workerRawSampleRate,
                                       workerSeekSampleRate,
                                       workerFeedSampleRate,
                                       workerStartDelaySeconds,
                                       workerInitialAudioRate,
                                       workerReplaySettings,
                                       workerAudioProcessor]() {
        QFile file(workerPath);
        if (!file.open(QIODevice::ReadOnly) ||
            !file.seek(static_cast<qint64>(workerDataOffset + workerStartByteOffset))) {
            qDebug() << "[SpectrumReplay] audio worker open failed" << workerPath << file.errorString();
            return;
        }

        constexpr int channelBlockSamples = kChannelIqReplaySamplesPerFrame;
        constexpr int fullBlockSamples = kFullIqReplaySamplesPerFrame;
        const int blockSamples = workerS16 ? channelBlockSamples : fullBlockSamples;
        const double audioStartPrebufferSeconds =
            workerS16 ? kReplayAudioPrebufferSeconds : kReplayRawFullIqPrebufferSeconds;
        quint64 bytesRead = workerStartByteOffset;
        double audioRate = workerInitialAudioRate;
        bool audioStarted = false;
        bool drainPending = false;
        auto drainStarted = std::chrono::steady_clock::now();
        const auto startTime = std::chrono::steady_clock::now();
        std::vector<float> rawScratch;
        std::vector<float> floatScratch;
        int fullIqLogCount = 0;

        while (!localIqWorkerStop.load(std::memory_order_acquire)) {
            const auto now = std::chrono::steady_clock::now();
            const double elapsedSeconds =
                std::chrono::duration<double>(now - startTime).count();
            const double effectiveElapsedSeconds = elapsedSeconds - workerStartDelaySeconds;
            if (effectiveElapsedSeconds + kReplayIqLeadSeconds <= 0.0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(kReplayIqPumpIntervalMs));
                continue;
            }

            const double targetSamples =
                (effectiveElapsedSeconds + kReplayIqLeadSeconds) * workerFeedSampleRate;
            quint64 targetBytes =
                workerStartByteOffset +
                static_cast<quint64>(std::floor(targetSamples)) *
                    static_cast<quint64>(workerBytesPerSample);
            targetBytes -= targetBytes % static_cast<quint64>(workerBytesPerSample);
            targetBytes = std::min(targetBytes, workerDataSize);

            int blocksFed = 0;
            while (!localIqWorkerStop.load(std::memory_order_acquire) &&
                   bytesRead < targetBytes &&
                   blocksFed < kReplayIqMaxBlocksPerPump) {
                const double queuedIqSamples = static_cast<double>(IqBuffer::queuedFloatCount()) / 2.0;
                const double queuedSeconds = queuedIqSamples / std::max(1.0, audioRate);
                if (queuedSeconds >= kReplayIqMaxQueuedSeconds) {
                    break;
                }

                const quint64 remaining = targetBytes - bytesRead;
                const int bytesToRead =
                    static_cast<int>(std::min<quint64>(
                        remaining,
                        static_cast<quint64>(blockSamples * workerBytesPerSample)));
                QByteArray frame = file.read(bytesToRead);
                if (!frame.isEmpty()) {
                    const int remainder = frame.size() % workerBytesPerSample;
                    if (remainder != 0) {
                        frame.chop(remainder);
                    }
                }
                if (frame.isEmpty()) {
                    break;
                }

                bytesRead += static_cast<quint64>(frame.size());
                const int sampleCount = frame.size() / workerBytesPerSample;
                if (workerS16) {
                    floatScratch.clear();
                    appendIqS16LeToFloat(frame, sampleCount, floatScratch);
                    if (!floatScratch.empty()) {
                        IqBuffer::setSampleRateEstimate(workerRawSampleRate);
                        IqBuffer::publish(floatScratch.data(), floatScratch.size(), true, false);
                        audioRate = workerRawSampleRate;
                    }
                } else {
                    rawScratch.clear();
                    appendIqS8ToFloat(frame, sampleCount, rawScratch);
                    if (!rawScratch.empty()) {
                        audioRate = workerRawSampleRate;
                        IqBuffer::setSampleRateEstimate(audioRate);
                        IqBuffer::publish(rawScratch.data(), rawScratch.size(), true, false);
                        if (fullIqLogCount < 8) {
                            ++fullIqLogCount;
                            double sumI = 0.0;
                            double sumQ = 0.0;
                            double sumSq = 0.0;
                            double peak = 0.0;
                            const int iqStatsCount = static_cast<int>(rawScratch.size() / 2U);
                            for (int n = 0; n < iqStatsCount; ++n) {
                                const double iValue = rawScratch[static_cast<std::size_t>(2 * n)];
                                const double qValue = rawScratch[static_cast<std::size_t>(2 * n + 1)];
                                sumI += iValue;
                                sumQ += qValue;
                                sumSq += iValue * iValue + qValue * qValue;
                                peak = std::max(peak, std::max(std::abs(iValue), std::abs(qValue)));
                            }
                            const double invCount = iqStatsCount > 0 ? 1.0 / static_cast<double>(iqStatsCount) : 0.0;
                            qDebug() << "[SpectrumReplay] full IQ raw feed"
                                     << "sampleRate" << workerRawSampleRate
                                     << "centerHz" << workerReplaySettings.centerFrequency
                                     << "listenHz" << workerReplaySettings.listeningFrequency
                                     << "bandwidthHz" << workerReplaySettings.bandwidth
                                     << "inputSamples" << sampleCount
                                     << "floatCount" << static_cast<int>(rawScratch.size())
                                     << "meanI" << sumI * invCount
                                     << "meanQ" << sumQ * invCount
                                     << "rms" << (iqStatsCount > 0 ? std::sqrt(sumSq / static_cast<double>(iqStatsCount)) : 0.0)
                                     << "peak" << peak;
                        }
                    }
                }

                if (!audioStarted) {
                    const double queuedIqSamples = static_cast<double>(IqBuffer::queuedFloatCount()) / 2.0;
                    const double queuedSeconds = queuedIqSamples / std::max(1.0, audioRate);
                    if (queuedSeconds >= audioStartPrebufferSeconds && workerAudioProcessor) {
                        audioStarted = true;
                        QMetaObject::invokeMethod(workerAudioProcessor,
                                                  "startDemodulation",
                                                  Qt::QueuedConnection);
                    }
                }
                ++blocksFed;
            }

            if (bytesRead >= workerDataSize) {
                if (IqBuffer::queuedFloatCount() == 0) {
                    if (!drainPending) {
                        drainPending = true;
                        drainStarted = std::chrono::steady_clock::now();
                        qDebug() << "[SpectrumReplay] audio iq eof"
                                 << "bytesRead" << static_cast<qulonglong>(bytesRead)
                                 << "dataSize" << static_cast<qulonglong>(workerDataSize)
                                 << "audioSampleRate" << audioRate
                                 << "seekSampleRate" << workerSeekSampleRate
                                 << "feedSampleRate" << workerFeedSampleRate;
                    }
                    const auto drainElapsedMs =
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - drainStarted)
                            .count();
                    if (drainElapsedMs >= kReplayAudioDrainAfterIqMs) {
                        if (workerAudioProcessor) {
                            QMetaObject::invokeMethod(workerAudioProcessor,
                                                      "stopDemodulation",
                                                      Qt::QueuedConnection);
                        }
                        return;
                    }
                }
            } else {
                drainPending = false;
            }

            std::this_thread::sleep_for(
                std::chrono::milliseconds(bytesRead < targetBytes ? 1 : kReplayIqPumpIntervalMs));
        }
    });

    qDebug() << "[SpectrumReplay] audio start"
             << "source" << (source == ReplayIqSource::ChannelIq ? "channel" : "full")
             << "selectedFrame" << selectedFrame
             << "selectedSeconds" << selectedSeconds
             << "iqDurationSeconds" << iqDurationSeconds
             << "timelineOffsetSeconds" << iqTimelineOffsetSeconds
             << "iqFileSeconds" << iqFileSeconds
             << "startDelaySeconds" << localIqPlaybackStartDelaySeconds
             << "dspSampleRate" << localIqPlaybackSampleRate
             << "audioSampleRate" << localIqPlaybackAudioSampleRate
             << "seekSampleRate" << localIqPlaybackSeekSampleRate
             << "feedSampleRate" << localIqPlaybackFeedSampleRate
             << "speed" << replaySpeedMultiplier();

    updateLinkedIqControls();
    updateReplayMarker();
}

void SpectrumFrameReplayDialog::startExperimentIqPlayback(const QString &path, quint64 dataOffset, quint64 byteOffset) {
    const auto settings = replaySettingsForSource(localIqPlaybackSampleRate);
    const bool channel = localIqPlaybackS16;
    if (settings.modulationType > MOD_WFM || (channel && (settings.sampleRate < 48000 || settings.sampleRate > 2000000))) {
        linkedIqLabel->setText(replayText("experiment_audio_limit", "Isolated replay audio: AM/NFM/SAM/USB/LSB/DSB/CW/WFM; channel IQ 48 kS/s..2 MS/s."));
        localIqPlaybackActive = false; return;
    }
    if (!experimentAudio) experimentAudio = std::make_unique<DspBranchAudioOutput>();
    experimentAudio->setActiveSinks({QStringLiteral("experiment-replay")});
    auto *output = experimentAudio.get();
    const auto generation = ++experimentAudioGeneration;
    const quint64 size = localIqPlaybackDataSize;
    const int bytesPerSample = localIqPlaybackBytesPerSample;
    const double feedRate = localIqPlaybackFeedSampleRate, delay = localIqPlaybackStartDelaySeconds;
    localIqWorkerStop.store(false);
    localIqWorkerThread = std::thread([this, output, generation, path, dataOffset, byteOffset, size, channel, bytesPerSample, settings, feedRate, delay] {
        const auto fail = [this, generation](const QString &message) {
            QMetaObject::invokeMethod(this, [this, generation, message] {
                if (experimentAudioGeneration == generation) { linkedIqLabel->setText(message); localIqPlaybackActive = false; }
            }, Qt::QueuedConnection);
        };
        try {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly) || !file.seek(qint64(dataOffset+byteOffset))) { fail(file.errorString()); return; }
            IqChannelizer channelizer;
            AudioProcessor demodulator;
            demodulator.setLocalPlaybackEnabled(false);
            std::vector<float> iq, filtered;
            std::vector<short> pcm;
            quint64 position = byteOffset;
            bool first = true;
            const auto started = std::chrono::steady_clock::now();
            while (!localIqWorkerStop.load() && position < size) {
                const double due = delay + double(position-byteOffset)/bytesPerSample/std::max(1.0, feedRate);
                const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();
                if (due > elapsed + 0.1) { std::this_thread::sleep_for(std::chrono::milliseconds(4)); continue; }
                const qint64 count = qint64(std::min<quint64>(size-position, quint64((channel ? 4096 : 65536)*bytesPerSample)));
                const QByteArray bytes = file.read(count);
                if (bytes.size() != count || bytes.size()%bytesPerSample) { fail("Experiment IQ read failed or file changed"); return; }
                position += quint64(bytes.size());
                iq.clear();
                if (channel) appendIqS16LeToFloat(bytes, bytes.size()/bytesPerSample, iq);
                else appendIqS8ToFloat(bytes, bytes.size()/bytesPerSample, iq);
                auto audio = settings;
                const std::vector<float> *input = &iq;
                if (!channel) {
                    const auto result = channelizer.processFloatIq(iq.data(), iq.size(), settings, filtered);
                    if (!result.valid) { fail("Unsupported full IQ replay channel"); return; }
                    audio.sampleRate = result.outputRate; input = &filtered;
                }
                if (input->empty()) continue;
                audio.inputMode = INPUT_RF;
                audio.centerFrequency = audio.actualFrequency = audio.listeningFrequency;
                if (!demodulator.processIndependentIq(*input, audio, first, pcm)) { fail("Unsupported isolated replay demodulation/rate"); return; }
                if (localIqWorkerStop.load()) return;
                DspBranchPcmFrame frame;
                frame.sinkId = QStringLiteral("experiment-replay"); frame.generation = generation; frame.reset = first;
                frame.pcm = QByteArray(reinterpret_cast<const char *>(pcm.data()), int(pcm.size()*sizeof(short)));
                output->enqueue(frame); first = false;
            }
        } catch (const std::exception &e) { fail(QString::fromUtf8(e.what())); }
    });
    updateLinkedIqControls(); updateReplayMarker();
}

void SpectrumFrameReplayDialog::stopLocalIqWorker() {
    ++experimentAudioGeneration;
    localIqWorkerStop.store(true, std::memory_order_release);
    if (localIqWorkerThread.joinable()) {
        localIqWorkerThread.join();
    }
    localIqWorkerStop.store(false, std::memory_order_release);
}

void SpectrumFrameReplayDialog::stopLocalIqPlayback() {
    resetLocalIqPlaybackState(true);
}

void SpectrumFrameReplayDialog::resetLocalIqPlaybackState(bool updateButton) {
    stopLocalIqWorker();
    if (localAudioProcessor) {
        localAudioProcessor->stopDemodulation();
    }
    if (experimentAudio) experimentAudio->setActiveSinks({});
    if (!isolatedExperiment) IqBuffer::clear();
    localIqPlaybackSampleRate = 0.0;
    localIqPlaybackAudioSampleRate = 0.0;
    localIqPlaybackSeekSampleRate = 0.0;
    localIqPlaybackFeedSampleRate = 0.0;
    localIqPlaybackDataSize = 0;
    localIqPlaybackBytesRead = 0;
    localIqPlaybackStartByteOffset = 0;
    localIqPlaybackBytesPerSample = 0;
    localIqPlaybackS16 = false;
    localIqPlaybackActive = false;
    localIqPlaybackDurationSeconds = 0.0;
    localIqPlaybackTimelineOffsetSeconds = 0.0;
    localIqPlaybackStartDelaySeconds = 0.0;
    Q_UNUSED(updateButton);
    updateLinkedIqControls();
}

RadioSettings SpectrumFrameReplayDialog::replaySettingsForSource(double sampleRate) const {
    RadioSettings settings;
    settings.inputMode = recording->metadata.value(QStringLiteral("inputMode")).toInt(settings.inputMode);
    settings.centerFrequency = recording->metadata.value(QStringLiteral("centerFrequency")).toDouble(settings.centerFrequency);
    settings.actualFrequency = recording->metadata.value(QStringLiteral("actualFrequency")).toDouble(settings.actualFrequency);
    settings.listeningFrequency = replayListenFrequencyHz > 0.0 ? replayListenFrequencyHz
                                                                 : recording->metadata.value(QStringLiteral("listeningFrequency")).toDouble(settings.listeningFrequency);
    settings.sampleRate = sampleRate > 0.0
                              ? sampleRate
                              : recording->metadata.value(QStringLiteral("sourceSampleRate")).toDouble(settings.sampleRate);
    settings.bandwidth = recording->metadata.value(QStringLiteral("bandwidth")).toDouble(settings.bandwidth);
    settings.modulationType = recording->metadata.value(QStringLiteral("modulationType")).toInt(settings.modulationType);
    settings.fftLength = recording->metadata.value(QStringLiteral("fftLength")).toInt(settings.fftLength);
    settings.fftWindowType = normalizedFftWindowType(
        recording->metadata.value(QStringLiteral("fftWindowType")).toInt(settings.fftWindowType));
    settings.audioEnabled = true;
    if (isolatedExperiment) {
        settings.bandwidth = experimentSettings.value("bandwidth").toDouble(settings.bandwidth);
        settings.modulationType = experimentSettings.value("modulationType").toInt(settings.modulationType);
        settings.audioLowPassHz = experimentSettings.value("audioLowPassHz").toDouble();
        settings.audioHighPassHz = experimentSettings.value("audioHighPassHz").toDouble();
        settings.audioFilterChainJson = experimentSettings.value("audioFilterChainJson").toString("[]");
    }

    if (selectedReplayIqSource() == ReplayIqSource::ChannelIq) {
        settings.centerFrequency = settings.listeningFrequency;
        settings.actualFrequency = settings.listeningFrequency;
    } else if (!recording->frames.empty()) {
        const int index = std::clamp(selectedFrame, 0, static_cast<int>(recording->frames.size()) - 1);
        const SpectrumFrameRecord &frame = recording->frames[static_cast<std::size_t>(index)];
        if (frame.centerFrequency > 0.0 && std::isfinite(frame.centerFrequency)) {
            settings.centerFrequency = frame.centerFrequency;
            settings.actualFrequency = frame.centerFrequency;
        }
        if (settings.listeningFrequency <= 0.0 || !std::isfinite(settings.listeningFrequency)) {
            settings.listeningFrequency = settings.centerFrequency;
        }
    }
    return settings;
}

SpectrumFrameReplayDialog::ReplayIqSource SpectrumFrameReplayDialog::selectedReplayIqSource() const {
    if (!replayIqSourceCombo) {
        return ReplayIqSource::ChannelIq;
    }
    bool ok = false;
    const int value = replayIqSourceCombo->currentData().toInt(&ok);
    return ok && value == static_cast<int>(ReplayIqSource::FullIq)
               ? ReplayIqSource::FullIq
               : ReplayIqSource::ChannelIq;
}

QString SpectrumFrameReplayDialog::replayIqPathForSource(ReplayIqSource source) const {
    const auto path = source == ReplayIqSource::FullIq ? linkedFullIqPath : linkedChannelIqPath;
    if (isolatedExperiment && !verifiedReplayAssets.contains(QFileInfo(path).canonicalFilePath())) return {};
    return path;
}

QString SpectrumFrameReplayDialog::replayIqSidecarPathForSource(ReplayIqSource source) const {
    QString path = replayIqPathForSource(source);
    if (path.isEmpty()) {
        return QString();
    }
    if (source == ReplayIqSource::FullIq) {
        if (path.endsWith(QStringLiteral("_full_iq_s8.iq8"))) {
            path.chop(QStringLiteral("_full_iq_s8.iq8").size());
            path += QStringLiteral("_full_iq_s8.json");
        } else {
            path += QStringLiteral(".json");
        }
    } else {
        if (path.endsWith(QStringLiteral("_channel_iq.wav"))) {
            path.chop(QStringLiteral("_channel_iq.wav").size());
            path += QStringLiteral("_channel_iq.json");
        } else {
            path += QStringLiteral(".json");
        }
    }
    if (isolatedExperiment && !verifiedReplayAssets.contains(QFileInfo(path).canonicalFilePath())) return {};
    return path;
}

double SpectrumFrameReplayDialog::replayIqTimelineOffsetSeconds(ReplayIqSource source,
                                                                double iqDurationSeconds) const {
    const QString sidecarPath = replayIqSidecarPathForSource(source);
    QFile sidecar(sidecarPath);
    if (!sidecar.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return 0.0;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(sidecar.readAll());
    if (!doc.isObject()) {
        return 0.0;
    }
    const QJsonObject root = doc.object();

    if (root.contains(QStringLiteral("spectrumOffsetMs"))) {
        const double offsetMs = root.value(QStringLiteral("spectrumOffsetMs")).toDouble(0.0);
        if (std::isfinite(offsetMs)) {
            return offsetMs / 1000.0;
        }
    }

    const double iqFirstUtcMs = root.value(QStringLiteral("firstFrameUtcMs")).toDouble(0.0);
    const double spectrumFirstUtcMs =
        root.value(QStringLiteral("spectrumFirstFrameUtcMs")).toDouble(0.0);
    if (iqFirstUtcMs > 0.0 && spectrumFirstUtcMs > 0.0) {
        return (iqFirstUtcMs - spectrumFirstUtcMs) / 1000.0;
    }

    if (recording->frames.empty() || iqDurationSeconds <= 0.0 || !std::isfinite(iqDurationSeconds)) {
        return 0.0;
    }
    const QString recordedAtLocal = root.value(QStringLiteral("recordedAtLocal")).toString();
    QDateTime stopTime = QDateTime::fromString(recordedAtLocal, Qt::ISODateWithMs);
    if (!stopTime.isValid()) {
        return 0.0;
    }
    if (stopTime.timeSpec() == Qt::LocalTime) {
        stopTime = stopTime.toLocalTime();
    }
    const qint64 estimatedIqStartUtcMs =
        stopTime.toMSecsSinceEpoch() -
        static_cast<qint64>(std::llround(iqDurationSeconds * 1000.0));
    return (estimatedIqStartUtcMs - recording->frames.front().utcMs) / 1000.0;
}

double SpectrumFrameReplayDialog::replayIqNominalSampleRate(ReplayIqSource source,
                                                            double fallbackSampleRate) const {
    const QString sidecarPath = replayIqSidecarPathForSource(source);
    QFile sidecar(sidecarPath);
    if (sidecar.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QJsonDocument doc = QJsonDocument::fromJson(sidecar.readAll());
        if (doc.isObject()) {
            const QJsonObject root = doc.object();
            const double nominalRate = root.value(QStringLiteral("sampleRate")).toDouble(0.0);
            if (nominalRate > 0.0 && std::isfinite(nominalRate)) {
                return nominalRate;
            }
            const double sourceRate = root.value(QStringLiteral("sourceSampleRate")).toDouble(0.0);
            if (source == ReplayIqSource::FullIq && sourceRate > 0.0 && std::isfinite(sourceRate)) {
                return sourceRate;
            }
        }
    }
    return fallbackSampleRate;
}

double SpectrumFrameReplayDialog::replayIqEffectiveSampleRate(ReplayIqSource source,
                                                              double fallbackSampleRate,
                                                              quint64 dataSize,
                                                              int bytesPerSample) const {
    if (fallbackSampleRate <= 0.0 || !std::isfinite(fallbackSampleRate) ||
        dataSize == 0 || bytesPerSample <= 0) {
        return fallbackSampleRate;
    }
    const double sampleCount =
        static_cast<double>(dataSize) / static_cast<double>(bytesPerSample);
    if (sampleCount <= 0.0 || !std::isfinite(sampleCount)) {
        return fallbackSampleRate;
    }

    const QString sidecarPath = replayIqSidecarPathForSource(source);
    QFile sidecar(sidecarPath);
    if (sidecar.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QJsonDocument doc = QJsonDocument::fromJson(sidecar.readAll());
        if (doc.isObject()) {
            const QJsonObject root = doc.object();
            const double effectiveRate =
                root.value(QStringLiteral("effectiveSampleRate")).toDouble(0.0);
            if (effectiveRate > 0.0 && std::isfinite(effectiveRate)) {
                return effectiveRate;
            }
            const double firstUtc = root.value(QStringLiteral("firstFrameUtcMs")).toDouble(0.0);
            const double lastUtc = root.value(QStringLiteral("lastFrameUtcMs")).toDouble(0.0);
            if (lastUtc > firstUtc && firstUtc > 0.0) {
                const double durationSeconds = (lastUtc - firstUtc) / 1000.0;
                if (durationSeconds > 0.0 && std::isfinite(durationSeconds)) {
                    const double estimatedRate = sampleCount / durationSeconds;
                    if (estimatedRate > 0.0 && std::isfinite(estimatedRate)) {
                        return estimatedRate;
                    }
                }
            }
        }
    }

    if (recording->frames.size() >= 2) {
        const qint64 firstUtc = recording->frames.front().utcMs;
        const qint64 lastUtc = recording->frames.back().utcMs;
        if (lastUtc > firstUtc) {
            const double durationSeconds = static_cast<double>(lastUtc - firstUtc) / 1000.0;
            if (durationSeconds > 0.0 && std::isfinite(durationSeconds)) {
                const double estimatedRate = sampleCount / durationSeconds;
                if (estimatedRate > 0.0 && std::isfinite(estimatedRate)) {
                    return estimatedRate;
                }
            }
        }
    }
    return fallbackSampleRate;
}

bool SpectrumFrameReplayDialog::replayIqSourceAvailable(ReplayIqSource source) const {
    const QString path = replayIqPathForSource(source);
    return !path.isEmpty() && QFileInfo::exists(path);
}

double SpectrumFrameReplayDialog::frequencyForLocalX(int x, int widgetWidth) const {
    if (recording->frames.empty() || widgetWidth <= 0) {
        return replayListenFrequencyHz;
    }
    const SpectrumFrameRecord &frame = recording->frames[static_cast<std::size_t>(selectedFrame)];
    if (frame.maxFrequency <= frame.minFrequency) {
        return frame.centerFrequency;
    }
    const double ratio = std::clamp(static_cast<double>(x) / static_cast<double>(std::max(1, widgetWidth - 1)),
                                    0.0,
                                    1.0);
    return frame.minFrequency + ratio * (frame.maxFrequency - frame.minFrequency);
}

QColor SpectrumFrameReplayDialog::colorForLevel(float value,
                                                float minLevel,
                                                float maxLevel,
                                                float contrastValue,
                                                float sensitivityValue) {
    const float range = std::max(1.0f, maxLevel - minLevel);
    const float normalized = std::clamp((value - minLevel) / range, 0.0f, 1.0f);
    const float contrastFactor = contrastValue / 10.0f;
    const float sensitivityFactor = sensitivityValue / 10.0f;
    const float scaled = std::clamp(normalized * sensitivityFactor, 0.0f, 1.0f);
    static const std::array<QColor, 17> palette = {{
        QColor("#000020"),
        QColor("#000050"),
        QColor("#000090"),
        QColor("#0000F0"),
        QColor("#0000FF"),
        QColor("#50F030"),
        QColor("#1E90FF"),
        QColor("#FFFFFF"),
        QColor("#FFFF00"),
        QColor("#FE6D16"),
        QColor("#FE6D16"),
        QColor("#FF0000"),
        QColor("#FF0000"),
        QColor("#C60000"),
        QColor("#9F0000"),
        QColor("#750000"),
        QColor("#4A0000"),
    }};
    int index = static_cast<int>(scaled * static_cast<float>(palette.size() - 1));
    index = std::clamp(index, 0, static_cast<int>(palette.size() - 1));
    const QColor baseColor = palette[static_cast<std::size_t>(index)];
    const int blue = (baseColor.green() + baseColor.blue()) / 3;
    const int r = std::clamp(
        static_cast<int>(baseColor.red() * contrastFactor + blue * (1.0f - contrastFactor)),
        0,
        255);
    const int g = std::clamp(
        static_cast<int>(baseColor.green() * contrastFactor + blue * (1.0f - contrastFactor)),
        0,
        255);
    const int b = std::clamp(
        static_cast<int>(baseColor.blue() * contrastFactor + blue * (1.0f - contrastFactor)),
        0,
        255);
    return QColor(r, g, b);
}

QString SpectrumFrameReplayDialog::formatFrequency(double hz) {
    const double absHz = std::abs(hz);
    if (absHz >= 1.0e9) {
        return QStringLiteral("%1 GHz").arg(hz / 1.0e9, 0, 'f', 6);
    }
    if (absHz >= 1.0e6) {
        return QStringLiteral("%1 MHz").arg(hz / 1.0e6, 0, 'f', 6);
    }
    if (absHz >= 1.0e3) {
        return QStringLiteral("%1 kHz").arg(hz / 1.0e3, 0, 'f', 3);
    }
    return QStringLiteral("%1 Hz").arg(hz, 0, 'f', 0);
}


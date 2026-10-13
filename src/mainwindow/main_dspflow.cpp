#include "main.h"
#include "multivfowidget.h"

#include "audiofilterchainwidget.h"
#include "appsettingsutils.h"
#include "dspflowpanel.h"
#include "dspcomputedialog.h"
#include "dspbranchsettingswidget.h"
#include "dspgraphmodel.h"
#include "frequencycontrol.h"
#include "tuningutils.h"
#include "researchanalysisdialog.h"
#include "videowidget.h"

#include <QAbstractButton>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QDockWidget>
#include <QFormLayout>
#include <QFileDialog>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QPointer>
#include <QSettings>
#include <QSet>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QSerialPort>
#include <QTimer>
#include <QTextCursor>
#include <QVBoxLayout>

#include <functional>
#include <memory>

namespace {

bool focusInside(const QWidget *widget) {
    QWidget *focus = QApplication::focusWidget();
    return widget && focus && (focus == widget || widget->isAncestorOf(focus));
}

bool comboContentsMatch(const QComboBox *source, const QComboBox *copy) {
    if (!source || !copy || source->count() != copy->count()) {
        return false;
    }
    for (int i = 0; i < source->count(); ++i) {
        if (source->itemText(i) != copy->itemText(i) || source->itemData(i) != copy->itemData(i)) {
            return false;
        }
    }
    return true;
}

void copyComboContents(QComboBox *source, QComboBox *copy) {
    if (!source || !copy) {
        return;
    }
    const QSignalBlocker blocker(copy);
    copy->clear();
    for (int i = 0; i < source->count(); ++i) {
        copy->addItem(source->itemIcon(i), source->itemText(i), source->itemData(i));
    }
    copy->setCurrentIndex(source->currentIndex());
}

QComboBox *linkedCombo(QComboBox *source, QWidget *parent, QTimer *syncTimer) {
    auto *copy = new QComboBox(parent);
    copy->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    if (!source) {
        copy->setEnabled(false);
        return copy;
    }

    QPointer<QComboBox> sourceGuard(source);
    copyComboContents(sourceGuard.data(), copy);
    QObject::connect(copy, QOverload<int>::of(&QComboBox::currentIndexChanged), copy,
                     [sourceGuard](int index) {
                         if (sourceGuard && sourceGuard->currentIndex() != index)
                             sourceGuard->setCurrentIndex(index);
                     });
    QObject::connect(source, QOverload<int>::of(&QComboBox::currentIndexChanged), copy,
                     [copy](int index) {
                         if (copy->currentIndex() != index) {
                             const QSignalBlocker blocker(copy);
                             copy->setCurrentIndex(index);
                         }
                     });
    QObject::connect(syncTimer, &QTimer::timeout, copy, [sourceGuard, copy]() {
        if (!sourceGuard) {
            copy->setEnabled(false);
            return;
        }
        if (focusInside(copy)) return;
        if (!comboContentsMatch(sourceGuard.data(), copy)) {
            copyComboContents(sourceGuard.data(), copy);
        } else if (copy->currentIndex() != sourceGuard->currentIndex()) {
            const QSignalBlocker blocker(copy);
            copy->setCurrentIndex(sourceGuard->currentIndex());
        }
        copy->setEnabled(sourceGuard->isEnabled());
    });
    return copy;
}
QWidget *linkedSlider(QSlider *source, QWidget *parent, QTimer *syncTimer,
                      const QString &suffix = QString(), double divisor = 1.0) {
    auto *container = new QWidget(parent);
    auto *layout = new QHBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(5);
    auto *copy = new QSlider(source ? source->orientation() : Qt::Horizontal, container);
    auto *valueLabel = new QLabel(container);
    valueLabel->setMinimumWidth(64);
    valueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    const auto updateLabel = [valueLabel, suffix, divisor](int value) {
        const double safeDivisor = divisor == 0.0 ? 1.0 : divisor;
        valueLabel->setText(QStringLiteral("%1%2")
                                .arg(value / safeDivisor, 0, 'f', safeDivisor == 1.0 ? 0 : 1)
                                .arg(suffix));
    };
    layout->addWidget(copy, 1);
    layout->addWidget(valueLabel);
    if (!source) {
        copy->setEnabled(false);
        valueLabel->setText(QStringLiteral("--"));
        return container;
    }

    QPointer<QSlider> sourceGuard(source);
    copy->setRange(source->minimum(), source->maximum());
    copy->setSingleStep(source->singleStep());
    copy->setPageStep(source->pageStep());
    copy->setInvertedAppearance(source->invertedAppearance());
    copy->setValue(source->value());
    updateLabel(copy->value());
    QObject::connect(copy, &QSlider::valueChanged, copy, [sourceGuard, updateLabel](int value) {
        updateLabel(value);
        if (sourceGuard && sourceGuard->value() != value) sourceGuard->setValue(value);
    });
    QObject::connect(source, &QSlider::valueChanged, copy, [copy, updateLabel](int value) {
        updateLabel(value);
        if (copy->value() != value) {
            const QSignalBlocker blocker(copy);
            copy->setValue(value);
        }
    });
    QObject::connect(syncTimer, &QTimer::timeout, copy, [sourceGuard, copy, updateLabel]() {
        if (!sourceGuard) {
            copy->setEnabled(false);
            return;
        }
        if (focusInside(copy)) return;
        if (copy->minimum() != sourceGuard->minimum() || copy->maximum() != sourceGuard->maximum()) {
            const QSignalBlocker blocker(copy);
            copy->setRange(sourceGuard->minimum(), sourceGuard->maximum());
        }
        if (copy->value() != sourceGuard->value()) {
            const QSignalBlocker blocker(copy);
            copy->setValue(sourceGuard->value());
            updateLabel(sourceGuard->value());
        }
        copy->setEnabled(sourceGuard->isEnabled());
    });
    return container;
}
QWidget *valueSlider(QSlider **sliderOut, QWidget *parent, int minimum, int maximum,
                     int value, const QString &suffix = QString(), double divisor = 1.0) {
    auto *container = new QWidget(parent);
    auto *layout = new QHBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(5);
    auto *slider = new QSlider(Qt::Horizontal, container);
    auto *valueLabel = new QLabel(container);
    valueLabel->setMinimumWidth(72);
    valueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    slider->setRange(minimum, maximum);
    slider->setValue(std::clamp(value, minimum, maximum));
    const auto updateLabel = [valueLabel, suffix, divisor](int current) {
        const double safeDivisor = divisor == 0.0 ? 1.0 : divisor;
        valueLabel->setText(QStringLiteral("%1%2")
                                .arg(current / safeDivisor, 0, 'f',
                                     safeDivisor == 1.0 ? 0 : 1)
                                .arg(suffix));
    };
    updateLabel(slider->value());
    QObject::connect(slider, &QSlider::valueChanged, slider, updateLabel);
    layout->addWidget(slider, 1);
    layout->addWidget(valueLabel);
    if (sliderOut) *sliderOut = slider;
    return container;
}
QCheckBox *linkedCheckBox(QCheckBox *source, const QString &text, QWidget *parent, QTimer *syncTimer) {
    auto *copy = new QCheckBox(text, parent);
    if (!source) {
        copy->setEnabled(false);
        return copy;
    }

    QPointer<QCheckBox> sourceGuard(source);
    copy->setChecked(sourceGuard->isChecked());
    QObject::connect(copy, &QCheckBox::toggled, copy, [sourceGuard](bool checked) {
        if (sourceGuard && sourceGuard->isChecked() != checked) sourceGuard->setChecked(checked);
    });
    QObject::connect(source, &QCheckBox::toggled, copy, [copy](bool checked) {
        if (copy->isChecked() != checked) {
            const QSignalBlocker blocker(copy);
            copy->setChecked(checked);
        }
    });
    QObject::connect(syncTimer, &QTimer::timeout, copy, [sourceGuard, copy]() {
        if (!sourceGuard) {
            copy->setEnabled(false);
            return;
        }
        if (focusInside(copy)) return;
        if (copy->isChecked() != sourceGuard->isChecked()) {
            const QSignalBlocker blocker(copy);
            copy->setChecked(sourceGuard->isChecked());
        }
        copy->setEnabled(sourceGuard->isEnabled());
    });
    return copy;
}
QDoubleSpinBox *linkedDoubleSpin(QDoubleSpinBox *source, QWidget *parent, QTimer *syncTimer) {
    auto *copy = new QDoubleSpinBox(parent);
    if (!source) { copy->setEnabled(false); return copy; }
    QPointer<QDoubleSpinBox> sourceGuard(source);
    copy->setRange(sourceGuard->minimum(), sourceGuard->maximum());
    copy->setDecimals(sourceGuard->decimals());
    copy->setSingleStep(sourceGuard->singleStep());
    copy->setSuffix(sourceGuard->suffix());
    copy->setKeyboardTracking(false);
    copy->setValue(sourceGuard->value());
    QObject::connect(copy, QOverload<double>::of(&QDoubleSpinBox::valueChanged), copy,
        [sourceGuard](double value) {
            if (sourceGuard && !qFuzzyCompare(sourceGuard->value() + 1.0, value + 1.0))
                sourceGuard->setValue(value);
        });
    QObject::connect(source, QOverload<double>::of(&QDoubleSpinBox::valueChanged), copy,
        [copy](double value) {
            if (!qFuzzyCompare(copy->value() + 1.0, value + 1.0)) {
                const QSignalBlocker blocker(copy); copy->setValue(value);
            }
        });
    QObject::connect(syncTimer, &QTimer::timeout, copy, [sourceGuard, copy]() {
        if (!sourceGuard) { copy->setEnabled(false); return; }
        if (focusInside(copy)) return;
        if (copy->minimum() != sourceGuard->minimum() || copy->maximum() != sourceGuard->maximum()) {
            const QSignalBlocker blocker(copy);
            copy->setRange(sourceGuard->minimum(), sourceGuard->maximum());
        }
        if (!qFuzzyCompare(copy->value() + 1.0, sourceGuard->value() + 1.0)) {
            const QSignalBlocker blocker(copy); copy->setValue(sourceGuard->value());
        }
        copy->setEnabled(sourceGuard->isEnabled());
    });
    return copy;
}
QSpinBox *linkedSpin(QSpinBox *source, QWidget *parent, QTimer *syncTimer) {
    auto *copy = new QSpinBox(parent);
    if (!source) { copy->setEnabled(false); return copy; }
    QPointer<QSpinBox> sourceGuard(source);
    copy->setRange(sourceGuard->minimum(), sourceGuard->maximum());
    copy->setSingleStep(sourceGuard->singleStep());
    copy->setSuffix(sourceGuard->suffix());
    copy->setValue(sourceGuard->value());
    QObject::connect(copy, QOverload<int>::of(&QSpinBox::valueChanged), copy,
        [sourceGuard](int value) {
            if (sourceGuard && sourceGuard->value() != value) sourceGuard->setValue(value);
        });
    QObject::connect(source, QOverload<int>::of(&QSpinBox::valueChanged), copy,
        [copy](int value) {
            if (copy->value() != value) {
                const QSignalBlocker blocker(copy); copy->setValue(value);
            }
        });
    QObject::connect(syncTimer, &QTimer::timeout, copy, [sourceGuard, copy]() {
        if (!sourceGuard) { copy->setEnabled(false); return; }
        if (focusInside(copy)) return;
        if (copy->minimum() != sourceGuard->minimum() || copy->maximum() != sourceGuard->maximum()) {
            const QSignalBlocker blocker(copy);
            copy->setRange(sourceGuard->minimum(), sourceGuard->maximum());
        }
        if (copy->value() != sourceGuard->value()) {
            const QSignalBlocker blocker(copy); copy->setValue(sourceGuard->value());
        }
        copy->setEnabled(sourceGuard->isEnabled());
    });
    return copy;
}
QLineEdit *linkedLineEdit(QLineEdit *source, QWidget *parent, QTimer *syncTimer) {
    auto *copy = new QLineEdit(parent);
    if (!source) { copy->setEnabled(false); return copy; }
    QPointer<QLineEdit> sourceGuard(source);
    copy->setText(sourceGuard->text());
    copy->setPlaceholderText(sourceGuard->placeholderText());
    QObject::connect(copy, &QLineEdit::editingFinished, copy, [sourceGuard, copy]() {
        if (sourceGuard && sourceGuard->text() != copy->text()) {
            sourceGuard->setText(copy->text()); emit sourceGuard->editingFinished();
        }
    });
    QObject::connect(source, &QLineEdit::textChanged, copy, [copy](const QString &value) {
        if (!focusInside(copy) && copy->text() != value) {
            const QSignalBlocker blocker(copy); copy->setText(value);
        }
    });
    QObject::connect(syncTimer, &QTimer::timeout, copy, [sourceGuard, copy]() {
        if (!sourceGuard) { copy->setEnabled(false); return; }
        if (focusInside(copy)) return;
        if (copy->text() != sourceGuard->text()) {
            const QSignalBlocker blocker(copy); copy->setText(sourceGuard->text());
        }
        copy->setEnabled(sourceGuard->isEnabled());
    });
    return copy;
}
QLabel *linkedLabel(QLabel *source, QWidget *parent, QTimer *syncTimer) {
    auto *copy = new QLabel(parent);
    copy->setWordWrap(true);
    copy->setTextInteractionFlags(Qt::TextSelectableByMouse);
    if (!source) {
        copy->setText(QStringLiteral("--"));
        copy->setEnabled(false);
        return copy;
    }
    QPointer<QLabel> sourceGuard(source);
    copy->setText(source->text());
    copy->setToolTip(source->toolTip());
    QObject::connect(syncTimer, &QTimer::timeout, copy, [sourceGuard, copy]() {
        if (!sourceGuard) {
            copy->setEnabled(false);
            return;
        }
        if (copy->text() != sourceGuard->text()) copy->setText(sourceGuard->text());
        if (copy->toolTip() != sourceGuard->toolTip()) copy->setToolTip(sourceGuard->toolTip());
        copy->setEnabled(sourceGuard->isEnabled());
    });
    return copy;
}
AudioFilterKind filterKindForBlock(const QString &type, bool *ok = nullptr) {
    static const QHash<QString, AudioFilterKind> kinds = {
        {QStringLiteral("filter_low_pass"), AudioFilterKind::LowPass},
        {QStringLiteral("filter_high_pass"), AudioFilterKind::HighPass},
        {QStringLiteral("filter_band_pass"), AudioFilterKind::BandPass},
        {QStringLiteral("filter_notch"), AudioFilterKind::Notch},
        {QStringLiteral("filter_dc_blocker"), AudioFilterKind::DcBlocker},
        {QStringLiteral("filter_de_emphasis"), AudioFilterKind::DeEmphasis},
        {QStringLiteral("filter_parametric_eq"), AudioFilterKind::ParametricEq},
        {QStringLiteral("filter_low_shelf"), AudioFilterKind::LowShelf},
        {QStringLiteral("filter_high_shelf"), AudioFilterKind::HighShelf},
        {QStringLiteral("filter_adaptive_notch"), AudioFilterKind::AdaptiveNotch},
        {QStringLiteral("filter_noise_blanker"), AudioFilterKind::NoiseBlanker},
        {QStringLiteral("filter_cw"), AudioFilterKind::CwFilter},
        {QStringLiteral("filter_ctcss"), AudioFilterKind::CtcssSuppressor},
        {QStringLiteral("filter_spectral_denoise"), AudioFilterKind::SpectralDenoise},
        {QStringLiteral("filter_custom_fir"), AudioFilterKind::CustomFir},
        {QStringLiteral("filter_gain"), AudioFilterKind::Gain},
        {QStringLiteral("filter_compressor"), AudioFilterKind::Compressor},
        {QStringLiteral("filter_limiter"), AudioFilterKind::Limiter},
        {QStringLiteral("filter_noise_gate"), AudioFilterKind::NoiseGate}
    };
    const auto it = kinds.constFind(type);
    if (ok) *ok = it != kinds.constEnd();
    return it == kinds.constEnd() ? AudioFilterKind::LowPass : it.value();
}

} // namespace

void YourClassName::ensureDspFlowPanel() {
    if (dspFlowPanel) return;
    dspFlowPanel = new DspFlowPanel(nullptr);
    dspFlowPanel->setAttribute(Qt::WA_DeleteOnClose, false);
    dspFlowPanel->setLanguage(normalizedUiLanguage(uiLanguage) == QStringLiteral("uk"));
    dspFlowPanel->setAnalogPeakMeterEnabled(showSpectrumPeakMeter);
    dspFlowPanel->setAnalogPeakMeterStyle(spectrumPeakMeterStyle);
    QSettings settings(persistentSettingsFilePath(), QSettings::IniFormat);
    dspFlowPanel->setConfigurationJson(
        settings.value(QStringLiteral("dspFlow/configuration")).toString());
    dspFlowPanel->setMainAudioFilterChainWidget(audioFilterChainWidget);
    connect(dspFlowPanel, &DspFlowPanel::configurationChanged, this,
            [this](const QString &json) {
                QSettings settings(persistentSettingsFilePath(), QSettings::IniFormat);
                settings.setValue(QStringLiteral("dspFlow/configuration"), json);
            });
    connect(dspFlowPanel, &DspFlowPanel::blockActivated,
            this, &YourClassName::openDspBlockReference);
    connect(dspFlowPanel, &DspFlowPanel::blockCreated,
            this, &YourClassName::registerDspFilterBlock);
    connect(dspFlowPanel, &DspFlowPanel::blockRemoved, this,
            [this](const QString &type, const QString &id) {
                if (type.startsWith(QStringLiteral("filter_")) && audioFilterChainWidget)
                    audioFilterChainWidget->removeStageById(id);
            });
    connect(dspFlowPanel, &DspFlowPanel::controlTriggered,
            this, &YourClassName::triggerDspControlBlock);
    connect(dspFlowPanel, &DspFlowPanel::controlStateRefreshRequested,
            this, &YourClassName::refreshDspControlStates);
    connect(dspFlowPanel, &DspFlowPanel::fineTuneDeltaRequested,
            this, [this](double deltaHz) { applyListeningFrequencyDelta(deltaHz, 60); });
    connect(dspFlowPanel, &DspFlowPanel::workspaceScaleChanged,
            this, &YourClassName::onWaterfallScaleChanged);
    connect(dspFlowPanel, &DspFlowPanel::workspacePanRequested,
            this, &YourClassName::panSpectrumView);
    connect(dspFlowPanel, &DspFlowPanel::workspaceTuneContextRequested,
            this, &YourClassName::showTuneContextMenu);
    connect(dspFlowPanel, &DspFlowPanel::workspaceAutoTuneRequested,
            this, &YourClassName::tuneSignalCenterAt);
    connect(dspFlowPanel, &DspFlowPanel::workspaceScienceMarkerRequested,
            this, &YourClassName::setSpectrumScienceMarker);
    connect(dspFlowPanel, &DspFlowPanel::workspaceMultiVfoSelectionRequested,
            this, [this](double lowHz, double highHz) {
                if (!multiVfoWidget) return;
                const int channelIndex = multiVfoWidget->addSelection(lowHz, highHz);
                if (dspFlowPanel && channelIndex >= 0) dspFlowPanel->addMultiVfoBranch(channelIndex);
            });
    connect(dspFlowPanel, &DspFlowPanel::workspaceListeningFrequencyRequested,
            this, [this](double frequency) {
                updateTuningFromScale(frequency, pendingSettings.centerFrequency);
            });
    connect(dspFlowPanel, &DspFlowPanel::workspaceCenterFrequencyRequested,
            this, [this](double frequency) {
                updateTuningFromScale(pendingSettings.listeningFrequency, frequency);
            });
    connect(dspFlowPanel, &DspFlowPanel::workspaceTuningRequested,
            this, &YourClassName::updateTuningFromScale);
    connect(dspFlowPanel, &DspFlowPanel::workspaceAnalogPeakMeterToggled,
            this, [this](bool enabled) {
                showSpectrumPeakMeter = enabled;
                if (graphWidget) graphWidget->setAnalogPeakMeterEnabled(enabled);
                if (dspFlowPanel) dspFlowPanel->setAnalogPeakMeterEnabled(enabled);
                savePersistentSettings();
            });
    connect(dspFlowPanel, &DspFlowPanel::workspaceAnalogPeakMeterStyleChanged,
            this, [this](int style) {
                spectrumPeakMeterStyle = std::clamp(style, 0, 1);
                if (graphWidget) graphWidget->setAnalogPeakMeterStyle(spectrumPeakMeterStyle);
                if (dspFlowPanel) dspFlowPanel->setAnalogPeakMeterStyle(spectrumPeakMeterStyle);
                savePersistentSettings();
            });
    connect(dspFlowPanel, &DspFlowPanel::workspaceModeExitRequested, this, [this]() {
        alternativeInterfaceMode = false;
        if (dspFlowPanel) {
            dspFlowPanel->setWorkspaceMode(false);
            dspFlowPanel->hide();
        }
        showNormal();
        show();
        raise();
        activateWindow();
        savePersistentSettings();
    });
    connect(dspFlowPanel, &DspFlowPanel::workspaceCloseRequested, this, [this]() {
        if (dspFlowPanel) dspFlowPanel->hide();
        close();
    });
    refreshDspControlStates();
}

void YourClassName::registerDspFilterBlock(const QString &type, const QString &id) {
    bool ok = false;
    const AudioFilterKind kind = filterKindForBlock(type, &ok);
    if (ok && audioFilterChainWidget) {
        audioFilterChainWidget->ensureStage(id, kind, false);
    }
}

void YourClassName::refreshDspControlStates() {
    if (!dspFlowPanel) return;
    dspFlowPanel->setBranchSourceContext(pendingSettings, runState == RadioRunState::Running,
                                        updateTimer ? updateTimer->interval() : 100, spectrumFftOverlapPercent);
    const auto checked = [](const QCheckBox *box) { return box && box->isChecked(); };
    QHash<QString, bool> states;
    states.insert(QStringLiteral("multi_vfo_channelizer"), multiVfoWidget && QJsonDocument::fromJson(
        multiVfoWidget->configurationJson().toUtf8()).object().value(QStringLiteral("enabled")).toBool(false));
    states.insert(QStringLiteral("spectrum_display"), checked(spectrumCheckbox));
    states.insert(QStringLiteral("second_spectrum"), checked(graphCheckbox));
    states.insert(QStringLiteral("waterfall_3d"), waterfallDisplayModeCombo &&
                  waterfallDisplayModeCombo->currentData().toInt() !=
                      static_cast<int>(MyWaterfallWidget::DisplayMode::Waterfall2D));
    states.insert(QStringLiteral("audio_output"), checked(audioCheckbox));
    states.insert(QStringLiteral("decoder"), checked(digitalDecodeCheckbox));
    states.insert(QStringLiteral("digital_audio_settings"), checked(digitalDecodeCheckbox));
    states.insert(QStringLiteral("dmr_decoder"), checked(digitalDecodeCheckbox));
    states.insert(QStringLiteral("cw_decoder"), checked(digitalDecodeCheckbox));
    states.insert(QStringLiteral("sstv_decoder"), checked(videoDecodeCheckbox));
    states.insert(QStringLiteral("digital_video"), checked(videoDecodeCheckbox));
    states.insert(QStringLiteral("digital_video_settings"), checked(videoDecodeCheckbox));
    states.insert(QStringLiteral("agile_scan"), checked(agileScanCheckbox));
    states.insert(QStringLiteral("standard_scan"), checked(standardScanCheckbox));
    states.insert(QStringLiteral("listening_scan"), checked(listeningScanCheckbox));
    states.insert(QStringLiteral("spectrum_measurement"), checked(scanMeasurementCheckbox));
    states.insert(QStringLiteral("spur_suppression"), checked(spurSuppressionCheckbox));
    states.insert(QStringLiteral("hf_interference"), checked(hfInterferenceBaselineCheckbox));
    states.insert(QStringLiteral("gnss_sdr"), checked(gnssMonitorCheckbox));
    states.insert(QStringLiteral("gnss_serial"), gnssSerialPort && gnssSerialPort->isOpen());
    dspFlowPanel->setControlStates(runState != RadioRunState::Idle, states);
    dspFlowPanel->setFineTuneRangeHz(fineTuneRangeHz());
    QJsonObject visualization;
    visualization.insert(QStringLiteral("displayMode"), waterfallDisplayModeCombo
        ? waterfallDisplayModeCombo->currentData().toInt()
        : static_cast<int>(MyWaterfallWidget::DisplayMode::Waterfall2D));
    visualization.insert(QStringLiteral("resolutionDivisor"), waterfall3DResolutionDivisor);
visualization.insert(QStringLiteral("historyRows"), waterfall3DHistoryRows);
    visualization.insert(QStringLiteral("rowsPerFrame"), waterfallRowsPerFrame);
    visualization.insert(QStringLiteral("gpuPrepared"), experimentalGpuWaterfall);
    visualization.insert(QStringLiteral("minimumDbfs"), displayLevelMin);
    visualization.insert(QStringLiteral("maximumDbfs"), displayLevelMax);
    visualization.insert(QStringLiteral("contrast"), contrast);
    visualization.insert(QStringLiteral("sensitivity"), sensitivity);
    visualization.insert(QStringLiteral("colorSpectrum"), checked(colorCheckbox));
    visualization.insert(QStringLiteral("spectrumGradientFill"),
                         checked(alternativeSpectrumGradientCheckbox));
    visualization.insert(QStringLiteral("spectrumGradientOpacity"),
                         alternativeSpectrumGradientOpacity);
    visualization.insert(QStringLiteral("secondSpectrum"), checked(graphCheckbox));
    visualization.insert(QStringLiteral("showSpectrumFps"), showSpectrumFps);
    visualization.insert(QStringLiteral("showWaterfallFps"), showWaterfallFps);
    visualization.insert(QStringLiteral("showExtendedSpectrumInfo"), showExtendedSpectrumInfo);
    visualization.insert(QStringLiteral("waterfallAreaMeasurementEnabled"), waterfallAreaMeasurementEnabled);
    visualization.insert(QStringLiteral("waterfall3DFixedPlane"), waterfall3DFixedPlane);
    visualization.insert(QStringLiteral("waterfall3DMonochrome"), waterfall3DMonochrome);
    visualization.insert(QStringLiteral("waterfall3DSurfaceStyle"), waterfall3DSurfaceStyle);
    visualization.insert(QStringLiteral("waterfall3DSmoothing"), waterfall3DSmoothing);
    visualization.insert(QStringLiteral("waterfall3DLighting"), waterfall3DLighting);
    visualization.insert(QStringLiteral("waterfall3DSpectrumSliceCapture"), waterfall3DSpectrumSliceCapture);
    visualization.insert(QStringLiteral("waterfall3DSpectrumSliceCaptureFixed"), waterfall3DSpectrumSliceCaptureFixed);
    visualization.insert(QStringLiteral("waterfall3DVncSliceInput"), waterfall3DVncSliceInput);
    visualization.insert(QStringLiteral("waterfall3DSliceScrollStep"), waterfall3DSliceScrollStep);
    visualization.insert(QStringLiteral("waterfall3DSliceWidth"), waterfall3DSliceWidth);
    visualization.insert(QStringLiteral("waterfall3DSpectrumSliceScrollStep"), waterfall3DSpectrumSliceScrollStep);
    visualization.insert(QStringLiteral("waterfall3DSpectrumSliceRows"), waterfall3DSpectrumSliceRows);
    visualization.insert(QStringLiteral("fftLength"), pendingSettings.fftLength);
    visualization.insert(QStringLiteral("fftWindowType"), pendingSettings.fftWindowType);
    dspFlowPanel->setWorkspaceVisualizationSettings(visualization);
}

void YourClassName::triggerDspControlBlock(const QString &controlType,
                                           const QString &controlId,
                                           const QString &targetType,
                                           const QString &targetId) {
    Q_UNUSED(controlId);
    if (controlType == QStringLiteral("enable_control") && targetType == QStringLiteral("ddc_channelizer") && dspFlowPanel) {
        auto settings = dspFlowPanel->blockSettings(targetId);
        settings.insert(QStringLiteral("enabled"), !settings.value(QStringLiteral("enabled")).toBool(false));
        dspFlowPanel->setBlockSettings(targetId, settings);
        refreshDspControlStates();
        return;
    }
    if (controlType == QStringLiteral("run_control")) {
        if (!targetType.isEmpty() && targetType != QStringLiteral("receiver_source") &&
            targetType != QStringLiteral("iq_source") && targetType != QStringLiteral("network_input") &&
            targetType != QStringLiteral("playback")) return;
        if (runState == RadioRunState::Idle) {
            if (startButton && startButton->isEnabled()) startButton->click();
        } else if (runState != RadioRunState::Stopping) {
            if (stopButton && stopButton->isEnabled()) stopButton->click();
        }
        refreshDspControlStates();
        return;
    }
    if (controlType != QStringLiteral("enable_control") || targetType.isEmpty()) return;
    if (targetType == QStringLiteral("multi_vfo_channelizer") && multiVfoWidget) {
        auto settings = QJsonDocument::fromJson(multiVfoWidget->configurationJson().toUtf8()).object();
        settings.insert(QStringLiteral("enabled"), !settings.value(QStringLiteral("enabled")).toBool(false));
        multiVfoWidget->setConfigurationJson(QString::fromUtf8(QJsonDocument(settings).toJson(QJsonDocument::Compact)));
        refreshDspControlStates(); savePersistentSettings(); return;
    }

    QCheckBox *toggle = nullptr;
    if (targetType == QStringLiteral("spectrum_display")) toggle = spectrumCheckbox;
    else if (targetType == QStringLiteral("second_spectrum")) toggle = graphCheckbox;
    else if (targetType == QStringLiteral("audio_output")) toggle = audioCheckbox;
    else if (targetType == QStringLiteral("decoder") || targetType == QStringLiteral("digital_audio_settings") ||
             targetType == QStringLiteral("dmr_decoder") ||
             targetType == QStringLiteral("cw_decoder")) toggle = digitalDecodeCheckbox;
    else if (targetType == QStringLiteral("sstv_decoder") || targetType == QStringLiteral("digital_video") ||
             targetType == QStringLiteral("digital_video_settings"))
        toggle = videoDecodeCheckbox;
    else if (targetType == QStringLiteral("agile_scan")) toggle = agileScanCheckbox;
    else if (targetType == QStringLiteral("standard_scan")) toggle = standardScanCheckbox;
    else if (targetType == QStringLiteral("listening_scan")) toggle = listeningScanCheckbox;
    else if (targetType == QStringLiteral("spectrum_measurement")) toggle = scanMeasurementCheckbox;
    else if (targetType == QStringLiteral("spur_suppression")) toggle = spurSuppressionCheckbox;
    else if (targetType == QStringLiteral("hf_interference")) toggle = hfInterferenceBaselineCheckbox;
    else if (targetType == QStringLiteral("gnss_sdr")) toggle = gnssMonitorCheckbox;

    if (toggle) {
        if (toggle->isEnabled()) toggle->click();
    } else if (targetType == QStringLiteral("waterfall_3d") && waterfallDisplayModeCombo) {
        const bool active = waterfallDisplayModeCombo->currentData().toInt() !=
                            static_cast<int>(MyWaterfallWidget::DisplayMode::Waterfall2D);
        const int mode = static_cast<int>(active ? MyWaterfallWidget::DisplayMode::Waterfall2D
                                                : MyWaterfallWidget::DisplayMode::Waterfall3D);
        const int index = waterfallDisplayModeCombo->findData(mode);
        if (index >= 0) waterfallDisplayModeCombo->setCurrentIndex(index);
    } else if (targetType == QStringLiteral("gnss_serial") && gnssSerialButton &&
               gnssSerialButton->isEnabled()) {
        gnssSerialButton->click();
    }
    refreshDspControlStates();
}

void YourClassName::openDspBlockReference(const QString &type, const QString &id) {
    const bool ukrainian = normalizedUiLanguage(uiLanguage) == QStringLiteral("uk");
    const auto text = [ukrainian](const char *uk, const char *en) {
        return QString::fromUtf8(ukrainian ? uk : en);
    };

    if (dspFlowPanel &&
        (type == QStringLiteral("workspace_spectrum") ||
         type == QStringLiteral("workspace_waterfall"))) {
        const QString controllerId = dspFlowPanel->boundWorkspaceSettingsBlockId(id);
        if (!controllerId.isEmpty()) {
            const QString controllerType = dspFlowPanel->blockTypeForId(controllerId);
            if (!controllerType.isEmpty()) {
                openDspBlockReference(controllerType, controllerId);
                return;
            }
        }
    }

    bool filterBlock = false;
    const AudioFilterKind filterKind = filterKindForBlock(type, &filterBlock);
    if (filterBlock) {
        if (audioFilterChainWidget) {
            audioFilterChainWidget->ensureStage(id, filterKind, false);
            audioFilterChainWidget->openStageEditor(id);
        }
        return;
    }

    if (type == QStringLiteral("network_output") || type == QStringLiteral("network_input")) {
        openNetworkSettingsDialog();
        return;
    }
    if (type == QStringLiteral("zoom_spectrum")) { openZoomSpectrum(); return; }
    if (type == QStringLiteral("zoom_density")) { openZoomDensity(); return; }
    if (type == QStringLiteral("zero_span")) { openZeroSpanDialog(); return; }
    if (type == QStringLiteral("research_analysis")) { openResearchAnalysis(0); return; }
    if (type == QStringLiteral("wireless_lab")) { openWirelessLab(); return; }
    if (type == QStringLiteral("radio_propagation")) { openPropagationLab(); return; }
    static const QHash<QString, ResearchAnalysisDialog::Tab> researchTabs = {
        {QStringLiteral("research_interference"), ResearchAnalysisDialog::InterferenceTab},
        {QStringLiteral("research_statistics"), ResearchAnalysisDialog::StatisticsTab},
        {QStringLiteral("research_iq"), ResearchAnalysisDialog::IqTab},
        {QStringLiteral("research_dual_input"), ResearchAnalysisDialog::DualInputTab},
        {QStringLiteral("research_analyzer"), ResearchAnalysisDialog::AnalyzerTab},
        {QStringLiteral("research_density"), ResearchAnalysisDialog::DensityTab},
        {QStringLiteral("research_masks"), ResearchAnalysisDialog::MasksTab},
        {QStringLiteral("research_pulse"), ResearchAnalysisDialog::PulseTab},
        {QStringLiteral("research_session"), ResearchAnalysisDialog::SessionTab}
    };
    const auto researchTab = researchTabs.constFind(type);
    if (researchTab != researchTabs.cend()) {
        openResearchAnalysis(static_cast<int>(researchTab.value()));
        return;
    }
    if (type == QStringLiteral("oscilloscope_view") ||
        type == QStringLiteral("constellation_view") ||
        type == QStringLiteral("eye_diagram_view") ||
        type == QStringLiteral("digital_sync_view")) return;
    if (type == QStringLiteral("spectrum_replay")) { openSpectrumFrameReplay(); return; }
    if (type == QStringLiteral("qth_map")) { openQthMapWindow(); return; }
    if (type == QStringLiteral("transmitter")) { openTransmitDialog(); return; }
    if (type == QStringLiteral("presets")) { openPresetManager(); return; }
    if (type == QStringLiteral("calibration")) { openPresetManager(); return; }
    if (type == QStringLiteral("application_settings")) { openApplicationSettings(); return; }
    const QString editorKey = id.isEmpty() ? type : QStringLiteral("%1:%2").arg(type, id);
    if (QDialog *existing = dspBlockEditors.value(editorKey).data()) {
        existing->show();
        existing->raise();
        existing->activateWindow();
        return;
    }

    if (type == QStringLiteral("fine_tune_control")) return;
    if (type == QStringLiteral("digital_text_output")) {
        auto *dialog = new QDialog(this, Qt::Tool);
        dialog->setAttribute(Qt::WA_DeleteOnClose, true);
        dialog->setModal(false);
        dialog->setWindowTitle(text("Цифровий текст", "Digital text"));
        dialog->resize(520, 300);
        auto *layout = new QVBoxLayout(dialog);
        auto *output = new QPlainTextEdit(dialog);
        output->setObjectName(QStringLiteral("dspDigitalTextOutput"));
        output->setReadOnly(true);
        output->setMaximumBlockCount(2000);
        if (digitalTextEdit) output->setPlainText(digitalTextEdit->toPlainText());
        auto *clearButton = new QPushButton(text("Очистити", "Clear"), dialog);
        layout->addWidget(output, 1);
        layout->addWidget(clearButton, 0, Qt::AlignRight);
        connect(clearButton, &QPushButton::clicked, output, &QPlainTextEdit::clear);
        if (digitalDecoder) {
            connect(digitalDecoder, &DigitalDecoder::textDecoded, output,
                    [output](const QString &decoded) {
                        if (decoded.isEmpty()) return;
                        QTextCursor cursor = output->textCursor();
                        cursor.movePosition(QTextCursor::End);
                        cursor.insertText(decoded);
                        output->setTextCursor(cursor);
                        output->ensureCursorVisible();
                    },
                    Qt::QueuedConnection);
        }
        dspBlockEditors.insert(editorKey, dialog);
        connect(dialog, &QObject::destroyed, this, [this, editorKey]() {
            dspBlockEditors.remove(editorKey);
        });
        dialog->show();
        dialog->raise();
        return;
    }
    if (type == QStringLiteral("digital_image_output")) {
        auto *dialog = new QDialog(this, Qt::Tool);
        dialog->setAttribute(Qt::WA_DeleteOnClose, true);
        dialog->setModal(false);
        dialog->setWindowTitle(text("Цифрове зображення", "Digital image"));
        dialog->resize(560, 430);
        auto *layout = new QVBoxLayout(dialog);
        auto *status = new QLabel(text("Очікування кадру", "Waiting for frame"), dialog);
        status->setWordWrap(true);
        auto *output = new VideoWidget(dialog);
        output->setObjectName(QStringLiteral("dspDigitalImageOutput"));
        layout->addWidget(status);
        layout->addWidget(output, 1);
        if (videoProcessor) {
            connect(videoProcessor, &VideoProcessor::frameReady,
                    output, &VideoWidget::setFrame, Qt::QueuedConnection);
            connect(videoProcessor, &VideoProcessor::statusChanged,
                    status, &QLabel::setText, Qt::QueuedConnection);
        }
        dspBlockEditors.insert(editorKey, dialog);
        connect(dialog, &QObject::destroyed, this, [this, editorKey]() {
            dspBlockEditors.remove(editorKey);
            QTimer::singleShot(0, this, [this]() { updateVideoProcessorMode(); });
        });
        dialog->show();
        dialog->raise();
        updateVideoProcessorMode();
        return;
    }

    auto *dialog = new QDialog(this, Qt::Tool);
    dialog->setAttribute(Qt::WA_DeleteOnClose, true);
    dialog->setModal(false);
    dialog->setMinimumWidth(370);
    auto *root = new QVBoxLayout(dialog);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(8);
    auto *form = new QFormLayout();
    form->setContentsMargins(0, 0, 0, 0);
    form->setHorizontalSpacing(10);
    form->setVerticalSpacing(7);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    root->addLayout(form);
    auto *syncTimer = new QTimer(dialog);
    syncTimer->setInterval(180);
    syncTimer->start();

    const auto addFrequencyControl = [dialog, syncTimer](FrequencyControl *source,
                                                               const std::function<void()> &apply) {
        return source->createLinkedControl(dialog, syncTimer, apply);
    };

    const auto actionButton = [this, dialog](QPushButton *source, const QString &caption) {
        auto *button = new QPushButton(caption, dialog);
        if (source) QObject::connect(button, &QPushButton::clicked, source, &QPushButton::click);
        else button->setEnabled(false);
        return button;
    };
    const auto commandButton = [this, dialog](const QString &caption, const std::function<void()> &command) {
        auto *button = new QPushButton(caption, dialog);
        QObject::connect(button, &QPushButton::clicked, dialog, command);
        return button;
    };
    const auto showDockButton = [this, dialog](QDockWidget *dock, const QString &caption) {
        auto *button = new QPushButton(caption, dialog);
        QObject::connect(button, &QPushButton::clicked, dialog, [dock]() {
            if (!dock) return;
            dock->show();
            dock->raise();
        });
        return button;
    };
    const auto buttonRow = [dialog](std::initializer_list<QPushButton*> buttons) {
        auto *container = new QWidget(dialog);
        auto *layout = new QHBoxLayout(container);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(5);
        for (QPushButton *button : buttons) layout->addWidget(button);
        return container;
    };

    QString title;
    if (DspBranchSettingsWidget::supportsType(type)) {
        title = DspBranchSettingsWidget::blockTitle(type, ukrainian);
        auto *settings = new DspBranchSettingsWidget(type, dspFlowPanel->blockSettings(id), ukrainian, dialog);
        root->addWidget(settings, 1);
        connect(settings, &DspBranchSettingsWidget::settingsEdited, dialog,
                [this, id](const QJsonObject &changes) {
                    if (!dspFlowPanel) return;
                    auto value = dspFlowPanel->blockSettings(id);
                    for (auto it = changes.constBegin(); it != changes.constEnd(); ++it)
                        value.insert(it.key(), it.value());
                    dspFlowPanel->setBlockSettings(id, value);
                });
        auto *status = new QLabel(dspFlowPanel->branchStatus(id), dialog);
        status->setWordWrap(true);
        root->addWidget(status);
        connect(syncTimer, &QTimer::timeout, status, [this, id, status]() {
            if (dspFlowPanel) status->setText(dspFlowPanel->branchStatus(id));
        });
        if (type == QStringLiteral("branch_audio_filter")) dialog->resize(620, 390);
    } else if (type == QStringLiteral("ddc_channelizer") || type == QStringLiteral("branch_fft") ||
        type == QStringLiteral("branch_spectrum") || type == QStringLiteral("branch_waterfall")) {
        const auto stored = dspFlowPanel->blockSettings(id);
        title = type == QStringLiteral("ddc_channelizer") ? text("DDC: додаткова IQ-гілка", "DDC: additional IQ branch") :
            type == QStringLiteral("branch_fft") ? text("FFT додаткової гілки", "Additional branch FFT") :
            type == QStringLiteral("branch_spectrum") ? text("Спектр гілки", "Branch spectrum") : text("Водоспад гілки", "Branch waterfall");
        auto *status = new QLabel(dspFlowPanel->branchStatus(id), dialog);
        status->setWordWrap(true);
        const auto number = [dialog, form](const QString &label, double value, double low, double high, int decimals, const QString &suffix) {
            auto *spin = new QDoubleSpinBox(dialog);
            spin->setRange(low, high); spin->setDecimals(decimals); spin->setSuffix(suffix);
            spin->setKeyboardTracking(false); spin->setValue(value); form->addRow(label, spin); return spin;
        };
        if (type == QStringLiteral("ddc_channelizer")) {
            auto *enabled = new QCheckBox(text("Увімкнути гілку", "Enable branch"), dialog);
            enabled->setChecked(stored.value("enabled").toBool(false)); form->addRow(enabled);
            auto *execution = new QComboBox(dialog);
            execution->addItem(text("На цьому ПК", "On this PC"), false);
            execution->addItem(text("Зовнішній обробник / GNU Radio", "External processor / GNU Radio"), true);
            execution->setCurrentIndex(stored.value("externalProcessing").toBool(false) ? 1 : 0);
            form->addRow(text("Обробка каналу:", "Channel processing:"), execution);
            auto *follow = new QCheckBox(text("Частота основного прослуховування", "Follow main listening frequency"), dialog);
            follow->setChecked(stored.value("frequencyHz").toDouble(0.0) == 0.0); form->addRow(follow);
            auto *frequency = number(text("Частота:", "Frequency:"),
                (stored.value("frequencyHz").toDouble(0.0) > 0.0 ? stored.value("frequencyHz").toDouble() : pendingSettings.listeningFrequency)/1.0e6, 0.0, 8000.0, 6, QStringLiteral(" MHz"));
            frequency->setEnabled(!follow->isChecked());
            auto *bandwidth = number(text("Смуга каналу:", "Channel bandwidth:"), stored.value("bandwidthHz").toDouble(12500.0)/1000.0,
                0.1, 1500.0, 3, QStringLiteral(" kHz"));
            auto *rate = number(text("Вихідний sample rate:", "Output sample rate:"), stored.value("outputRateHz").toDouble(192000.0)/1000.0,
                1.0, 2000.0, 3, QStringLiteral(" ksps"));
            auto *decimation = new QSpinBox(dialog);
            decimation->setRange(0, 4096); decimation->setSpecialValueText(QStringLiteral("Auto"));
            decimation->setKeyboardTracking(false); decimation->setValue(stored.value("decimation").toInt(0));
            form->addRow(text("Децимація до ресемплера:", "Decimation before resampler:"), decimation);
            auto *firTaps = new QComboBox(dialog);
            firTaps->setObjectName(QStringLiteral("firTaps"));
            for (int taps : {16, 32, 64, 128, 256}) firTaps->addItem(QString::number(taps), taps);
            const int savedTaps = firTaps->findData(stored.value("firTaps").toInt(32));
            firTaps->setCurrentIndex(savedTaps < 0 ? firTaps->findData(32) : savedTaps);
            form->addRow(text("Коефіцієнтів FIR:", "FIR taps:"), firTaps);
            auto *transition = number(text("Перехідна смуга FIR:", "FIR transition band:"),
                stored.value("transitionHz").toDouble(0.0) / 1000.0, 0.0, 500.0, 3, text(" кГц", " kHz"));
            transition->setObjectName(QStringLiteral("transitionHz"));
            transition->setSpecialValueText(text("Авто", "Auto"));
            const auto apply = [this, id, enabled, follow, frequency, bandwidth, rate, decimation, execution, firTaps, transition]() {
                auto value = dspFlowPanel->blockSettings(id);
                value.insert("enabled", enabled->isChecked());
                value.insert("frequencyHz", follow->isChecked() ? 0.0 : frequency->value()*1.0e6);
                value.insert("bandwidthHz", bandwidth->value()*1000.0);
                value.insert("outputRateHz", rate->value()*1000.0);
                value.insert("decimation", decimation->value());
                value.insert("firTaps", firTaps->currentData().toInt());
                value.insert("transitionHz", transition->value() * 1000.0);
                value.insert("externalProcessing", execution->currentData().toBool());
                frequency->setEnabled(!follow->isChecked());
                dspFlowPanel->setBlockSettings(id, value);
            };
            connect(enabled, &QCheckBox::toggled, dialog, [apply](bool) { apply(); });
            connect(execution, QOverload<int>::of(&QComboBox::currentIndexChanged), dialog, [apply](int) { apply(); });
            connect(follow, &QCheckBox::toggled, dialog, [apply](bool) { apply(); });
            for (auto *spin : {frequency, bandwidth, rate, transition}) connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), dialog, [apply](double) { apply(); });
            connect(firTaps, QOverload<int>::of(&QComboBox::currentIndexChanged), dialog, [apply](int) { apply(); });
            connect(decimation, QOverload<int>::of(&QSpinBox::valueChanged), dialog, [apply](int) { apply(); });
            auto *record = new QPushButton(text("Запис IQ гілки", "Record branch IQ"), dialog);
            auto *stopRecord = new QPushButton(text("Зупинити запис", "Stop recording"), dialog);
            form->addRow(buttonRow({record, stopRecord}));
            connect(record, &QPushButton::clicked, dialog, [this, id, dialog, status, text]() {
                const QString folder = QFileDialog::getExistingDirectory(dialog, text("Папка запису IQ гілки", "Branch IQ recording directory"));
                if (!folder.isEmpty() && !dspFlowPanel->startBranchRecording(id, folder))
                    status->setText(text("Увімкніть приймач і гілку; вимкніть спрощений режим", "Enable receiver and branch; disable simplified mode"));
            });
            connect(stopRecord, &QPushButton::clicked, dialog, [this, id]() { dspFlowPanel->stopBranchRecording(id); });
            connect(syncTimer, &QTimer::timeout, dialog, [this, id, record, stopRecord]() {
                const bool recording = dspFlowPanel->branchRecordingRequested(id);
                record->setEnabled(!recording); stopRecord->setEnabled(recording);
            });
        } else if (type == QStringLiteral("branch_fft")) {
            auto *fft = new QComboBox(dialog);
            for (int n = 512; n <= 65536; n *= 2) fft->addItem(QString::number(n), n);
            fft->setCurrentIndex(std::max(0, fft->findData(stored.value("fftLength").toInt(2048))));
            form->addRow(QStringLiteral("FFT:"), fft);
            auto *window = new QComboBox(dialog);
            for (int w = FFT_WINDOW_RECTANGULAR; w <= FFT_WINDOW_FLAT_TOP; ++w) window->addItem(QString::fromLatin1(fftWindowTypeName(w)), w);
            window->setCurrentIndex(std::max(0, window->findData(stored.value("window").toInt(FFT_WINDOW_HANN))));
            form->addRow(text("Вікно:", "Window:"), window);
            auto *refresh = new QSpinBox(dialog);
            refresh->setRange(20, 2000); refresh->setSuffix(QStringLiteral(" ms")); refresh->setKeyboardTracking(false);
            refresh->setValue(stored.value("refreshMs").toInt(100)); form->addRow(text("Оновлення:", "Refresh:"), refresh);
            const auto apply = [this, id, fft, window, refresh]() {
                auto value = dspFlowPanel->blockSettings(id);
                value.insert("fftLength", fft->currentData().toInt()); value.insert("window", window->currentData().toInt());
                value.insert("refreshMs", refresh->value()); dspFlowPanel->setBlockSettings(id, value);
            };
            for (auto *combo : {fft, window}) connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), dialog, [apply](int) { apply(); });
            connect(refresh, QOverload<int>::of(&QSpinBox::valueChanged), dialog, [apply](int) { apply(); });
        } else {
            auto *minimum = number(text("Мінімум:", "Minimum:"), stored.value("minimumDbfs").toDouble(-140.0), -200.0, 19.0, 1, QStringLiteral(" dBFS"));
            auto *maximum = number(text("Максимум:", "Maximum:"), stored.value("maximumDbfs").toDouble(-40.0), -199.0, 20.0, 1, QStringLiteral(" dBFS"));
            const auto apply = [this, id, minimum, maximum]() {
                if (maximum->value() <= minimum->value()) return;
                auto value = dspFlowPanel->blockSettings(id);
                value.insert("minimumDbfs", minimum->value()); value.insert("maximumDbfs", maximum->value());
                dspFlowPanel->setBlockSettings(id, value);
            };
            connect(minimum, QOverload<double>::of(&QDoubleSpinBox::valueChanged), dialog, [apply](double) { apply(); });
            connect(maximum, QOverload<double>::of(&QDoubleSpinBox::valueChanged), dialog, [apply](double) { apply(); });
        }
        form->addRow(text("Стан:", "Status:"), status);
        connect(syncTimer, &QTimer::timeout, status, [this, id, status]() { status->setText(dspFlowPanel->branchStatus(id)); });
    } else if (type == QStringLiteral("oscilloscope") || type == QStringLiteral("constellation") ||
        type == QStringLiteral("eye_diagram") || type == QStringLiteral("digital_sync_lab")) {
        title = type == QStringLiteral("oscilloscope")
                    ? text("Осцилограф: параметри", "Oscilloscope settings")
                    : (type == QStringLiteral("constellation")
                           ? text("Сузір'я: параметри", "Constellation settings")
                           : (type == QStringLiteral("eye_diagram")
                                  ? text("Окова діаграма: параметри", "Eye diagram settings")
                                  : text("Синхронізація: параметри", "Synchronization settings")));
        QJsonObject settings = dspFlowPanel ? dspFlowPanel->blockSettings(id) : QJsonObject();
        auto *samples = new QSpinBox(dialog);
        samples->setRange(256, 262144);
        samples->setSingleStep(256);
        samples->setValue(settings.value(QStringLiteral("samples")).toInt(4096));
        auto *refresh = new QSpinBox(dialog);
        refresh->setRange(10, 1000);
        refresh->setSuffix(QStringLiteral(" ms"));
        refresh->setValue(settings.value(QStringLiteral("refreshMs")).toInt(40));
        auto *gain = new QDoubleSpinBox(dialog);
        gain->setRange(0.1, 20.0);
        gain->setDecimals(2);
        gain->setSingleStep(0.1);
        gain->setSuffix(QStringLiteral(" x"));
        gain->setValue(settings.value(QStringLiteral("gain")).toDouble(1.0));
        form->addRow(text("Відліки:", "Samples:"), samples);
        form->addRow(text("Оновлення:", "Refresh:"), refresh);
        form->addRow(text("Масштаб Y:", "Y scale:"), gain);

        QDoubleSpinBox *symbolRate = nullptr;
        QDoubleSpinBox *phase = nullptr;
        QSpinBox *traces = nullptr;
        QCheckBox *constellation3D = nullptr;
        QCheckBox *constellationSurface = nullptr;
        if (type == QStringLiteral("constellation")) {
            constellation3D = new QCheckBox(text("3D-сузір'я", "3D constellation"), dialog);
            constellation3D->setChecked(settings.value(QStringLiteral("constellation3D")).toBool(false));
            form->addRow(constellation3D);
            constellationSurface = new QCheckBox(text("Радіальна поверхня", "Radial surface"), dialog);
            constellationSurface->setChecked(settings.value(QStringLiteral("constellationSurface")).toBool(false));
            constellationSurface->setToolTip(text("Інтерполяція лише для відображення, не відновлення IQ.", "Visual interpolation only, not IQ reconstruction."));
            form->addRow(constellationSurface);
        }
        if (type == QStringLiteral("constellation") || type == QStringLiteral("eye_diagram") || type == QStringLiteral("digital_sync_lab")) {
            symbolRate = new QDoubleSpinBox(dialog);
            symbolRate->setRange(1.0, 5000000.0);
            symbolRate->setDecimals(1);
            symbolRate->setSuffix(QStringLiteral(" Bd"));
            symbolRate->setValue(settings.value(QStringLiteral("symbolRate")).toDouble(4800.0));
            form->addRow(text("Символьна швидкість:", "Symbol rate:"), symbolRate);
        }
        if (type == QStringLiteral("constellation") || type == QStringLiteral("eye_diagram")) {
            phase = new QDoubleSpinBox(dialog);
            phase->setRange(0.0, 100.0);
            phase->setDecimals(1);
            phase->setSuffix(QStringLiteral(" %"));
            phase->setValue(settings.value(QStringLiteral("phasePercent")).toDouble(50.0));
            form->addRow(text("Фаза:", "Phase:"), phase);
            if (type == QStringLiteral("eye_diagram")) {
                traces = new QSpinBox(dialog);
                traces->setRange(1, 256);
                traces->setValue(settings.value(QStringLiteral("traces")).toInt(32));
                form->addRow(text("Траси:", "Traces:"), traces);
            }
        }

        const auto applyResearchSettings = [this, id, samples, refresh, gain, symbolRate, phase, traces, constellation3D, constellationSurface]() {
            if (!dspFlowPanel) return;
            QJsonObject value;
            value.insert(QStringLiteral("samples"), samples->value());
            value.insert(QStringLiteral("refreshMs"), refresh->value());
            value.insert(QStringLiteral("gain"), gain->value());
            if (symbolRate) value.insert(QStringLiteral("symbolRate"), symbolRate->value());
            if (phase) value.insert(QStringLiteral("phasePercent"), phase->value());
            if (traces) value.insert(QStringLiteral("traces"), traces->value());
            if (constellation3D) value.insert(QStringLiteral("constellation3D"), constellation3D->isChecked());
            if (constellationSurface) value.insert(QStringLiteral("constellationSurface"), constellationSurface->isChecked());
            dspFlowPanel->setBlockSettings(id, value);
        };
        connect(samples, QOverload<int>::of(&QSpinBox::valueChanged), dialog,
                [applyResearchSettings](int) { applyResearchSettings(); });
        connect(refresh, QOverload<int>::of(&QSpinBox::valueChanged), dialog,
                [applyResearchSettings](int) { applyResearchSettings(); });
        connect(gain, QOverload<double>::of(&QDoubleSpinBox::valueChanged), dialog,
                [applyResearchSettings](double) { applyResearchSettings(); });
        if (symbolRate) connect(symbolRate, QOverload<double>::of(&QDoubleSpinBox::valueChanged), dialog,
                                [applyResearchSettings](double) { applyResearchSettings(); });
        if (phase) connect(phase, QOverload<double>::of(&QDoubleSpinBox::valueChanged), dialog,
                           [applyResearchSettings](double) { applyResearchSettings(); });
        if (traces) connect(traces, QOverload<int>::of(&QSpinBox::valueChanged), dialog,
                            [applyResearchSettings](int) { applyResearchSettings(); });
        if (constellation3D) {
            symbolRate->setEnabled(constellation3D->isChecked());
            phase->setEnabled(constellation3D->isChecked());
            constellationSurface->setEnabled(constellation3D->isChecked());
            connect(constellationSurface, &QCheckBox::toggled, dialog,
                    [applyResearchSettings](bool) { applyResearchSettings(); });
            connect(constellation3D, &QCheckBox::toggled, dialog,
                    [applyResearchSettings, symbolRate, phase, constellationSurface](bool enabled) {
                        symbolRate->setEnabled(enabled);
                        phase->setEnabled(enabled);
                        constellationSurface->setEnabled(enabled);
                        applyResearchSettings();
                    });
        }

        auto *fullAnalysis = new QPushButton(text("Повне вікно аналізу", "Open full analysis"), dialog);
        connect(fullAnalysis, &QPushButton::clicked, dialog, [this, type, constellation3D]() {
            const bool synchronization = type == QStringLiteral("digital_sync_lab");
            openResearchAnalysis(static_cast<int>(synchronization
                ? ResearchAnalysisDialog::SynchronizationTab : ResearchAnalysisDialog::IqTab));
            if (!synchronization && researchAnalysisDialog) {
                const int viewMode = type == QStringLiteral("oscilloscope")
                                         ? 1
                                         : (type == QStringLiteral("constellation") ? (constellation3D && constellation3D->isChecked() ? 4 : 2) : 3);
                researchAnalysisDialog->setIqView(viewMode);
            }
        });
        root->addWidget(fullAnalysis);
    } else if (type == QStringLiteral("receiver_source") || type == QStringLiteral("iq_source")) {
        title = text("Вхід приймача", "Receiver input");
        form->addRow(text("Приймач:", "Receiver:"), linkedCombo(comboBox, dialog, syncTimer));
        form->addRow(text("Вхід / режим:", "Input / mode:"), linkedCombo(modeBox, dialog, syncTimer));
        form->addRow(text("Джерело такту:", "Clock source:"), linkedCombo(clkBox, dialog, syncTimer));
        form->addRow(text("Sample rate:", "Sample rate:"), linkedCombo(sampleBox, dialog, syncTimer));
        form->addRow(QStringLiteral("FFT:"), linkedCombo(fftComboBox, dialog, syncTimer));
        form->addRow(QString(), linkedCheckBox(fftBinWidthModeCheckbox, QStringLiteral("Hz/point"), dialog, syncTimer));
        form->addRow(QStringLiteral("Hz/point:"), linkedDoubleSpin(fftBinWidthSpin, dialog, syncTimer));
        form->addRow(QStringLiteral("LNA:"), linkedSlider(lnaGainSlider, dialog, syncTimer));
        form->addRow(QStringLiteral("VGA:"), linkedSlider(vgaGainSlider, dialog, syncTimer));
        form->addRow(QString(), linkedCheckBox(rtlAgcCheckbox, QStringLiteral("RTL AGC"), dialog, syncTimer));
        form->addRow(QStringLiteral("RTL gain:"), linkedSlider(rtlGainSlider, dialog, syncTimer, QStringLiteral(" dB"), 10.0));
        form->addRow(QString(), buttonRow({
            actionButton(fobosButton, text("Деталі Fobos", "Fobos details")),
            actionButton(hackRfSettingsButton, QStringLiteral("HackRF RX..."))
        }));
        auto *refresh = new QPushButton(text("Оновити список пристроїв", "Refresh device list"), dialog);
        connect(refresh, &QPushButton::clicked, refreshButton, &QPushButton::click);
        form->addRow(QString(), refresh);
        form->addRow(QString(), buttonRow({actionButton(startButton, text("Старт", "Start")),
                                          actionButton(stopButton, text("Стоп", "Stop"))}));
    } else if (type == QStringLiteral("frequency_control")) {
        title = text("Частоти", "Frequencies");
        form->addRow(text("Центральна:", "Center:"),
                     addFrequencyControl(frequencyControl,
                                         [this]() { onFrequencyEntered(); }));
        form->addRow(text("Прослуховування:", "Listening:"),
                     addFrequencyControl(listeningFrequencyControl,
                                         [this]() { onListeningFrequencyEntered(); }));
    } else if (type == QStringLiteral("fft")) {
        title = text("FFT і роздільність", "FFT and resolution");
        if (dspFlowPanel) {
            auto *compute = new SpectrumComputeControls(ukrainian, dialog);
            compute->setOptions(dspFlowPanel->spectrumComputeOptions());
            compute->setStatus(dspFlowPanel->spectrumComputeStatus());
            form->addRow(compute);
            connect(compute, &SpectrumComputeControls::applyRequested, dialog, [this, compute]() {
                if (dspFlowPanel) dspFlowPanel->setSpectrumComputeOptions(compute->options());
            });
            connect(dspFlowPanel, &DspFlowPanel::spectrumComputeOptionsChanged, compute, [this, compute]() {
                if (dspFlowPanel) compute->setOptions(dspFlowPanel->spectrumComputeOptions());
            });
            connect(dspFlowPanel, &DspFlowPanel::spectrumComputeStatusChanged,
                    compute, &SpectrumComputeControls::setStatus);
            form->addRow(QString(), commandButton(text("Менеджер потоків DSP", "DSP stream manager"), [this]() {
                if (dspFlowPanel) dspFlowPanel->showComputeDialog();
            }));
        }
        form->addRow(QStringLiteral("FFT:"), linkedCombo(fftComboBox, dialog, syncTimer));
        form->addRow(QString(), linkedCheckBox(fftBinWidthModeCheckbox, QStringLiteral("Hz/point"), dialog, syncTimer));
        form->addRow(QStringLiteral("Hz/point:"), linkedDoubleSpin(fftBinWidthSpin, dialog, syncTimer));
        auto *window = new QComboBox(dialog);
        for (int w = FFT_WINDOW_RECTANGULAR; w <= FFT_WINDOW_FLAT_TOP; ++w) window->addItem(QString::fromLatin1(fftWindowTypeName(w)), w);
        window->setCurrentIndex(window->findData(normalizedFftWindowType(pendingSettings.fftWindowType)));
        form->addRow(text("Вікно FFT:", "FFT window:"), window);
        connect(window, QOverload<int>::of(&QComboBox::currentIndexChanged), dialog, [this, window](int) {
            pendingSettings.fftWindowType = normalizedFftWindowType(window->currentData().toInt()); savePersistentSettings();
        });
        connect(syncTimer, &QTimer::timeout, window, [this, window]() {
            if (focusInside(window)) return;
            const QSignalBlocker blocker(window); window->setCurrentIndex(window->findData(normalizedFftWindowType(pendingSettings.fftWindowType)));
        });
    } else if (type == QStringLiteral("display_scale")) {
        title = text("Масштаб і рівні", "Scale and levels");
        form->addRow(text("Масштаб:", "Scale:"),
                     linkedSlider(scaleSlider, dialog, syncTimer,
                                  QStringLiteral("%"), SCALE_SLIDER_FACTOR));
        form->addRow(text("Дод. масштаб:", "Extra scale:"),
                     linkedSlider(additionalScaleDivisorSlider, dialog, syncTimer));
        form->addRow(text("Мінімум:", "Minimum:"),
                     linkedSlider(levelMinSlider, dialog, syncTimer,
                                  QStringLiteral(" dBFS"), LEVEL_SLIDER_FACTOR));
        form->addRow(text("Максимум:", "Maximum:"),
                     linkedSlider(levelMaxSlider, dialog, syncTimer,
                                  QStringLiteral(" dBFS"), LEVEL_SLIDER_FACTOR));
        auto *hint = new QLabel(
            text("Глобальні значення синхронно застосовуються до спектра, водоспаду та частотної шкали.",
                 "Global values are applied synchronously to spectrum, waterfall, and frequency ruler."),
            dialog);
        hint->setWordWrap(true);
        form->addRow(QString(), hint);
    } else if (type == QStringLiteral("hf_interference")) {
        title = text("HF лабораторія завад", "HF interference lab");
        form->addRow(QString(), linkedCheckBox(hfNoiseCancelFreezeCheckbox,
                                               text("Заморозити оцінку", "Freeze estimate"), dialog, syncTimer));
        form->addRow(text("Глибина придушення:", "Cancellation depth:"),
                     linkedSlider(hfNoiseCancelDepthSlider, dialog, syncTimer, QStringLiteral("%"), 1.0));
        form->addRow(text("Підсилення опори:", "Reference gain:"),
                     linkedSlider(hfNoiseCancelRefGainSlider, dialog, syncTimer, QStringLiteral(" dB"), 10.0));
        form->addRow(text("Затримка опори:", "Reference delay:"),
                     linkedSlider(hfNoiseCancelRefDelaySlider, dialog, syncTimer, QStringLiteral(" ns")));
        form->addRow(text("Нахил опори:", "Reference tilt:"),
                     linkedSlider(hfNoiseCancelRefTiltSlider, dialog, syncTimer, QStringLiteral(" dB"), 10.0));
        form->addRow(QString(), linkedCheckBox(hfAudioBlankerCheckbox,
                                               text("Аудіо blanker", "Audio blanker"), dialog, syncTimer));
        form->addRow(text("Поріг blanker:", "Blanker threshold:"),
                     linkedSlider(hfAudioBlankerThresholdSlider, dialog, syncTimer, QStringLiteral(" x"), 10.0));
        form->addRow(QString(), linkedCheckBox(hfInterferenceBaselineCheckbox,
                                               text("Віднімати baseline", "Subtract baseline"), dialog, syncTimer));
        form->addRow(QString(), linkedCheckBox(hfInterferenceRawOverlayCheckbox,
                                               text("Показати сирий спектр", "Show raw spectrum"), dialog, syncTimer));
        form->addRow(text("Глибина baseline:", "Baseline depth:"),
                     linkedSlider(hfInterferenceBaselineDepthSlider, dialog, syncTimer, QStringLiteral("%")));
        form->addRow(text("Згладжування:", "Smoothing:"),
                     linkedSlider(hfInterferenceBaselineSmoothSlider, dialog, syncTimer));
        form->addRow(QString(), buttonRow({actionButton(hfInterferenceBaselineLearnButton, text("Навчити", "Learn")),
                                          actionButton(hfInterferenceBaselineClearButton, text("Очистити", "Clear")),
                                          actionButton(hfInterferenceDefaultsButton, text("Типові", "Defaults"))}));
        form->addRow(QString(), actionButton(hfInterferenceAnalyzeButton,
                                             text("Відкрити аналіз", "Open analysis")));
    } else if (type == QStringLiteral("resampler")) {
        title = text("Sample rate приймача", "Receiver sample rate");
        form->addRow(text("Sample rate:", "Sample rate:"), linkedCombo(sampleBox, dialog, syncTimer));
    } else if (type == QStringLiteral("workspace_spectrum") ||
               type == QStringLiteral("workspace_ruler") ||
               type == QStringLiteral("workspace_waterfall")) {
        title = type == QStringLiteral("workspace_spectrum")
                    ? text("Робочий спектр", "Workspace spectrum")
                    : (type == QStringLiteral("workspace_ruler")
                           ? text("Лінійка частот", "Frequency ruler")
                           : text("Робочий водоспад", "Workspace waterfall"));
        const QJsonObject blockSettings = dspFlowPanel
                                              ? dspFlowPanel->blockSettings(id)
                                              : QJsonObject();
        auto *paused = new QCheckBox(text("Пауза", "Pause"), dialog);
        paused->setChecked(blockSettings.value(QStringLiteral("paused")).toBool(false));
        form->addRow(QString(), paused);
        QComboBox *workspaceDisplayMode = nullptr;
        if (type == QStringLiteral("workspace_waterfall")) {
            workspaceDisplayMode = new QComboBox(dialog);
            if (waterfallDisplayModeCombo) {
                for (int index = 0; index < waterfallDisplayModeCombo->count(); ++index) {
                    workspaceDisplayMode->addItem(waterfallDisplayModeCombo->itemText(index),
                                                  waterfallDisplayModeCombo->itemData(index));
                }
            }
            if (workspaceDisplayMode->count() == 0) {
                workspaceDisplayMode->addItem(text("2D водоспад", "2D waterfall"), 0);
                workspaceDisplayMode->addItem(text("3D водоспад", "3D waterfall"), 1);
                workspaceDisplayMode->addItem(text("3D + міні", "3D + mini"), 2);
            }
            const int globalMode = waterfallDisplayModeCombo
                                       ? waterfallDisplayModeCombo->currentData().toInt()
                                       : 0;
            const int storedMode = blockSettings.value(QStringLiteral("displayMode")).toInt(globalMode);
            const int modeIndex = workspaceDisplayMode->findData(storedMode);
            workspaceDisplayMode->setCurrentIndex(modeIndex >= 0 ? modeIndex : 0);
            form->addRow(text("Режим цього віджета:", "This widget mode:"), workspaceDisplayMode);
        }

        QCheckBox *workspaceColor = nullptr;
        QCheckBox *workspaceGradient = nullptr;
        QSlider *workspaceGradientOpacity = nullptr;
        QSlider *workspaceSensitivity = nullptr;
        QSlider *workspaceContrast = nullptr;
        QCheckBox *workspaceFixedPlane = nullptr;
        QCheckBox *workspaceMonochrome = nullptr;
        QComboBox *workspaceResolution = nullptr;
        QComboBox *workspaceSurface = nullptr;
        QComboBox *workspaceSmoothing = nullptr;
        QComboBox *workspaceLighting = nullptr;
        QSlider *workspaceHistory = nullptr;
        QSlider *workspaceFrequencyStep = nullptr;
        QSlider *workspaceFrequencyWidth = nullptr;
        QSlider *workspaceSpectrumStep = nullptr;
        QSlider *workspaceSpectrumRows = nullptr;
        QCheckBox *workspaceCapture = nullptr;
        QCheckBox *workspaceCaptureFixed = nullptr;
        QCheckBox *workspaceVncInput = nullptr;
        if (type == QStringLiteral("workspace_waterfall")) {
            form->addRow(text("Чутливість:", "Sensitivity:"),
                         valueSlider(&workspaceSensitivity, dialog, 1, 30,
                                     blockSettings.value(QStringLiteral("sensitivity")).toInt(sensitivity)));
            form->addRow(text("Контраст:", "Contrast:"),
                         valueSlider(&workspaceContrast, dialog, 1, 20,
                                     blockSettings.value(QStringLiteral("contrast")).toInt(contrast)));
        }
        if (type == QStringLiteral("workspace_spectrum") ||
            type == QStringLiteral("workspace_waterfall")) {
            workspaceColor = new QCheckBox(text("Кольоровий спектр", "Colored spectrum"), dialog);
            workspaceColor->setChecked(
                blockSettings.value(QStringLiteral("colorSpectrum")).toBool(
                    colorCheckbox ? colorCheckbox->isChecked() : true));
        }
        if (type == QStringLiteral("workspace_spectrum") ||
            type == QStringLiteral("workspace_waterfall")) {
            workspaceGradient = new QCheckBox(text("Градієнт спектра", "Spectrum gradient"), dialog);
            workspaceGradient->setChecked(
                blockSettings.value(QStringLiteral("spectrumGradientFill")).toBool(
                    alternativeSpectrumGradientCheckbox
                        ? alternativeSpectrumGradientCheckbox->isChecked()
                        : false));
            form->addRow(QString(), workspaceColor);
            form->addRow(QString(), workspaceGradient);
            form->addRow(text("Прозорість градієнта:", "Gradient opacity:"),
                         valueSlider(&workspaceGradientOpacity, dialog, 0, 100,
                                     blockSettings.value(QStringLiteral("spectrumGradientOpacity")).toInt(
                                         alternativeSpectrumGradientOpacity),
                                     QStringLiteral("%")));
        }
        if (type == QStringLiteral("workspace_waterfall")) {
            workspaceFixedPlane = new QCheckBox(
                text("Зафіксувати площину", "Fix plane"), dialog);
            workspaceMonochrome = new QCheckBox(
                text("Однотонний синій 3D", "Monochrome blue 3D"), dialog);
            workspaceFixedPlane->setChecked(
                blockSettings.value(QStringLiteral("waterfall3DFixedPlane")).toBool(
                    waterfall3DFixedPlane));
            workspaceMonochrome->setChecked(
                blockSettings.value(QStringLiteral("waterfall3DMonochrome")).toBool(
                    waterfall3DMonochrome));
            form->addRow(QString(), workspaceFixedPlane);
            form->addRow(QString(), workspaceMonochrome);
            workspaceResolution = new QComboBox(dialog);
            for (int divisor : {1, 2, 4, 8, 16, 32, 64})
                workspaceResolution->addItem(QStringLiteral("1/%1").arg(divisor), divisor);
            workspaceResolution->setCurrentIndex((std::max)(0, workspaceResolution->findData(
                blockSettings.value(QStringLiteral("resolutionDivisor")).toInt(waterfall3DResolutionDivisor))));
            form->addRow(text("Роздільність:", "Resolution:"), workspaceResolution);
            workspaceSurface = new QComboBox(dialog);
            workspaceSurface->addItem(text("Оригінал / голки", "Original / needles"), 0);
            workspaceSurface->addItem(text("Суцільна поверхня", "Solid surface"), 1);
            workspaceSurface->setCurrentIndex((std::max)(0, workspaceSurface->findData(
                blockSettings.value(QStringLiteral("waterfall3DSurfaceStyle")).toInt(waterfall3DSurfaceStyle))));
            form->addRow(text("Поверхня:", "Surface:"), workspaceSurface);
            workspaceSmoothing = new QComboBox(dialog);
            workspaceSmoothing->addItem(text("Вимкнено", "Off"), 0);
            workspaceSmoothing->addItem(text("М'яке", "Soft"), 1);
            workspaceSmoothing->addItem(text("Сильне", "Strong"), 2);
            workspaceSmoothing->setCurrentIndex((std::max)(0, workspaceSmoothing->findData(
                blockSettings.value(QStringLiteral("waterfall3DSmoothing")).toInt(waterfall3DSmoothing))));
            form->addRow(text("Згладжування:", "Smoothing:"), workspaceSmoothing);
            workspaceLighting = new QComboBox(dialog);
            workspaceLighting->addItem(text("Вимкнено", "Off"), 0);
            workspaceLighting->addItem(text("М'які тіні", "Soft shadows"), 1);
            workspaceLighting->addItem(text("Сильні тіні", "Strong shadows"), 2);
            workspaceLighting->setCurrentIndex((std::max)(0, workspaceLighting->findData(
                blockSettings.value(QStringLiteral("waterfall3DLighting")).toInt(waterfall3DLighting))));
            form->addRow(text("Освітлення:", "Lighting:"), workspaceLighting);
            form->addRow(text("Пам'ять:", "Memory:"),
                         valueSlider(&workspaceHistory, dialog, 16, 2048,
                                     blockSettings.value(QStringLiteral("historyRows")).toInt(waterfall3DHistoryRows),
                                     text(" рядків", " rows")));
            form->addRow(text("Крок частотного зрізу:", "Frequency slice step:"),
                         valueSlider(&workspaceFrequencyStep, dialog, 1, 256,
                                     blockSettings.value(QStringLiteral("waterfall3DSliceScrollStep")).toInt(
                                         waterfall3DSliceScrollStep), text(" тчк", " pt")));
            form->addRow(text("Ширина частотного зрізу:", "Frequency slice width:"),
                         valueSlider(&workspaceFrequencyWidth, dialog, 1, 4096,
                                     blockSettings.value(QStringLiteral("waterfall3DSliceWidth")).toInt(
                                         waterfall3DSliceWidth), text(" тчк", " pt")));
            form->addRow(text("Крок часового зрізу:", "Time slice step:"),
                         valueSlider(&workspaceSpectrumStep, dialog, 1, 2048,
                                     blockSettings.value(QStringLiteral("waterfall3DSpectrumSliceScrollStep")).toInt(
                                         waterfall3DSpectrumSliceScrollStep), text(" ряд.", " rows")));
            form->addRow(text("Ширина часового зрізу:", "Time slice rows:"),
                         valueSlider(&workspaceSpectrumRows, dialog, 1, 2048,
                                     blockSettings.value(QStringLiteral("waterfall3DSpectrumSliceRows")).toInt(
                                         waterfall3DSpectrumSliceRows), text(" ряд.", " rows")));
            workspaceCapture = new QCheckBox(text("Захват", "Capture"), dialog);
            workspaceCaptureFixed = new QCheckBox(text("Зафіксувати захват", "Freeze capture"), dialog);
            workspaceVncInput = new QCheckBox(
                text("Зрізи без Alt/Shift (VNC)", "Slices without Alt/Shift (VNC)"), dialog);
            workspaceCapture->setChecked(blockSettings.value(
                QStringLiteral("waterfall3DSpectrumSliceCapture")).toBool(waterfall3DSpectrumSliceCapture));
            workspaceCaptureFixed->setChecked(blockSettings.value(
                QStringLiteral("waterfall3DSpectrumSliceCaptureFixed")).toBool(
                    waterfall3DSpectrumSliceCaptureFixed));
            workspaceCaptureFixed->setEnabled(workspaceCapture->isChecked());
            workspaceVncInput->setChecked(blockSettings.value(
                QStringLiteral("waterfall3DVncSliceInput")).toBool(waterfall3DVncSliceInput));
            auto *captureRow = new QWidget(dialog);
            auto *captureLayout = new QHBoxLayout(captureRow);
            captureLayout->setContentsMargins(0, 0, 0, 0);
            captureLayout->addWidget(workspaceCapture);
            captureLayout->addWidget(workspaceCaptureFixed);
            captureLayout->addStretch(1);
            form->addRow(QString(), captureRow);
            form->addRow(QString(), workspaceVncInput);
        }
        if (type != QStringLiteral("workspace_ruler")) {
            form->addRow(text("Масштаб:", "Scale:"), linkedSlider(scaleSlider, dialog, syncTimer, QStringLiteral("%"), SCALE_SLIDER_FACTOR));
            form->addRow(text("Дод. масштаб:", "Extra scale:"),
                         linkedSlider(additionalScaleDivisorSlider, dialog, syncTimer));
        }

        const auto levelOverride = std::make_shared<bool>(
            blockSettings.value(QStringLiteral("levelOverride")).toBool(false));
        QRadioButton *globalLevels = nullptr;
        QRadioButton *individualLevels = nullptr;
        if (type != QStringLiteral("workspace_ruler")) {
            auto *modeRow = new QWidget(dialog);
            auto *modeLayout = new QHBoxLayout(modeRow);
            modeLayout->setContentsMargins(0, 0, 0, 0);
            globalLevels = new QRadioButton(text("Загально", "Global"), modeRow);
            individualLevels = new QRadioButton(text("Індивідуально", "Individual"), modeRow);
            auto *modeGroup = new QButtonGroup(modeRow);
            modeGroup->addButton(globalLevels);
            modeGroup->addButton(individualLevels);
            globalLevels->setChecked(!*levelOverride);
            individualLevels->setChecked(*levelOverride);
            modeLayout->addWidget(globalLevels);
            modeLayout->addWidget(individualLevels);
            modeLayout->addStretch(1);
            form->addRow(text("Рівні dBFS:", "dBFS levels:"), modeRow);
        }

        QSlider *minimumSlider = nullptr;
        QSlider *maximumSlider = nullptr;
        QLabel *minimumLabel = nullptr;
        QLabel *maximumLabel = nullptr;
        if (type != QStringLiteral("workspace_ruler")) {
            minimumSlider = new QSlider(Qt::Horizontal, dialog);
            maximumSlider = new QSlider(Qt::Horizontal, dialog);
            minimumLabel = new QLabel(dialog);
            maximumLabel = new QLabel(dialog);
            minimumSlider->setRange(MIN_LEVEL_SLIDER_VALUE, MAX_LEVEL_SLIDER_VALUE - 1);
            maximumSlider->setRange(MIN_LEVEL_SLIDER_VALUE + 1, MAX_LEVEL_SLIDER_VALUE);
            minimumSlider->setValue(levelToSliderValue(static_cast<float>(blockSettings.value(QStringLiteral("minimumDbfs")).toDouble(displayLevelMin))));
            maximumSlider->setValue(levelToSliderValue(static_cast<float>(blockSettings.value(QStringLiteral("maximumDbfs")).toDouble(displayLevelMax))));
            if (minimumSlider->value() >= maximumSlider->value()) {
                maximumSlider->setValue((std::min)(MAX_LEVEL_SLIDER_VALUE, minimumSlider->value() + 1));
            }
            const auto makeRow = [dialog](QSlider *slider, QLabel *label) {
                auto *container = new QWidget(dialog);
                auto *layout = new QHBoxLayout(container);
                layout->setContentsMargins(0, 0, 0, 0);
                label->setMinimumWidth(62);
                label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
                layout->addWidget(slider, 1);
                layout->addWidget(label);
                return container;
            };
            form->addRow(text("Мінімум:", "Minimum:"), makeRow(minimumSlider, minimumLabel));
            form->addRow(text("Максимум:", "Maximum:"), makeRow(maximumSlider, maximumLabel));
            minimumSlider->setEnabled(*levelOverride);
            maximumSlider->setEnabled(*levelOverride);
        }

        const auto applyWorkspaceSettings = [this, id, type, paused, workspaceDisplayMode,
                                             minimumSlider, maximumSlider,
                                             minimumLabel, maximumLabel, levelOverride,
                                             workspaceColor, workspaceGradient,
                                             workspaceGradientOpacity, workspaceSensitivity,
                                             workspaceContrast, workspaceFixedPlane,
                                             workspaceMonochrome, workspaceResolution,
                                             workspaceSurface, workspaceSmoothing, workspaceLighting,
                                             workspaceHistory, workspaceFrequencyStep,
                                             workspaceFrequencyWidth, workspaceSpectrumStep,
                                             workspaceSpectrumRows, workspaceCapture,
                                             workspaceCaptureFixed, workspaceVncInput]() {
            if (!dspFlowPanel) return;
            QJsonObject settings = dspFlowPanel->blockSettings(id);
            settings.insert(QStringLiteral("paused"), paused->isChecked());
            if (workspaceDisplayMode) {
                settings.insert(QStringLiteral("displayMode"),
                                workspaceDisplayMode->currentData().toInt());
            }
            if (workspaceSensitivity)
                settings.insert(QStringLiteral("sensitivity"), workspaceSensitivity->value());
            else if (type == QStringLiteral("workspace_spectrum"))
                settings.remove(QStringLiteral("sensitivity"));
            if (workspaceContrast)
                settings.insert(QStringLiteral("contrast"), workspaceContrast->value());
            else if (type == QStringLiteral("workspace_spectrum"))
                settings.remove(QStringLiteral("contrast"));
            if (workspaceColor)
                settings.insert(QStringLiteral("colorSpectrum"), workspaceColor->isChecked());
            if (workspaceGradient)
                settings.insert(QStringLiteral("spectrumGradientFill"),
                                workspaceGradient->isChecked());
            if (workspaceGradientOpacity)
                settings.insert(QStringLiteral("spectrumGradientOpacity"),
                                workspaceGradientOpacity->value());
            if (workspaceFixedPlane)
                settings.insert(QStringLiteral("waterfall3DFixedPlane"),
                                workspaceFixedPlane->isChecked());
            if (workspaceMonochrome)
                settings.insert(QStringLiteral("waterfall3DMonochrome"),
                                workspaceMonochrome->isChecked());
            if (workspaceResolution) {
                settings.insert(QStringLiteral("resolutionDivisor"),
                                workspaceResolution->currentData().toInt());
                settings.insert(QStringLiteral("waterfall3DSurfaceStyle"),
                                workspaceSurface->currentData().toInt());
                settings.insert(QStringLiteral("waterfall3DSmoothing"),
                                workspaceSmoothing->currentData().toInt());
                settings.insert(QStringLiteral("waterfall3DLighting"),
                                workspaceLighting->currentData().toInt());
                settings.insert(QStringLiteral("historyRows"), workspaceHistory->value());
                settings.insert(QStringLiteral("waterfall3DSliceScrollStep"),
                                workspaceFrequencyStep->value());
                settings.insert(QStringLiteral("waterfall3DSliceWidth"),
                                workspaceFrequencyWidth->value());
                settings.insert(QStringLiteral("waterfall3DSpectrumSliceScrollStep"),
                                workspaceSpectrumStep->value());
                settings.insert(QStringLiteral("waterfall3DSpectrumSliceRows"),
                                workspaceSpectrumRows->value());
                settings.insert(QStringLiteral("waterfall3DSpectrumSliceCapture"),
                                workspaceCapture->isChecked());
                settings.insert(QStringLiteral("waterfall3DSpectrumSliceCaptureFixed"),
                                workspaceCaptureFixed->isChecked());
                settings.insert(QStringLiteral("waterfall3DVncSliceInput"),
                                workspaceVncInput->isChecked());
            }
            if (minimumSlider && maximumSlider) {
                if (minimumSlider->value() >= maximumSlider->value()) {
                    const QSignalBlocker blocker(maximumSlider);
                    maximumSlider->setValue((std::min)(maximumSlider->maximum(),
                                                       minimumSlider->value() + 1));
                }
                settings.insert(QStringLiteral("minimumDbfs"), sliderValueToLevel(minimumSlider->value()));
                settings.insert(QStringLiteral("maximumDbfs"), sliderValueToLevel(maximumSlider->value()));
                settings.insert(QStringLiteral("levelOverride"), *levelOverride);
                minimumLabel->setText(QStringLiteral("%1 dBFS").arg(sliderValueToLevel(minimumSlider->value()), 0, 'f', 1));
                maximumLabel->setText(QStringLiteral("%1 dBFS").arg(sliderValueToLevel(maximumSlider->value()), 0, 'f', 1));
            }
            dspFlowPanel->setBlockSettings(id, settings);
        };
        connect(paused, &QCheckBox::toggled, dialog,
                [applyWorkspaceSettings](bool) { applyWorkspaceSettings(); });
        if (workspaceDisplayMode) {
            connect(workspaceDisplayMode, QOverload<int>::of(&QComboBox::currentIndexChanged), dialog,
                    [applyWorkspaceSettings](int) { applyWorkspaceSettings(); });
        }
        if (workspaceSensitivity)
            connect(workspaceSensitivity, &QSlider::valueChanged, dialog,
                    [applyWorkspaceSettings](int) { applyWorkspaceSettings(); });
        if (workspaceContrast)
            connect(workspaceContrast, &QSlider::valueChanged, dialog,
                    [applyWorkspaceSettings](int) { applyWorkspaceSettings(); });
        if (workspaceColor)
            connect(workspaceColor, &QCheckBox::toggled, dialog,
                    [applyWorkspaceSettings](bool) { applyWorkspaceSettings(); });
        if (workspaceGradient)
            connect(workspaceGradient, &QCheckBox::toggled, dialog,
                    [applyWorkspaceSettings](bool) { applyWorkspaceSettings(); });
        if (workspaceGradientOpacity)
            connect(workspaceGradientOpacity, &QSlider::valueChanged, dialog,
                    [applyWorkspaceSettings](int) { applyWorkspaceSettings(); });
        if (workspaceFixedPlane)
            connect(workspaceFixedPlane, &QCheckBox::toggled, dialog,
                    [applyWorkspaceSettings](bool) { applyWorkspaceSettings(); });
        if (workspaceMonochrome)
            connect(workspaceMonochrome, &QCheckBox::toggled, dialog,
                    [applyWorkspaceSettings](bool) { applyWorkspaceSettings(); });
        for (QComboBox *combo : {workspaceResolution, workspaceSurface,
                                 workspaceSmoothing, workspaceLighting}) {
            if (combo) connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), dialog,
                               [applyWorkspaceSettings](int) { applyWorkspaceSettings(); });
        }
        for (QSlider *slider : {workspaceHistory, workspaceFrequencyStep,
                                workspaceFrequencyWidth, workspaceSpectrumStep,
                                workspaceSpectrumRows}) {
            if (slider) connect(slider, &QSlider::valueChanged, dialog,
                                [applyWorkspaceSettings](int) { applyWorkspaceSettings(); });
        }
        if (workspaceCapture) {
            connect(workspaceCapture, &QCheckBox::toggled, dialog,
                    [applyWorkspaceSettings, workspaceCaptureFixed](bool checked) {
                        workspaceCaptureFixed->setEnabled(checked);
                        applyWorkspaceSettings();
                    });
            connect(workspaceCaptureFixed, &QCheckBox::toggled, dialog,
                    [applyWorkspaceSettings](bool) { applyWorkspaceSettings(); });
            connect(workspaceVncInput, &QCheckBox::toggled, dialog,
                    [applyWorkspaceSettings](bool) { applyWorkspaceSettings(); });
        }
        if (minimumSlider && maximumSlider) {
            connect(minimumSlider, &QSlider::valueChanged, dialog,
                    [applyWorkspaceSettings](int) { applyWorkspaceSettings(); });
            connect(maximumSlider, &QSlider::valueChanged, dialog,
                    [applyWorkspaceSettings](int) { applyWorkspaceSettings(); });
            connect(globalLevels, &QRadioButton::toggled, dialog,
                    [applyWorkspaceSettings, levelOverride, minimumSlider, maximumSlider](bool checked) {
                        if (!checked) return;
                        *levelOverride = false;
                        minimumSlider->setEnabled(false);
                        maximumSlider->setEnabled(false);
                        applyWorkspaceSettings();
                    });
            connect(individualLevels, &QRadioButton::toggled, dialog,
                    [applyWorkspaceSettings, levelOverride, minimumSlider, maximumSlider](bool checked) {
                        if (!checked) return;
                        *levelOverride = true;
                        minimumSlider->setEnabled(true);
                        maximumSlider->setEnabled(true);
                        applyWorkspaceSettings();
                    });
        }
        applyWorkspaceSettings();
    } else if (type == QStringLiteral("vfo_spectrum") || type == QStringLiteral("vfo_waterfall")) {
        title = type == QStringLiteral("vfo_spectrum")
                    ? text("Спектр VFO", "VFO spectrum")
                    : text("Водоспад VFO", "VFO waterfall");
        auto *vfoSelector = new QComboBox(dialog);
        const auto rebuildSelector = [this, vfoSelector, id, ukrainian]() {
            const int selectedIndex = dspFlowPanel ? dspFlowPanel->vfoIndexForBlock(id) : -1;
            const QSignalBlocker blocker(vfoSelector);
            vfoSelector->clear();
            const QJsonDocument document = QJsonDocument::fromJson(
                multiVfoWidget ? multiVfoWidget->configurationJson().toUtf8() : QByteArray());
            const QJsonArray channels = document.object().value(QStringLiteral("channels")).toArray();
            for (int channelIndex = 0; channelIndex < channels.size(); ++channelIndex) {
                const QJsonObject channel = channels.at(channelIndex).toObject();
                const QString name = channel.value(QStringLiteral("name"))
                                         .toString(QStringLiteral("VFO %1").arg(channelIndex + 1));
                const double frequencyMHz =
                    channel.value(QStringLiteral("frequencyHz")).toDouble() / 1.0e6;
                vfoSelector->addItem(
                    QStringLiteral("%1 - %2 - %3 MHz")
                        .arg(channelIndex + 1)
                        .arg(name)
                        .arg(frequencyMHz, 0, 'f', 6),
                    channelIndex);
            }
            if (vfoSelector->count() == 0) {
                vfoSelector->addItem(
                    ukrainian ? QStringLiteral("Немає каналів Multi-VFO")
                              : QStringLiteral("No Multi-VFO channels"),
                    -1);
                vfoSelector->setEnabled(false);
                return;
            }
            vfoSelector->setEnabled(true);
            const int comboIndex = vfoSelector->findData(selectedIndex);
            vfoSelector->setCurrentIndex(comboIndex >= 0 ? comboIndex : 0);
        };
        rebuildSelector();
        form->addRow(text("Канал Multi-VFO:", "Multi-VFO channel:"), vfoSelector);
        connect(vfoSelector, QOverload<int>::of(&QComboBox::currentIndexChanged), dialog,
                [this, vfoSelector, id](int) {
                    if (!dspFlowPanel) return;
                    const int channelIndex = vfoSelector->currentData().toInt();
                    if (channelIndex >= 0) dspFlowPanel->assignVfoIndexToBlock(id, channelIndex);
                });
        if (multiVfoWidget) {
            connect(multiVfoWidget, &MultiVfoWidget::configurationChanged, dialog,
                    [rebuildSelector](const QString &) { rebuildSelector(); });
        }

        const QJsonObject blockSettings =
            dspFlowPanel ? dspFlowPanel->blockSettings(id) : QJsonObject();
        auto *minimumSlider = new QSlider(Qt::Horizontal, dialog);
        auto *maximumSlider = new QSlider(Qt::Horizontal, dialog);
        auto *minimumLabel = new QLabel(dialog);
        auto *maximumLabel = new QLabel(dialog);
        minimumSlider->setRange(MIN_LEVEL_SLIDER_VALUE, MAX_LEVEL_SLIDER_VALUE - 1);
        maximumSlider->setRange(MIN_LEVEL_SLIDER_VALUE + 1, MAX_LEVEL_SLIDER_VALUE);
        minimumSlider->setValue(levelToSliderValue(static_cast<float>(blockSettings.value(QStringLiteral("minimumDbfs")).toDouble(displayLevelMin))));
        maximumSlider->setValue(levelToSliderValue(static_cast<float>(blockSettings.value(QStringLiteral("maximumDbfs")).toDouble(displayLevelMax))));
        if (minimumSlider->value() >= maximumSlider->value()) {
            minimumSlider->setValue((std::max)(minimumSlider->minimum(),
                                               maximumSlider->value() - 1));
        }
        const auto sliderRow = [dialog](QSlider *slider, QLabel *label) {
            auto *container = new QWidget(dialog);
            auto *layout = new QHBoxLayout(container);
            layout->setContentsMargins(0, 0, 0, 0);
            layout->setSpacing(6);
            label->setMinimumWidth(62);
            label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            layout->addWidget(slider, 1);
            layout->addWidget(label);
            return container;
        };
        const auto updateLabels = [minimumSlider, maximumSlider, minimumLabel, maximumLabel]() {
            minimumLabel->setText(QStringLiteral("%1 dBFS").arg(sliderValueToLevel(minimumSlider->value()), 0, 'f', 1));
            maximumLabel->setText(QStringLiteral("%1 dBFS").arg(sliderValueToLevel(maximumSlider->value()), 0, 'f', 1));
        };
        const auto applyRange = [this, id, minimumSlider, maximumSlider]() {
            if (!dspFlowPanel) return;
            QJsonObject settings = dspFlowPanel->blockSettings(id);
            settings.insert(QStringLiteral("minimumDbfs"), sliderValueToLevel(minimumSlider->value()));
            settings.insert(QStringLiteral("maximumDbfs"), sliderValueToLevel(maximumSlider->value()));
            dspFlowPanel->setBlockSettings(id, settings);
        };
        form->addRow(text("Мінімум:", "Minimum:"), sliderRow(minimumSlider, minimumLabel));
        form->addRow(text("Максимум:", "Maximum:"), sliderRow(maximumSlider, maximumLabel));
        connect(minimumSlider, &QSlider::valueChanged, dialog,
                [minimumSlider, maximumSlider, updateLabels, applyRange](int value) {
                    if (value >= maximumSlider->value()) {
                        maximumSlider->setValue((std::min)(maximumSlider->maximum(), value + 1));
                    }
                    updateLabels();
                    applyRange();
                });
        connect(maximumSlider, &QSlider::valueChanged, dialog,
                [minimumSlider, maximumSlider, updateLabels, applyRange](int value) {
                    if (value <= minimumSlider->value()) {
                        minimumSlider->setValue((std::max)(minimumSlider->minimum(), value - 1));
                    }
                    updateLabels();
                    applyRange();
                });
        updateLabels();
        dialog->setMinimumWidth(390);
    } else if (type == QStringLiteral("multi_vfo_channelizer") ||
               type == QStringLiteral("vfo_channel")) {
        title = type == QStringLiteral("multi_vfo_channelizer")
                    ? text("Multi-VFO каналайзер", "Multi-VFO channelizer")
                    : text("Канал VFO", "VFO channel");
        QComboBox *vfoSelector = nullptr;
        if (type != QStringLiteral("multi_vfo_channelizer")) {
            vfoSelector = new QComboBox(dialog);
            const auto rebuildSelector = [this, vfoSelector, id, ukrainian]() {
                const int selectedIndex = dspFlowPanel ? dspFlowPanel->vfoIndexForBlock(id) : -1;
                const QSignalBlocker blocker(vfoSelector);
                vfoSelector->clear();
                const QJsonDocument document = QJsonDocument::fromJson(
                    multiVfoWidget ? multiVfoWidget->configurationJson().toUtf8() : QByteArray());
                const QJsonArray channels = document.object().value(QStringLiteral("channels")).toArray();
                for (int channelIndex = 0; channelIndex < channels.size(); ++channelIndex) {
                    const QJsonObject channel = channels.at(channelIndex).toObject();
                    const QString name = channel.value(QStringLiteral("name"))
                                             .toString(QStringLiteral("VFO %1").arg(channelIndex + 1));
                    const double frequencyMHz =
                        channel.value(QStringLiteral("frequencyHz")).toDouble() / 1.0e6;
                    vfoSelector->addItem(
                        QStringLiteral("%1 - %2 - %3 MHz")
                            .arg(channelIndex + 1)
                            .arg(name)
                            .arg(frequencyMHz, 0, 'f', 6),
                        channelIndex);
                }
                if (vfoSelector->count() == 0) {
                    vfoSelector->addItem(
                        ukrainian ? QStringLiteral("Немає каналів Multi-VFO")
                                  : QStringLiteral("No Multi-VFO channels"),
                        -1);
                    vfoSelector->setEnabled(false);
                    return;
                }
                vfoSelector->setEnabled(true);
                const int comboIndex = vfoSelector->findData(selectedIndex);
                vfoSelector->setCurrentIndex(comboIndex >= 0 ? comboIndex : 0);
            };
            rebuildSelector();
            form->addRow(text("Канал Multi-VFO:", "Multi-VFO channel:"), vfoSelector);
            connect(vfoSelector, QOverload<int>::of(&QComboBox::currentIndexChanged), dialog,
                    [this, vfoSelector, id](int) {
                        if (!dspFlowPanel) return;
                        const int channelIndex = vfoSelector->currentData().toInt();
                        if (channelIndex >= 0) dspFlowPanel->assignVfoIndexToBlock(id, channelIndex);
                    });
            if (multiVfoWidget) {
                connect(multiVfoWidget, &MultiVfoWidget::configurationChanged, dialog,
                        [rebuildSelector](const QString &) { rebuildSelector(); });
            }
        }
        auto *multiVfo = new MultiVfoWidget(dialog);
        multiVfo->setLanguage(ukrainian);
        multiVfo->setReceiverContext(pendingSettings.centerFrequency,
                                     pendingSettings.sampleRate,
                                     pendingSettings.listeningFrequency,
                                     pendingSettings.bandwidth,
                                     pendingSettings.modulationType);
        if (multiVfoWidget) multiVfo->setConfigurationJson(multiVfoWidget->configurationJson());
        root->addWidget(multiVfo, 1);
        dialog->resize(type == QStringLiteral("vfo_channel") ? 760 : 940, 360);
        const auto syncing = std::make_shared<bool>(false);
        connect(multiVfo, &MultiVfoWidget::configurationChanged, dialog,
                [this, multiVfo, syncing](const QString &json) {
                    if (*syncing || !multiVfoWidget) return;
                    *syncing = true;
                    multiVfoWidget->setConfigurationJson(json);
                    savePersistentSettings();
                    *syncing = false;
                });
        if (multiVfoWidget) {
            connect(multiVfoWidget, &MultiVfoWidget::configurationChanged, multiVfo,
                    [multiVfo, syncing](const QString &json) {
                        if (*syncing || multiVfo->configurationJson() == json) return;
                        *syncing = true;
                        multiVfo->setConfigurationJson(json);
                        *syncing = false;
                    });
        }
        connect(multiVfo, &MultiVfoWidget::monitorRequested, dialog,
                [this](double frequencyHz, double bandwidthHz, int modulationType) {
                    if (pendingSettings.modulationType != modulationType) onModulationChanged(modulationType);
                    pendingSettings.bandwidth = (std::clamp)(bandwidthHz, 10.0, 20000000.0);
                    if (bandwidthControl) bandwidthControl->setValueHz(pendingSettings.bandwidth);
                    updateTuningFromScale(frequencyHz, pendingSettings.centerFrequency);
                });
        dialog->setMinimumSize(700, 320);
    } else if (type == QStringLiteral("channel_filter")) {
        title = text("Канальний фільтр", "Channel filter");
        form->addRow(text("Смуга:", "Bandwidth:"),
                     addFrequencyControl(bandwidthControl,
                                         [this]() { onBandwidthChanged(); }));
    } else if (type == QStringLiteral("demodulator")) {
        title = text("Демодулятор", "Demodulator");
        auto *modulation = new QComboBox(dialog);
        const QStringList names = {QStringLiteral("AM"), QStringLiteral("NFM"), QStringLiteral("SAM"),
                                   QStringLiteral("USB"), QStringLiteral("LSB"), QStringLiteral("DSB"),
                                   QStringLiteral("CW"), QStringLiteral("WFM")};
        const QVector<int> ids = {MOD_AM, MOD_NFM, MOD_SAM, MOD_USB, MOD_LSB, MOD_DSB, MOD_CW, MOD_WFM};
        for (int i = 0; i < ids.size(); ++i) modulation->addItem(names.at(i), ids.at(i));
        modulation->setCurrentIndex((std::max)(0, modulation->findData(pendingSettings.modulationType)));
        connect(modulation, QOverload<int>::of(&QComboBox::currentIndexChanged), dialog,
                [this, modulation](int index) {
                    const int mode = modulation->itemData(index).toInt();
                    if (modulationButtonGroup) {
                        if (QAbstractButton *button = modulationButtonGroup->button(mode)) {
                            const QSignalBlocker blocker(modulationButtonGroup);
                            button->setChecked(true);
                        }
                    }
                    onModulationChanged(mode);
                });
        connect(syncTimer, &QTimer::timeout, modulation, [this, modulation]() {
            if (focusInside(modulation)) return;
            const int index = modulation->findData(pendingSettings.modulationType);
            if (index >= 0 && index != modulation->currentIndex()) {
                const QSignalBlocker blocker(modulation); modulation->setCurrentIndex(index);
            }
        });
        form->addRow(text("Модуляція:", "Modulation:"), modulation);
        form->addRow(text("Смуга:", "Bandwidth:"),
                     addFrequencyControl(bandwidthControl,
                                         [this]() { onBandwidthChanged(); }));
    } else if (type == QStringLiteral("audio_filter")) {
        title = text("Ланцюг аудіофільтрів", "Audio filter chain");
        auto *chain = new AudioFilterChainWidget(dialog);
        chain->setLanguage(ukrainian);
        chain->setConfigurationJson(audioFilterChainWidget->configurationJson());
        root->addWidget(chain, 1);
        dialog->resize(620, 390);
        const auto syncing = std::make_shared<bool>(false);
        const auto applyChain = [this, syncing](const QString &json) {
            if (*syncing || !audioFilterChainWidget) return;
            *syncing = true;
            audioFilterChainWidget->setConfigurationJson(json);
            pendingSettings.audioFilterChainJson = json;
            if (audioProcessor) audioProcessor->configure(audioProcessorSettings());
            savePersistentSettings();
            if (isNetworkClientMode()) scheduleRemoteSettingsCommand();
            *syncing = false;
        };
        connect(chain, &AudioFilterChainWidget::configurationChanged, dialog, applyChain);
        auto *applyOrder = new QPushButton(text("Застосувати порядок зі зв'язків DSP", "Apply order from DSP links"), dialog);
        applyOrder->setObjectName(QStringLiteral("dspApplyAudioFilterOrder"));
        auto *orderStatus = new QLabel(dialog);
        orderStatus->setWordWrap(true);
        root->addWidget(applyOrder);
        root->addWidget(orderStatus);
        connect(applyOrder, &QPushButton::clicked, dialog,
                [this, chain, orderStatus, applyChain, text]() {
                    if (!dspFlowPanel || !audioFilterChainWidget) return;
                    QString error;
                    const auto graph = QJsonDocument::fromJson(dspFlowPanel->configurationJson().toUtf8()).object();
                    const QStringList order = compileDspAudioFilterOrder(graph, &error);
                    if (!error.isEmpty() || order.isEmpty()) {
                        orderStatus->setText(error == QStringLiteral("Ambiguous audio filter chain: branches or multiple inputs")
                            ? text("Порядок не змінено: ланцюг має розгалуження або кілька входів.",
                                   "Order unchanged: the chain has branches or multiple inputs.")
                            : text("Порядок не змінено: потрібен один повний ланцюг Демодулятор → фільтри → Аудіовихід без циклів.",
                                   "Order unchanged: connect one complete Demodulator -> filters -> Audio output chain without cycles."));
                        return;
                    }
                    bool valid = false;
                    auto stages = decodeAudioFilterChain(audioFilterChainWidget->configurationJson(), &valid);
                    QHash<QString, AudioFilterStage> byId;
                    for (const auto &stage : stages) {
                        if (byId.contains(stage.id)) valid = false;
                        byId.insert(stage.id, stage);
                    }
                    QSet<QString> selected;
                    for (const QString &stageId : order) {
                        if (!byId.contains(stageId) || selected.contains(stageId)) valid = false;
                        selected.insert(stageId);
                    }
                    if (!valid) {
                        orderStatus->setText(text("Порядок не змінено: фільтри на дошці не відповідають поточному аудіоланцюгу.",
                                                  "Order unchanged: board filters do not match the current audio chain."));
                        return;
                    }
                    // Keep unrelated stages in their existing slots and preserve every stage's parameters.
                    int next = 0;
                    for (auto &stage : stages)
                        if (selected.contains(stage.id)) stage = byId.value(order.at(next++));
                    const QString json = encodeAudioFilterChain(stages);
                    if (json != audioFilterChainWidget->configurationJson()) {
                        chain->setConfigurationJson(json);
                        applyChain(json);
                    }
                    orderStatus->setText(text("Порядок застосовано. Параметри та стан увімкнення фільтрів збережено.",
                                              "Order applied. Filter parameters and enabled states are preserved."));
                });
        connect(audioFilterChainWidget, &AudioFilterChainWidget::configurationChanged, chain,
                [chain, syncing](const QString &json) {
                    if (*syncing || chain->configurationJson() == json) return;
                    *syncing = true;
                    chain->setConfigurationJson(json);
                    *syncing = false;
                });
        connect(syncTimer, &QTimer::timeout, chain, [this, chain, syncing]() {
            if (*syncing || focusInside(chain)) return;
            const QString json = audioFilterChainWidget->configurationJson();
            if (chain->configurationJson() != json) {
                *syncing = true;
                chain->setConfigurationJson(json);
                *syncing = false;
            }
        });
        dialog->setMinimumSize(620, 390);
    } else if (type == QStringLiteral("spectrum_display") ||
               type == QStringLiteral("waterfall_2d") ||
               type == QStringLiteral("waterfall_3d")) {
        const bool spectrumProfile = type == QStringLiteral("spectrum_display");
        const bool waterfall3DProfile = type == QStringLiteral("waterfall_3d");
        title = spectrumProfile
                    ? text("Налаштування спектра", "Spectrum settings")
                    : (waterfall3DProfile
                           ? text("Налаштування 3D-водоспаду", "3D waterfall settings")
                           : text("Налаштування 2D-водоспаду", "2D waterfall settings"));
        const QJsonObject stored = dspFlowPanel ? dspFlowPanel->blockSettings(id) : QJsonObject();
        auto *targetSelector = new QComboBox(dialog);
        targetSelector->addItem(text("Не призначено", "Not assigned"), QString());
        if (dspFlowPanel) {
            const QJsonArray targets = dspFlowPanel->workspaceDisplayTargets(id);
            for (const QJsonValue &value : targets) {
                const QJsonObject target = value.toObject();
                const QString targetId = target.value(QStringLiteral("id")).toString();
                const QString targetTitle = target.value(QStringLiteral("title")).toString();
                targetSelector->addItem(QStringLiteral("%1 [%2]").arg(targetTitle, targetId.left(8)),
                                        targetId);
                if (target.value(QStringLiteral("bound")).toBool())
                    targetSelector->setCurrentIndex(targetSelector->count() - 1);
            }
        }
        form->addRow(text("Керувати віджетом:", "Control widget:"), targetSelector);
        connect(targetSelector, QOverload<int>::of(&QComboBox::currentIndexChanged), dialog,
                [this, id, targetSelector](int) {
                    if (dspFlowPanel)
                        dspFlowPanel->bindWorkspaceSettingsBlock(id,
                            targetSelector->currentData().toString());
                });

        const auto levelOverride = std::make_shared<bool>(
            stored.value(QStringLiteral("levelOverride")).toBool(false));
        auto *levelModeRow = new QWidget(dialog);
        auto *levelModeLayout = new QHBoxLayout(levelModeRow);
        levelModeLayout->setContentsMargins(0, 0, 0, 0);
        auto *globalLevels = new QRadioButton(text("Загально", "Global"), levelModeRow);
        auto *individualLevels = new QRadioButton(text("Індивідуально", "Individual"), levelModeRow);
        auto *levelModeGroup = new QButtonGroup(levelModeRow);
        levelModeGroup->addButton(globalLevels);
        levelModeGroup->addButton(individualLevels);
        globalLevels->setChecked(!*levelOverride);
        individualLevels->setChecked(*levelOverride);
        levelModeLayout->addWidget(globalLevels);
        levelModeLayout->addWidget(individualLevels);
        levelModeLayout->addStretch(1);
        form->addRow(text("Рівні dBFS:", "dBFS levels:"), levelModeRow);

        QSlider *minimum = nullptr;
        QSlider *maximum = nullptr;
        QSlider *profileSensitivity = nullptr;
        QSlider *profileContrast = nullptr;
        form->addRow(text("Мінімум:", "Minimum:"),
                     valueSlider(&minimum, dialog, -2000, 190,
                                 qRound(stored.value(QStringLiteral("minimumDbfs")).toDouble(displayLevelMin) * 10.0),
                                 QStringLiteral(" dBFS"), 10.0));
        form->addRow(text("Максимум:", "Maximum:"),
                     valueSlider(&maximum, dialog, -1990, 200,
                                 qRound(stored.value(QStringLiteral("maximumDbfs")).toDouble(displayLevelMax) * 10.0),
                                 QStringLiteral(" dBFS"), 10.0));
        minimum->setEnabled(*levelOverride);
        maximum->setEnabled(*levelOverride);
        if (!spectrumProfile) {
            form->addRow(text("Чутливість:", "Sensitivity:"),
                         valueSlider(&profileSensitivity, dialog, 1, 30,
                                     stored.value(QStringLiteral("sensitivity")).toInt(sensitivity)));
            form->addRow(text("Контраст:", "Contrast:"),
                         valueSlider(&profileContrast, dialog, 1, 20,
                                     stored.value(QStringLiteral("contrast")).toInt(contrast)));
        }

        QComboBox *profileMode = nullptr;
        if (!spectrumProfile) {
            profileMode = new QComboBox(dialog);
            profileMode->addItem(text("2D водоспад", "2D waterfall"), 0);
            profileMode->addItem(text("3D водоспад", "3D waterfall"), 1);
            profileMode->addItem(text("3D + міні", "3D + mini"), 2);
            const int defaultMode = waterfall3DProfile ? 1 : 0;
            const int modeIndex = profileMode->findData(
                stored.value(QStringLiteral("displayMode")).toInt(defaultMode));
            profileMode->setCurrentIndex(modeIndex >= 0 ? modeIndex : defaultMode);
            profileMode->setEnabled(waterfall3DProfile);
            form->addRow(text("Режим:", "Mode:"), profileMode);
        }

        auto *profileColor = new QCheckBox(text("Кольоровий спектр", "Colored spectrum"), dialog);
        auto *profileGradient = new QCheckBox(text("Градієнт спектра", "Spectrum gradient"), dialog);
        QSlider *profileOpacity = nullptr;
        profileColor->setChecked(stored.value(QStringLiteral("colorSpectrum")).toBool(
            colorCheckbox ? colorCheckbox->isChecked() : true));
        profileGradient->setChecked(stored.value(QStringLiteral("spectrumGradientFill")).toBool(
            alternativeSpectrumGradientCheckbox
                ? alternativeSpectrumGradientCheckbox->isChecked()
                : false));
        const bool showSpectrumProfileControls = spectrumProfile || waterfall3DProfile;
        profileColor->setVisible(showSpectrumProfileControls);
        profileGradient->setVisible(showSpectrumProfileControls);
        if (showSpectrumProfileControls) {
            form->addRow(QString(), profileColor);
            form->addRow(QString(), profileGradient);
            form->addRow(text("Прозорість градієнта:", "Gradient opacity:"),
                         valueSlider(&profileOpacity, dialog, 0, 100,
                                     stored.value(QStringLiteral("spectrumGradientOpacity")).toInt(
                                         alternativeSpectrumGradientOpacity),
                                     QStringLiteral("%")));
        }

        auto *profileFixedPlane = new QCheckBox(
            text("Зафіксувати площину", "Fix plane"), dialog);
        auto *profileMonochrome = new QCheckBox(
            text("Однотонний синій 3D", "Monochrome blue 3D"), dialog);
        profileFixedPlane->setChecked(
            stored.value(QStringLiteral("waterfall3DFixedPlane")).toBool(waterfall3DFixedPlane));
        profileMonochrome->setChecked(
            stored.value(QStringLiteral("waterfall3DMonochrome")).toBool(waterfall3DMonochrome));
        profileFixedPlane->setVisible(waterfall3DProfile);
        profileMonochrome->setVisible(waterfall3DProfile);
        QComboBox *profileResolution = nullptr;
        QComboBox *profileSurface = nullptr;
        QComboBox *profileSmoothing = nullptr;
        QComboBox *profileLighting = nullptr;
        QSlider *profileHistory = nullptr;
        QSlider *profileFrequencyStep = nullptr;
        QSlider *profileFrequencyWidth = nullptr;
        QSlider *profileSpectrumStep = nullptr;
        QSlider *profileSpectrumRows = nullptr;
        QCheckBox *profileCapture = nullptr;
        QCheckBox *profileCaptureFixed = nullptr;
        QCheckBox *profileVncInput = nullptr;
        if (waterfall3DProfile) {
            form->addRow(QString(), profileFixedPlane);
            form->addRow(QString(), profileMonochrome);
            profileResolution = new QComboBox(dialog);
            for (int divisor : {1, 2, 4, 8, 16, 32, 64})
                profileResolution->addItem(QStringLiteral("1/%1").arg(divisor), divisor);
            const int resolutionIndex = profileResolution->findData(
                stored.value(QStringLiteral("resolutionDivisor")).toInt(waterfall3DResolutionDivisor));
            profileResolution->setCurrentIndex(resolutionIndex >= 0 ? resolutionIndex : 0);
            form->addRow(text("Роздільність:", "Resolution:"), profileResolution);

            profileSurface = new QComboBox(dialog);
            profileSurface->addItem(text("Оригінал / голки", "Original / needles"), 0);
            profileSurface->addItem(text("Суцільна поверхня", "Solid surface"), 1);
            profileSurface->setCurrentIndex(profileSurface->findData(
                stored.value(QStringLiteral("waterfall3DSurfaceStyle")).toInt(waterfall3DSurfaceStyle)));
            form->addRow(text("Поверхня:", "Surface:"), profileSurface);
            profileSmoothing = new QComboBox(dialog);
            profileSmoothing->addItem(text("Вимкнено", "Off"), 0);
            profileSmoothing->addItem(text("М'яке", "Soft"), 1);
            profileSmoothing->addItem(text("Сильне", "Strong"), 2);
            profileSmoothing->setCurrentIndex(profileSmoothing->findData(
                stored.value(QStringLiteral("waterfall3DSmoothing")).toInt(waterfall3DSmoothing)));
            form->addRow(text("Згладжування:", "Smoothing:"), profileSmoothing);
            profileLighting = new QComboBox(dialog);
            profileLighting->addItem(text("Вимкнено", "Off"), 0);
            profileLighting->addItem(text("М'які тіні", "Soft shadows"), 1);
            profileLighting->addItem(text("Сильні тіні", "Strong shadows"), 2);
            profileLighting->setCurrentIndex(profileLighting->findData(
                stored.value(QStringLiteral("waterfall3DLighting")).toInt(waterfall3DLighting)));
            form->addRow(text("Освітлення:", "Lighting:"), profileLighting);

            form->addRow(text("Пам'ять:", "Memory:"),
                         valueSlider(&profileHistory, dialog, 16, 2048,
                                     stored.value(QStringLiteral("historyRows")).toInt(waterfall3DHistoryRows),
                                     text(" рядків", " rows")));
            form->addRow(text("Крок частотного зрізу:", "Frequency slice step:"),
                         valueSlider(&profileFrequencyStep, dialog, 1, 256,
                                     stored.value(QStringLiteral("waterfall3DSliceScrollStep")).toInt(
                                         waterfall3DSliceScrollStep), text(" тчк", " pt")));
            form->addRow(text("Ширина частотного зрізу:", "Frequency slice width:"),
                         valueSlider(&profileFrequencyWidth, dialog, 1, 4096,
                                     stored.value(QStringLiteral("waterfall3DSliceWidth")).toInt(
                                         waterfall3DSliceWidth), text(" тчк", " pt")));
            form->addRow(text("Крок часового зрізу:", "Time slice step:"),
                         valueSlider(&profileSpectrumStep, dialog, 1, 2048,
                                     stored.value(QStringLiteral("waterfall3DSpectrumSliceScrollStep")).toInt(
                                         waterfall3DSpectrumSliceScrollStep), text(" ряд.", " rows")));
            form->addRow(text("Ширина часового зрізу:", "Time slice rows:"),
                         valueSlider(&profileSpectrumRows, dialog, 1, 2048,
                                     stored.value(QStringLiteral("waterfall3DSpectrumSliceRows")).toInt(
                                         waterfall3DSpectrumSliceRows), text(" ряд.", " rows")));
            profileCapture = new QCheckBox(text("Захват", "Capture"), dialog);
            profileCaptureFixed = new QCheckBox(text("Зафіксувати захват", "Freeze capture"), dialog);
            profileVncInput = new QCheckBox(
                text("Зрізи без Alt/Shift (VNC)", "Slices without Alt/Shift (VNC)"), dialog);
            profileCapture->setChecked(stored.value(
                QStringLiteral("waterfall3DSpectrumSliceCapture")).toBool(waterfall3DSpectrumSliceCapture));
            profileCaptureFixed->setChecked(stored.value(
                QStringLiteral("waterfall3DSpectrumSliceCaptureFixed")).toBool(
                    waterfall3DSpectrumSliceCaptureFixed));
            profileCaptureFixed->setEnabled(profileCapture->isChecked());
            profileVncInput->setChecked(stored.value(
                QStringLiteral("waterfall3DVncSliceInput")).toBool(waterfall3DVncSliceInput));
            auto *captureRow = new QWidget(dialog);
            auto *captureLayout = new QHBoxLayout(captureRow);
            captureLayout->setContentsMargins(0, 0, 0, 0);
            captureLayout->addWidget(profileCapture);
            captureLayout->addWidget(profileCaptureFixed);
            captureLayout->addStretch(1);
            form->addRow(QString(), captureRow);
            form->addRow(QString(), profileVncInput);
        }

        auto *bindingHint = new QLabel(
            text("Виберіть віджет у списку вище. Стрілка з цього блока у віджет лишається рівнозначним способом прив'язки.",
                 "Choose a widget above. Connecting this block to a view with an arrow remains an equivalent binding method."),
            dialog);
        bindingHint->setWordWrap(true);
        form->addRow(QString(), bindingHint);

        const auto applyProfile = [this, id, spectrumProfile, waterfall3DProfile,
                                   minimum, maximum, profileSensitivity, profileContrast,
                                   profileMode, profileColor, profileGradient, profileOpacity,
                                   profileFixedPlane, profileMonochrome, profileResolution,
                                   profileSurface, profileSmoothing, profileLighting, profileHistory,
                                   profileFrequencyStep, profileFrequencyWidth, profileSpectrumStep,
                                   profileSpectrumRows, profileCapture, profileCaptureFixed,
                                   profileVncInput, levelOverride]() {
            if (!dspFlowPanel) return;
            QJsonObject settings = dspFlowPanel->blockSettings(id);
            double low = minimum->value() / 10.0;
            double high = maximum->value() / 10.0;
            if (low >= high) high = low + 1.0;
            settings.insert(QStringLiteral("minimumDbfs"), low);
            settings.insert(QStringLiteral("maximumDbfs"), high);
            settings.insert(QStringLiteral("levelOverride"), *levelOverride);
            if (profileSensitivity)
                settings.insert(QStringLiteral("sensitivity"), profileSensitivity->value());
            else
                settings.remove(QStringLiteral("sensitivity"));
            if (profileContrast)
                settings.insert(QStringLiteral("contrast"), profileContrast->value());
            else
                settings.remove(QStringLiteral("contrast"));
            if (!spectrumProfile)
                settings.insert(QStringLiteral("displayMode"),
                                waterfall3DProfile ? profileMode->currentData().toInt() : 0);
            if (spectrumProfile || waterfall3DProfile) {
                settings.insert(QStringLiteral("colorSpectrum"), profileColor->isChecked());
                settings.insert(QStringLiteral("spectrumGradientFill"), profileGradient->isChecked());
                settings.insert(QStringLiteral("spectrumGradientOpacity"), profileOpacity->value());
            }
            if (waterfall3DProfile) {
                settings.insert(QStringLiteral("waterfall3DFixedPlane"),
                                profileFixedPlane->isChecked());
                settings.insert(QStringLiteral("waterfall3DMonochrome"),
                                profileMonochrome->isChecked());
                settings.insert(QStringLiteral("resolutionDivisor"),
                                profileResolution->currentData().toInt());
                settings.insert(QStringLiteral("waterfall3DSurfaceStyle"),
                                profileSurface->currentData().toInt());
                settings.insert(QStringLiteral("waterfall3DSmoothing"),
                                profileSmoothing->currentData().toInt());
                settings.insert(QStringLiteral("waterfall3DLighting"),
                                profileLighting->currentData().toInt());
                settings.insert(QStringLiteral("historyRows"), profileHistory->value());
                settings.insert(QStringLiteral("waterfall3DSliceScrollStep"),
                                profileFrequencyStep->value());
                settings.insert(QStringLiteral("waterfall3DSliceWidth"),
                                profileFrequencyWidth->value());
                settings.insert(QStringLiteral("waterfall3DSpectrumSliceScrollStep"),
                                profileSpectrumStep->value());
                settings.insert(QStringLiteral("waterfall3DSpectrumSliceRows"),
                                profileSpectrumRows->value());
                settings.insert(QStringLiteral("waterfall3DSpectrumSliceCapture"),
                                profileCapture->isChecked());
                settings.insert(QStringLiteral("waterfall3DSpectrumSliceCaptureFixed"),
                                profileCaptureFixed->isChecked());
                settings.insert(QStringLiteral("waterfall3DVncSliceInput"),
                                profileVncInput->isChecked());
            }
            dspFlowPanel->setBlockSettings(id, settings);
        };
        connect(minimum, &QSlider::valueChanged, dialog,
                [applyProfile](int) { applyProfile(); });
        connect(maximum, &QSlider::valueChanged, dialog,
                [applyProfile](int) { applyProfile(); });
        connect(globalLevels, &QRadioButton::toggled, dialog,
                [applyProfile, levelOverride, minimum, maximum](bool checked) {
                    if (!checked) return;
                    *levelOverride = false;
                    minimum->setEnabled(false);
                    maximum->setEnabled(false);
                    applyProfile();
                });
        connect(individualLevels, &QRadioButton::toggled, dialog,
                [applyProfile, levelOverride, minimum, maximum](bool checked) {
                    if (!checked) return;
                    *levelOverride = true;
                    minimum->setEnabled(true);
                    maximum->setEnabled(true);
                    applyProfile();
                });
        if (profileSensitivity)
            connect(profileSensitivity, &QSlider::valueChanged, dialog,
                    [applyProfile](int) { applyProfile(); });
        if (profileContrast)
            connect(profileContrast, &QSlider::valueChanged, dialog,
                    [applyProfile](int) { applyProfile(); });
        if (profileMode)
            connect(profileMode, QOverload<int>::of(&QComboBox::currentIndexChanged), dialog,
                    [applyProfile](int) { applyProfile(); });
        connect(profileColor, &QCheckBox::toggled, dialog,
                [applyProfile](bool) { applyProfile(); });
        connect(profileGradient, &QCheckBox::toggled, dialog,
                [applyProfile](bool) { applyProfile(); });
        if (profileOpacity)
            connect(profileOpacity, &QSlider::valueChanged, dialog,
                    [applyProfile](int) { applyProfile(); });
        connect(profileFixedPlane, &QCheckBox::toggled, dialog,
                [applyProfile](bool) { applyProfile(); });
        connect(profileMonochrome, &QCheckBox::toggled, dialog,
                [applyProfile](bool) { applyProfile(); });
        if (profileResolution)
            connect(profileResolution, QOverload<int>::of(&QComboBox::currentIndexChanged), dialog,
                    [applyProfile](int) { applyProfile(); });
        if (profileSurface)
            connect(profileSurface, QOverload<int>::of(&QComboBox::currentIndexChanged), dialog,
                    [applyProfile](int) { applyProfile(); });
        if (profileSmoothing)
            connect(profileSmoothing, QOverload<int>::of(&QComboBox::currentIndexChanged), dialog,
                    [applyProfile](int) { applyProfile(); });
        if (profileLighting)
            connect(profileLighting, QOverload<int>::of(&QComboBox::currentIndexChanged), dialog,
                    [applyProfile](int) { applyProfile(); });
        for (QSlider *slider : {profileHistory, profileFrequencyStep, profileFrequencyWidth,
                                profileSpectrumStep, profileSpectrumRows}) {
            if (slider) connect(slider, &QSlider::valueChanged, dialog,
                                [applyProfile](int) { applyProfile(); });
        }
        if (profileCapture) {
            connect(profileCapture, &QCheckBox::toggled, dialog,
                    [applyProfile, profileCaptureFixed](bool checked) {
                        profileCaptureFixed->setEnabled(checked);
                        applyProfile();
                    });
            connect(profileCaptureFixed, &QCheckBox::toggled, dialog,
                    [applyProfile](bool) { applyProfile(); });
            connect(profileVncInput, &QCheckBox::toggled, dialog,
                    [applyProfile](bool) { applyProfile(); });
        }
        applyProfile();
    } else if (type == QStringLiteral("second_spectrum")) {
        title = text("Другий спектр", "Second spectrum");
        form->addRow(QString(), linkedCheckBox(graphCheckbox, text("Показати другий спектр", "Show second spectrum"),
                                               dialog, syncTimer));
    } else if (type == QStringLiteral("agile_scan")) {
        title = QStringLiteral("Agile scan");
        form->addRow(QString(), linkedCheckBox(agileScanCheckbox, text("Увімкнути", "Enable"), dialog, syncTimer));
        form->addRow(text("Пресет:", "Preset:"), linkedCombo(agileScanPresetCombo, dialog, syncTimer));
        form->addRow(text("Відображення:", "Display:"), linkedCombo(scanVisualModeCombo, dialog, syncTimer));
        form->addRow(text("Діапазони:", "Ranges:"), linkedLineEdit(agileScanRangesEdit, dialog, syncTimer));
        form->addRow(text("Крок, МГц:", "Step, MHz:"), linkedDoubleSpin(agileScanStepSpin, dialog, syncTimer));
        form->addRow(QString(), linkedCheckBox(agileScanAutoStepCheckbox, text("Крок = sample rate", "Step = sample rate"),
                                               dialog, syncTimer));
        form->addRow(QString(), buttonRow({actionButton(agileScanSavePresetButton, text("Зберегти", "Save")),
                                          actionButton(agileScanDeletePresetButton, text("Видалити", "Delete"))}));
    } else if (type == QStringLiteral("standard_scan")) {
        title = text("Стандартний скан", "Standard scan");
        form->addRow(QString(), linkedCheckBox(standardScanCheckbox, text("Увімкнути", "Enable"), dialog, syncTimer));
        form->addRow(text("Пресет:", "Preset:"), linkedCombo(standardScanPresetCombo, dialog, syncTimer));
        form->addRow(text("Центри, МГц:", "Centers, MHz:"), linkedLineEdit(standardScanCentersEdit, dialog, syncTimer));
        form->addRow(text("Початок:", "Range start:"), linkedLineEdit(standardScanRangeStartEdit, dialog, syncTimer));
        form->addRow(text("Кінець:", "Range end:"), linkedLineEdit(standardScanRangeEndEdit, dialog, syncTimer));
        form->addRow(text("Затримка:", "Dwell:"), linkedSpin(standardScanDwellSpin, dialog, syncTimer));
        form->addRow(text("Стабілізація:", "Settle:"), linkedSpin(standardScanSettleSpin, dialog, syncTimer));
        form->addRow(QString(), actionButton(standardScanFillRangeButton, text("Заповнити діапазон", "Fill range")));
        form->addRow(QString(), buttonRow({actionButton(standardScanSavePresetButton, text("Зберегти", "Save")),
                                          actionButton(standardScanDeletePresetButton, text("Видалити", "Delete"))}));
    } else if (type == QStringLiteral("listening_scan")) {
        title = text("Скан прослуховування", "Listening scan");
        form->addRow(QString(), linkedCheckBox(listeningScanCheckbox, text("Увімкнути", "Enable"), dialog, syncTimer));
        form->addRow(QString(), linkedCheckBox(scanListeningLockCheckbox,
                                               text("Фіксувати частоту прослуховування", "Lock listening frequency"),
                                               dialog, syncTimer));
        form->addRow(text("Пресет:", "Preset:"), linkedCombo(listeningScanPresetCombo, dialog, syncTimer));
        form->addRow(text("Частоти, МГц:", "Frequencies, MHz:"), linkedLineEdit(listeningScanTargetsEdit, dialog, syncTimer));
        form->addRow(text("Затримка:", "Dwell:"), linkedSpin(listeningScanDwellSpin, dialog, syncTimer));
        form->addRow(text("Стабілізація:", "Settle:"), linkedSpin(listeningScanSettleSpin, dialog, syncTimer));
        form->addRow(QString(), buttonRow({actionButton(listeningScanSavePresetButton, text("Зберегти", "Save")),
                                          actionButton(listeningScanDeletePresetButton, text("Видалити", "Delete"))}));
    } else if (type == QStringLiteral("spectrum_measurement")) {
        title = text("Вимірювання спектра", "Spectrum measurement");
        form->addRow(QString(), linkedCheckBox(scanMeasurementCheckbox,
                                               text("Накопичення вимірювань", "Measurement accumulation"),
                                               dialog, syncTimer));
        form->addRow(text("Крок біну:", "Bin width:"),
                     linkedDoubleSpin(scanMeasurementBinSpin, dialog, syncTimer));
        form->addRow(QString(), linkedCheckBox(spectrumScienceMaxHoldCheckbox,
                                               QStringLiteral("Max hold"), dialog, syncTimer));
        form->addRow(QString(), linkedCheckBox(spectrumScienceMinHoldCheckbox,
                                               QStringLiteral("Min hold"), dialog, syncTimer));
        form->addRow(QString(), linkedCheckBox(spectrumScienceAverageCheckbox,
                                               text("Усереднення", "Average"), dialog, syncTimer));
        form->addRow(text("Час усереднення:", "Average time:"),
                     linkedDoubleSpin(spectrumScienceAverageSpin, dialog, syncTimer));
        form->addRow(QString(), linkedCheckBox(waterfallAreaMeasurementCheckbox,
                                               text("Лінійка водоспаду", "Waterfall ruler"),
                                               dialog, syncTimer));
        form->addRow(text("Маркер:", "Marker:"),
                     linkedCombo(spectrumScienceMarkerCombo, dialog, syncTimer));
        form->addRow(QString(), buttonRow({
            actionButton(spectrumScienceSetButton, text("Встановити", "Set")),
            actionButton(spectrumSciencePeakButton, text("Пік", "Peak")),
            actionButton(spectrumScienceClearButton, text("Очистити", "Clear"))
        }));
        form->addRow(QString(), buttonRow({
            actionButton(spectrumSciencePreviousButton, text("Попередній", "Previous")),
            actionButton(spectrumScienceNextButton, text("Наступний", "Next"))
        }));
        form->addRow(QString(), buttonRow({
            actionButton(scanMeasurementBaselineButton, QStringLiteral("Baseline")),
            actionButton(scanMeasurementResetPeakButton, text("Скинути піки", "Reset peaks")),
            actionButton(scanMeasurementExportButton, QStringLiteral("CSV"))
        }));
        form->addRow(QString(), buttonRow({
            actionButton(spectrumScienceResetButton, text("Скинути", "Reset")),
            actionButton(spectrumScienceExportButton, text("Звіт", "Report"))
        }));
        form->addRow(QString(), buttonRow({
            actionButton(zeroSpanButton, QStringLiteral("Zero-span")),
            actionButton(researchToolsButton, text("Дослідження", "Research"))
        }));
        form->addRow(text("Накопичення:", "Accumulation:"),
                     linkedLabel(scanMeasurementStatusLabel, dialog, syncTimer));
        form->addRow(text("Маркери:", "Markers:"),
                     linkedLabel(spectrumScienceMarkerStatusLabel, dialog, syncTimer));
        form->addRow(text("Результати:", "Results:"),
                     linkedLabel(spectrumScienceMetricsLabel, dialog, syncTimer));
    } else if (type == QStringLiteral("spectrum_recorder")) {
        title = text("Запис кадрів спектра", "Spectrum frame recorder");
        form->addRow(QString(), linkedCheckBox(spectrumFrameBufferCheckbox, text("Постійний передбуфер", "Continuous prebuffer"),
                                               dialog, syncTimer));
        form->addRow(text("Режим:", "Mode:"), linkedCombo(spectrumEventModeCombo, dialog, syncTimer));
        form->addRow(text("Передбуфер:", "Prebuffer:"), linkedSpin(spectrumFramePrebufferSpin, dialog, syncTimer));
        form->addRow(text("Біни:", "Bins:"), linkedCombo(spectrumFrameBinsCombo, dialog, syncTimer));
        form->addRow(QString(), actionButton(spectrumFrameRecordButton, text("Запис / стоп", "Record / stop")));
    } else if (type == QStringLiteral("spur_suppression")) {
        title = text("Придушення spur", "Spur suppression");
        form->addRow(QString(), linkedCheckBox(spurSuppressionCheckbox, text("Увімкнути", "Enable"), dialog, syncTimer));
        form->addRow(QString(), buttonRow({actionButton(spurCalibrateButton, text("Калібрувати", "Calibrate")),
                                          actionButton(spurClearButton, text("Очистити", "Clear"))}));
    } else if (type == QStringLiteral("playback")) {
        title = text("Відтворення запису", "Recording playback");
        form->addRow(text("Файл:", "File:"), linkedCombo(playbackFileCombo, dialog, syncTimer));
        form->addRow(QString(), buttonRow({actionButton(playbackRefreshButton, text("Оновити", "Refresh")),
                                          actionButton(playbackButton, text("Відтворити / стоп", "Play / stop"))}));
    } else if (type == QStringLiteral("audio_output")) {
        title = text("Аудіовихід", "Audio output");
        form->addRow(QString(), linkedCheckBox(audioCheckbox, text("Аудіо увімкнено", "Audio enabled"),
                                               dialog, syncTimer));
        form->addRow(text("Пристрій:", "Device:"), linkedCombo(audioDeviceComboBox, dialog, syncTimer));
        form->addRow(text("Гучність:", "Volume:"), linkedSlider(volumeSlider, dialog, syncTimer, QStringLiteral("%")));
    } else if (type == QStringLiteral("decoder") || type == QStringLiteral("digital_audio_settings")) {
        title = type == QStringLiteral("digital_audio_settings")
                    ? text("Налаштування цифрового аудіо", "Digital audio settings")
                    : text("Цифровий декодер", "Digital decoder");
        form->addRow(QString(), linkedCheckBox(digitalDecodeCheckbox, text("Декодувати", "Decode"),
                                               dialog, syncTimer));
        form->addRow(text("DMR backend:", "DMR backend:"), linkedCombo(dmrBackendCombo, dialog, syncTimer));
        form->addRow(QString(), linkedLabel(dmrPrivacyCapabilityLabel, dialog, syncTimer));
        form->addRow(text("4FSK rate:", "4FSK rate:"), linkedCombo(dmrBasebandRateCombo, dialog, syncTimer));
        form->addRow(text("Тон BPSK31:", "BPSK31 tone:"), linkedDoubleSpin(pskDecoderToneSpin, dialog, syncTimer));
        form->addRow(text("Вибірковість BPSK31:", "BPSK31 selectivity:"), linkedSpin(pskDecoderSquelchSpin, dialog, syncTimer));
        form->addRow(QString(), linkedCheckBox(pskDecoderAfcCheckbox, QStringLiteral("PSK AFC"), dialog, syncTimer));
        form->addRow(text("Кодування PSK:", "PSK encoding:"), linkedCombo(pskDecoderEncodingCombo, dialog, syncTimer));
        form->addRow(text("Канал:", "Channel:"), linkedCombo(dmrChannelRateCombo, dialog, syncTimer));
        form->addRow(QString(), commandButton(text("Відкрити текстовий вихід", "Open text output"),
                                              [this, id]() { openDspBlockReference(QStringLiteral("digital_text_output"), id + QStringLiteral(":text")); }));
    } else if (type == QStringLiteral("dmr_decoder")) {
        title = QStringLiteral("DMR");
        form->addRow(QString(), linkedCheckBox(digitalDecodeCheckbox, text("Декодувати", "Decode"), dialog, syncTimer));
        form->addRow(text("Backend:", "Backend:"), linkedCombo(dmrBackendCombo, dialog, syncTimer));
        form->addRow(QString(), linkedLabel(dmrPrivacyCapabilityLabel, dialog, syncTimer));
        form->addRow(text("Baseband:", "Baseband:"), linkedCombo(dmrBasebandRateCombo, dialog, syncTimer));
        form->addRow(text("Канал:", "Channel:"), linkedCombo(dmrChannelRateCombo, dialog, syncTimer));
        form->addRow(text("AMBE layout:", "AMBE layout:"), linkedCombo(dmrAmbeLayoutCombo, dialog, syncTimer));
        form->addRow(text("Privacy:", "Privacy:"), linkedCombo(dmrPrivacyModeCombo, dialog, syncTimer));
        form->addRow(text("Key ID:", "Key ID:"), linkedCombo(dmrPrivacyKeyIdCombo, dialog, syncTimer));
        form->addRow(text("Frame offset:", "Frame offset:"), linkedCombo(dmrPrivacyFrameOffsetCombo, dialog, syncTimer));
        form->addRow(text("Drop alignment:", "Drop alignment:"), linkedCombo(dmrPrivacyDropCombo, dialog, syncTimer));
        form->addRow(QString(), linkedCheckBox(dmrAdaptiveSlicerCheckbox, text("Адаптивний slicer", "Adaptive slicer"),
                                               dialog, syncTimer));
        form->addRow(QString(), actionButton(dmrPrivacyKeysButton, text("Редактор ключів", "Key editor")));
        form->addRow(QString(), commandButton(text("Відкрити текстовий вихід", "Open text output"),
                                              [this, id]() { openDspBlockReference(QStringLiteral("digital_text_output"), id + QStringLiteral(":text")); }));
    } else if (type == QStringLiteral("cw_decoder")) {
        title = text("Декодер CW", "CW decoder");
        form->addRow(QString(), linkedCheckBox(digitalDecodeCheckbox, text("Декодувати", "Decode"), dialog, syncTimer));
        form->addRow(text("Абетка:", "Alphabet:"), linkedCombo(cwDecoderAlphabetCombo, dialog, syncTimer));
        form->addRow(text("Тон:", "Tone:"), linkedSpin(cwDecoderToneSpin, dialog, syncTimer));
        form->addRow(text("Швидкість:", "Speed:"), linkedSpin(cwDecoderWpmSpin, dialog, syncTimer));
        form->addRow(text("Вибірковість:", "Selectivity:"),
                     linkedSpin(cwDecoderSelectivitySpin, dialog, syncTimer));
        form->addRow(QString(), linkedCheckBox(cwDecoderAdaptiveCheckbox, text("Авто швидкість", "Adaptive speed"),
                                               dialog, syncTimer));
        form->addRow(QString(), commandButton(text("Відкрити текстовий вихід", "Open text output"),
                                              [this, id]() { openDspBlockReference(QStringLiteral("digital_text_output"), id + QStringLiteral(":text")); }));
    } else if (type == QStringLiteral("sstv_decoder")) {
        title = QStringLiteral("SSTV");
        form->addRow(QString(), linkedCheckBox(videoDecodeCheckbox, text("Декодувати відео", "Decode video"), dialog, syncTimer));
        form->addRow(text("Демодуляція:", "Demodulation:"), linkedCombo(sstvDemodulationCombo, dialog, syncTimer));
        form->addRow(text("Стандарт:", "Standard:"), linkedCombo(videoStandardCombo, dialog, syncTimer));
        form->addRow(QString(), linkedCheckBox(videoInvertCheckbox, text("Інверсія", "Invert"), dialog, syncTimer));
        form->addRow(QString(), commandButton(text("Відкрити зображення", "Open image output"),
                                              [this, id]() { openDspBlockReference(QStringLiteral("digital_image_output"), id + QStringLiteral(":image")); }));
    } else if (type == QStringLiteral("digital_video") || type == QStringLiteral("digital_video_settings")) {
        title = type == QStringLiteral("digital_video_settings")
                    ? text("Налаштування цифрового відео", "Digital video settings")
                    : text("Цифрове відео", "Digital video");
        form->addRow(QString(), linkedCheckBox(videoDecodeCheckbox, text("Декодувати", "Decode"), dialog, syncTimer));
        form->addRow(text("Демодуляція:", "Demodulation:"), linkedCombo(videoDemodCombo, dialog, syncTimer));
        form->addRow(text("Стандарт:", "Standard:"), linkedCombo(videoStandardCombo, dialog, syncTimer));
        form->addRow(QString(), linkedCheckBox(videoHSyncCheckbox, QStringLiteral("H sync"), dialog, syncTimer));
        form->addRow(QString(), linkedCheckBox(videoVSyncCheckbox, QStringLiteral("V sync"), dialog, syncTimer));
        form->addRow(QString(), linkedCheckBox(videoTestPatternCheckbox, text("Тестовий шаблон", "Test pattern"), dialog, syncTimer));
        form->addRow(QString(), commandButton(text("Декодувати LRPT запис (SatDump)", "Decode LRPT recording (SatDump)"), [this] { showLrptDecoder(); }));
        form->addRow(QString(), commandButton(text("Відкрити зображення", "Open image output"),
                                              [this, id]() { openDspBlockReference(QStringLiteral("digital_image_output"), id + QStringLiteral(":image")); }));
    } else if (type == QStringLiteral("dmr_hunter") || type == QStringLiteral("fpv_hunter") ||
               type == QStringLiteral("digital_video_hunter")) {
        const bool dmr = type == QStringLiteral("dmr_hunter");
        const bool fpv = type == QStringLiteral("fpv_hunter");
        title = dmr ? QStringLiteral("DMR Hunter") : (fpv ? QStringLiteral("FPV Hunter") : QStringLiteral("Digital Video Hunter"));
        SpectrumHunterControls *controls = dmr ? dmrHunterControls : (fpv ? fpvHunterControls : digitalVideoHunterControls);
        form->addRow(QString(), commandButton(text("Увімкнути / вимкнути пошук", "Toggle detection"), [this, type, controls]() {
            if (!controls) return;
            const bool enabled = type == QStringLiteral("dmr_hunter") ? dmrHunterSettings.enabled
                               : type == QStringLiteral("fpv_hunter") ? fpvHunterSettings.enabled
                                                                      : digitalVideoHunterSettings.enabled;
            controls->setDetectChecked(!enabled);
        }));
        form->addRow(QString(), commandButton(text("Застосувати пресет до скану", "Apply preset to scan"), [this, type]() {
            if (type == QStringLiteral("dmr_hunter")) applyDmrHunterPresetToScan();
            else if (type == QStringLiteral("fpv_hunter")) applyFpvHunterPresetToScan();
            else applyDigitalVideoHunterPresetToScan();
        }));
        form->addRow(QString(), commandButton(text("Перейти до кандидата", "Tune candidate"), [this, type]() {
            if (type == QStringLiteral("dmr_hunter")) tuneDmrHunterCandidate();
            else if (type == QStringLiteral("fpv_hunter")) tuneFpvHunterCandidate();
            else tuneDigitalVideoHunterCandidate();
        }));
        auto *note = new QLabel(text(
            "Керування пошуком доступне безпосередньо в цьому вікні.",
            "Hunter controls are available directly in this window."), dialog);
        note->setWordWrap(true);
        form->addRow(QString(), note);
    } else if (type == QStringLiteral("gnss_sdr")) {
        title = text("GNSS через SDR", "GNSS via SDR");
        form->addRow(text("Система:", "System:"), linkedCombo(gnssSystemCombo, dialog, syncTimer));
        form->addRow(QString(), linkedCheckBox(gnssMonitorCheckbox, text("Безперервний аналіз", "Continuous analysis"),
                                               dialog, syncTimer));
        form->addRow(text("Інтеграція:", "Integration:"), linkedSpin(gnssIntegrationSpin, dialog, syncTimer));
        form->addRow(text("Фільтр каналу:", "Channel filter:"), linkedDoubleSpin(gnssChannelFilterSpin, dialog, syncTimer));
        form->addRow(text("Doppler span:", "Doppler span:"), linkedSpin(gnssDopplerSpanSpin, dialog, syncTimer));
        form->addRow(text("Doppler step:", "Doppler step:"), linkedSpin(gnssDopplerStepSpin, dialog, syncTimer));
        form->addRow(QString(), buttonRow({actionButton(gnssTuneButton, text("Налаштувати", "Tune")),
                                          actionButton(gnssScanButton, text("Скан", "Scan")),
                                          actionButton(gnssAcquireButton, text("Накопичення", "Acquire"))}));
        form->addRow(QString(), buttonRow({actionButton(gnssPlotButton, text("Графік", "Plot")),
                                          actionButton(gnssSatellitesButton, text("Супутники", "Satellites")),
                                          actionButton(gnssMonitorResetButton, text("Скинути", "Reset"))}));
        form->addRow(QString(), commandButton(text("Позиція з SDR (PocketSDR)", "SDR PVT (PocketSDR)"), [this] { showGnssSdrPvt(); }));
    } else if (type == QStringLiteral("gnss_serial")) {
        title = text("GNSS модуль / NMEA / UBX", "GNSS module / NMEA / UBX");
        form->addRow(text("Порт:", "Port:"), linkedCombo(gnssSerialPortEdit, dialog, syncTimer));
        form->addRow(text("Baud:", "Baud:"), linkedSpin(gnssSerialBaudSpin, dialog, syncTimer));
        form->addRow(text("Політика позиції:", "Position policy:"), linkedCombo(gnssPositionPolicyCombo, dialog, syncTimer));
        form->addRow(text("Часовий пояс:", "Time zone:"), linkedCombo(gnssTimeZoneCombo, dialog, syncTimer));
        form->addRow(QString(), buttonRow({actionButton(gnssSerialButton, text("Підключити / стоп", "Connect / stop")),
                                          actionButton(gnssSatellitesButton, text("Супутники", "Satellites")),
                                          actionButton(qthMapButton, text("Карта", "Map"))}));
        form->addRow(QString(), buttonRow({actionButton(gnssUbxSystemsButton, QStringLiteral("UBX CFG-GNSS")),
                                          actionButton(gnssUbxSaveButton, text("Зберегти в модуль", "Save to module"))}));
        form->addRow(QString(), buttonRow({actionButton(gnssNmeaLogButton, QStringLiteral("NMEA log")),
                                          actionButton(gnssSerialRawLogButton, QStringLiteral("Raw UBX/NMEA")),
                                          actionButton(gnssNmeaReplayButton, text("Реплей", "Replay"))}));
    } else if (type == QStringLiteral("gpio")) {
        title = QStringLiteral("GPIO");
        for (int i = 0; i < 8; ++i)
            form->addRow(QString(), linkedCheckBox(checkBoxes[i], QStringLiteral("GPIO %1").arg(i + 1), dialog, syncTimer));
    } else if (type == QStringLiteral("recorder")) {
        title = text("Записувач", "Recorder");
        form->addRow(text("Режим:", "Mode:"), linkedCombo(recordingModeCombo, dialog, syncTimer));
        form->addRow(QString(), linkedCheckBox(spectrumFrameBufferCheckbox,
                                               text("Передбуфер спектра", "Spectrum prebuffer"),
                                               dialog, syncTimer));
        form->addRow(text("Передбуфер:", "Prebuffer:"), linkedSpin(spectrumFramePrebufferSpin, dialog, syncTimer));
        form->addRow(text("Біни:", "Bins:"), linkedCombo(spectrumFrameBinsCombo, dialog, syncTimer));
        auto *record = new QPushButton(text("Запис", "Record"), dialog);
        connect(record, &QPushButton::clicked, recordButton, &QPushButton::click);
        form->addRow(QString(), record);
    } else {
        dialog->deleteLater();
        return;
    }

    dialog->setWindowTitle(title);
    auto *stayOnTop = new QCheckBox(text("Залишатися поверх вікон", "Stay on top"), dialog);
    stayOnTop->setObjectName(QStringLiteral("dspStayOnTopCheckBox"));
    stayOnTop->setChecked(true);
    dialog->setWindowFlag(Qt::WindowStaysOnTopHint, true);
    connect(stayOnTop, &QCheckBox::toggled, dialog, [dialog](bool enabled) {
        dialog->setWindowFlag(Qt::WindowStaysOnTopHint, enabled);
        dialog->show();
        dialog->raise();
        dialog->activateWindow();
    });
    root->addWidget(stayOnTop);
    auto *dockSectorCombo = new QComboBox(dialog);
    dockSectorCombo->setObjectName(QStringLiteral("dspDockSectorCombo"));
    const int sectorCount = dspFlowPanel ? (std::max)(1, dspFlowPanel->workspaceSectorCount()) : 1;
    for (int sector = 0; sector < sectorCount; ++sector) {
        dockSectorCombo->addItem(text("Сектор %1", "Sector %1").arg(sector + 1), sector);
    }
    if (dspFlowPanel) {
        const QJsonObject currentSettings = dspFlowPanel->blockSettings(id);
        const int currentSector = currentSettings.value(QStringLiteral("dockedSettingsSector")).toInt(
            currentSettings.value(QStringLiteral("workspaceSector")).toInt(0));
        dockSectorCombo->setCurrentIndex(std::clamp(currentSector, 0, sectorCount - 1));
    }
    auto *dockToWorkspace = new QPushButton(
        text("Закріпити", "Dock"), dialog);
    dockToWorkspace->setObjectName(QStringLiteral("dspDockToWorkspaceButton"));
    connect(dockToWorkspace, &QPushButton::clicked, dialog,
            [this, id, dialog, dockToWorkspace, dockSectorCombo]() {
                if (dspFlowPanel && dspFlowPanel->dockSettingsDialog(
                        id, dialog, dockSectorCombo->currentData().toInt())) {
                    dockToWorkspace->hide();
                    dockSectorCombo->hide();
                }
            });
    auto *dockRow = new QHBoxLayout();
    dockRow->setContentsMargins(0, 0, 0, 0);
    dockRow->addWidget(dockSectorCombo, 1);
    dockRow->addWidget(dockToWorkspace);
    root->addLayout(dockRow);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    buttons->button(QDialogButtonBox::Close)->setText(text("Закрити", "Close"));
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    root->addWidget(buttons);

    dspBlockEditors.insert(editorKey, dialog);
    connect(dialog, &QObject::destroyed, this, [this, editorKey]() {
        dspBlockEditors.remove(editorKey);
    });
    const bool restoreDocked = dspFlowPanel &&
        dspFlowPanel->blockSettings(id).contains(QStringLiteral("dockedSettingsSector"));
    dialog->show();
    dialog->adjustSize();
    if (restoreDocked && dspFlowPanel->dockSettingsDialog(id, dialog)) {
        dockToWorkspace->hide();
        dockSectorCombo->hide();
    } else {
        dialog->raise();
        dialog->activateWindow();
    }
}

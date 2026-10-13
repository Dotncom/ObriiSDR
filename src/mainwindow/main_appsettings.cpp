#include "main.h"
#include "multivfowidget.h"

#include "appconstants.h"
#include "apphelp.h"
#include "appsettingsutils.h"
#include "diagnosticlogging.h"
#include "dspflowpanel.h"
#include "spaceeventsfeed.h"
#include "wirelesslabdialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

extern bool secondGraph;

namespace {

struct InterfaceLayoutProfile {
    QString name;
    QByteArray geometry;
    QByteArray windowState;
    QJsonObject view;
};

QVector<InterfaceLayoutProfile> loadInterfaceLayoutProfiles() {
    QSettings settings(persistentSettingsFilePath(), QSettings::IniFormat);
    QVector<InterfaceLayoutProfile> profiles;
    const int count = settings.beginReadArray(QStringLiteral("uiLayoutProfiles"));
    profiles.reserve(count);
    for (int index = 0; index < count; ++index) {
        settings.setArrayIndex(index);
        InterfaceLayoutProfile profile;
        profile.name = settings.value(QStringLiteral("name")).toString().trimmed();
        profile.geometry = settings.value(QStringLiteral("geometry")).toByteArray();
        profile.windowState = settings.value(QStringLiteral("windowState")).toByteArray();
        const QJsonDocument viewDocument = QJsonDocument::fromJson(
            settings.value(QStringLiteral("view")).toByteArray());
        if (viewDocument.isObject()) profile.view = viewDocument.object();
        if (!profile.name.isEmpty()) profiles.push_back(profile);
    }
    settings.endArray();
    std::sort(profiles.begin(), profiles.end(), [](const auto &left, const auto &right) {
        return left.name.compare(right.name, Qt::CaseInsensitive) < 0;
    });
    return profiles;
}

void storeInterfaceLayoutProfiles(const QVector<InterfaceLayoutProfile> &profiles) {
    QSettings settings(persistentSettingsFilePath(), QSettings::IniFormat);
    settings.remove(QStringLiteral("uiLayoutProfiles"));
    settings.beginWriteArray(QStringLiteral("uiLayoutProfiles"), profiles.size());
    for (int index = 0; index < profiles.size(); ++index) {
        settings.setArrayIndex(index);
        settings.setValue(QStringLiteral("name"), profiles[index].name);
        settings.setValue(QStringLiteral("geometry"), profiles[index].geometry);
        settings.setValue(QStringLiteral("windowState"), profiles[index].windowState);
        settings.setValue(QStringLiteral("view"),
                          QJsonDocument(profiles[index].view).toJson(QJsonDocument::Compact));
    }
    settings.endArray();
    settings.sync();
}

} // namespace

void YourClassName::showConfiguredInterface() {
    if (alternativeInterfaceMode) {
        applyAlternativeInterfaceMode();
        return;
    }
#ifdef _WIN32
    showNormal();
    raise();
    activateWindow();
#else
    show();
#endif
}
void YourClassName::applyAlternativeInterfaceMode() {
    if (!graphLayout || !graphWidget || !scaleWidget || !waterfallWidget) {
        return;
    }

    graphLayout->removeWidget(graphWidget);
    graphLayout->removeWidget(scaleWidget);
    graphLayout->removeWidget(waterfallWidget);

    if (waterfall3DAlternativeView) {
        if (secondGraph) {
            graphWidget->show();
            graphLayout->insertWidget(0, graphWidget, 2);
            graphLayout->insertWidget(1, waterfallWidget, 5);
            graphLayout->insertWidget(2, scaleWidget, 0);
        } else {
            graphWidget->hide();
            graphLayout->insertWidget(0, waterfallWidget, 1);
            graphLayout->insertWidget(1, scaleWidget, 0);
        }
    } else {
        graphWidget->show();
        graphLayout->insertWidget(0, graphWidget, 2);
        graphLayout->insertWidget(1, scaleWidget, 0);
        graphLayout->insertWidget(2, waterfallWidget, 5);
    }

    for (int index = 0; index < graphLayout->count(); ++index) {
        graphLayout->setStretch(index, 0);
    }
    if (waterfall3DAlternativeView) {
        if (secondGraph) {
            graphLayout->setStretch(0, 2);
            graphLayout->setStretch(1, 5);
        } else {
            graphLayout->setStretch(0, 1);
        }
    } else {
        graphLayout->setStretch(0, 2);
        graphLayout->setStretch(2, 5);
    }

    const bool alternativeMiniMode =
        waterfallDisplayMode == static_cast<int>(MyWaterfallWidget::DisplayMode::Waterfall3DWithMini);
    const int effectiveMode = waterfall3DAlternativeView
                                  ? (alternativeMiniMode
                                         ? static_cast<int>(MyWaterfallWidget::DisplayMode::Waterfall3DWithMini)
                                         : static_cast<int>(MyWaterfallWidget::DisplayMode::Waterfall3D))
                                  : waterfallDisplayMode;
    waterfallWidget->setAlternativeInterfaceMode(waterfall3DAlternativeView);
    graphWidget->setFrequencyAxisLabelsVisible(waterfall3DAlternativeView && secondGraph);
    waterfallWidget->setDisplayMode(
        static_cast<MyWaterfallWidget::DisplayMode>(effectiveMode));

    if (waterfallDisplayModeCombo) {
        QSignalBlocker blocker(waterfallDisplayModeCombo);
        if (auto *model = qobject_cast<QStandardItemModel*>(waterfallDisplayModeCombo->model())) {
            for (int row = 0; row < model->rowCount(); ++row) {
                if (QStandardItem *item = model->item(row)) {
                    const int mode = waterfallDisplayModeCombo->itemData(row).toInt();
                    item->setEnabled(!waterfall3DAlternativeView ||
                                     mode == static_cast<int>(MyWaterfallWidget::DisplayMode::Waterfall3D) ||
                                     mode == static_cast<int>(MyWaterfallWidget::DisplayMode::Waterfall3DWithMini));
                }
            }
        }
        const int index = waterfallDisplayModeCombo->findData(effectiveMode);
        if (index >= 0) {
            waterfallDisplayModeCombo->setCurrentIndex(index);
        }
        waterfallDisplayModeCombo->setToolTip(waterfall3DAlternativeView
            ? uiText(QStringLiteral("alternative_interface_mode_locked"),
                     QStringLiteral("Alternative interface supports fixed 3D and 3D with a mini waterfall."))
            : uiText(QStringLiteral("waterfall_display_mode_tooltip"),
                     QStringLiteral("Choose 2D waterfall, 3D waterfall, or 3D with a 2D overview.")));
    }
    if (alternativeSpectrumGradientCheckbox) {
        alternativeSpectrumGradientCheckbox->setEnabled(
            waterfall3DAlternativeView || waterfall3DFixedPlane);
    }
    if (alternativeSpectrumGradientOpacitySlider) {
        alternativeSpectrumGradientOpacitySlider->setEnabled(
            (waterfall3DAlternativeView || waterfall3DFixedPlane) &&
            alternativeSpectrumGradientFill);
    }
    if (waterfall3DFixedPlaneCheckbox) {
        waterfall3DFixedPlaneCheckbox->setEnabled(!waterfall3DAlternativeView);
    }

    centralWidget->updateGeometry();
    waterfallWidget->updateGeometry();
    waterfallWidget->update();

    if (alternativeInterfaceMode) {
        ensureDspFlowPanel();
        dspFlowPanel->setWorkspaceMode(true);
        dspFlowPanel->showMaximized();
        dspFlowPanel->raise();
        dspFlowPanel->activateWindow();
        QTimer::singleShot(0, this, [this]() {
            if (alternativeInterfaceMode) hide();
        });
    } else if (dspFlowPanel && dspFlowPanel->isWorkspaceMode()) {
        dspFlowPanel->setWorkspaceMode(false);
        dspFlowPanel->hide();
#ifdef _WIN32
        showNormal();
        raise();
        activateWindow();
#else
        show();
#endif
    }
}

void YourClassName::openApplicationHelp() {
    QWidget *parentWidget = QApplication::activeWindow();
    QDialog dialog(parentWidget ? parentWidget : static_cast<QWidget*>(this));
    dialog.setWindowTitle(uiText(QStringLiteral("program_help_title"),
                                 QStringLiteral("Obrii SDR feature guide")));
    dialog.resize(760, 640);

    QVBoxLayout *rootLayout = new QVBoxLayout(&dialog);
    QPlainTextEdit *helpText = new QPlainTextEdit(&dialog);
    helpText->setReadOnly(true);
    helpText->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    helpText->setPlainText(applicationHelpText(uiLanguage));
    rootLayout->addWidget(helpText, 1);

    QDialogButtonBox *buttonBox = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    if (QPushButton *closeButton = buttonBox->button(QDialogButtonBox::Close)) {
        closeButton->setText(uiText(QStringLiteral("close"), QStringLiteral("Close")));
    }
    rootLayout->addWidget(buttonBox);
    connect(buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    dialog.exec();
}

void YourClassName::openApplicationSettings() {
    QDialog dialog(this);
    dialog.setWindowTitle(uiText(QStringLiteral("settings"), QStringLiteral("Settings...")));
    dialog.setMinimumWidth(420);

    QVBoxLayout *rootLayout = new QVBoxLayout(&dialog);

    QFormLayout *generalLayout = new QFormLayout();
    QComboBox *languageCombo = new QComboBox(&dialog);
    populateLanguageCombo(languageCombo);
    languageCombo->setCurrentIndex(languageCombo->findData(uiLanguage));
    if (languageCombo->currentIndex() < 0) {
        languageCombo->setCurrentIndex(0);
    }

    QComboBox *fineTuneModeCombo = new QComboBox(&dialog);
    fineTuneModeCombo->addItem(uiText(QStringLiteral("fine_tune_scale"), QStringLiteral("Horizontal scale (mouse wheel)")),
                               FINE_TUNE_MODE_SCALE);
    fineTuneModeCombo->addItem(uiText(QStringLiteral("fine_tune_dial"), QStringLiteral("Round dial")),
                               FINE_TUNE_MODE_DIAL);
    const int fineTuneIndex = fineTuneModeCombo->findData(fineTuneControlMode);
    fineTuneModeCombo->setCurrentIndex(fineTuneIndex >= 0 ? fineTuneIndex : 0);

    QComboBox *fftWindowCombo = new QComboBox(&dialog);
    fftWindowCombo->addItem(uiText(QStringLiteral("fft_window_rectangular"), QStringLiteral("Rectangular")),
                            FFT_WINDOW_RECTANGULAR);
    fftWindowCombo->addItem(uiText(QStringLiteral("fft_window_hann"), QStringLiteral("Hann")),
                            FFT_WINDOW_HANN);
    fftWindowCombo->addItem(uiText(QStringLiteral("fft_window_hamming"), QStringLiteral("Hamming")),
                            FFT_WINDOW_HAMMING);
    fftWindowCombo->addItem(uiText(QStringLiteral("fft_window_blackman_harris"), QStringLiteral("Blackman-Harris")),
                            FFT_WINDOW_BLACKMAN_HARRIS);
    fftWindowCombo->addItem(uiText(QStringLiteral("fft_window_flat_top"), QStringLiteral("Flat-top")),
                            FFT_WINDOW_FLAT_TOP);
    const int fftWindowIndex = fftWindowCombo->findData(
        normalizedFftWindowType(pendingSettings.fftWindowType));
    fftWindowCombo->setCurrentIndex(fftWindowIndex >= 0 ? fftWindowIndex : 0);
    fftWindowCombo->setToolTip(uiText(
        QStringLiteral("fft_window_tooltip"),
        QStringLiteral("FFT window applied before the transform. Hann is a good general-purpose choice; Blackman-Harris suppresses leakage near strong signals; Flat-top improves amplitude accuracy but widens RBW.")));

    QComboBox *fftBackendCombo = new QComboBox(&dialog);
    fftBackendCombo->addItem(uiText(QStringLiteral("fft_backend_auto"),
                                     QStringLiteral("Auto (benchmark CPU/GPU)")),
                             FFT_BACKEND_AUTO);
    fftBackendCombo->addItem(uiText(QStringLiteral("fft_backend_cpu"),
                                     QStringLiteral("CPU FFTW")),
                             FFT_BACKEND_CPU_FFTW);
    fftBackendCombo->addItem(uiText(QStringLiteral("fft_backend_gpu"),
                                     QStringLiteral("GPU VkFFT (experimental)")),
                             FFT_BACKEND_GPU_VKFFT);
    const int fftBackendIndex = fftBackendCombo->findData(
        normalizedFftBackendPreference(fftBackendPreference));
    fftBackendCombo->setCurrentIndex(fftBackendIndex >= 0 ? fftBackendIndex : 0);
    fftBackendCombo->setToolTip(uiText(
        QStringLiteral("fft_backend_tooltip"),
        QStringLiteral("Auto benchmarks FFTW and Vulkan VkFFT for large transforms and keeps the faster backend. Every GPU error falls back to FFTW without stopping reception.")));
#ifndef FOBOSAPP_HAS_VKFFT
    fftBackendCombo->setItemText(
        fftBackendCombo->findData(FFT_BACKEND_GPU_VKFFT),
        uiText(QStringLiteral("fft_backend_gpu_unavailable"),
               QStringLiteral("GPU VkFFT (not included in this build)")));
#endif

    QSpinBox *spectrumUpdateSpin = new QSpinBox(&dialog);
    spectrumUpdateSpin->setRange(SPECTRUM_UPDATE_AUTO_MS, SPECTRUM_UPDATE_MAX_MS);
    spectrumUpdateSpin->setSpecialValueText(uiText(QStringLiteral("auto"), QStringLiteral("Auto")));
    spectrumUpdateSpin->setSuffix(QStringLiteral(" ms"));
    spectrumUpdateSpin->setSingleStep(1);
    spectrumUpdateSpin->setValue(spectrumUpdateIntervalMs);
    spectrumUpdateSpin->setToolTip(uiText(
        QStringLiteral("spectrum_update_interval_tooltip"),
        QStringLiteral("Spectrum and waterfall update interval. Auto keeps the FFT-dependent default.")));

    QSpinBox *waterfallRowsSpin = new QSpinBox(&dialog);
    waterfallRowsSpin->setRange(WATERFALL_ROWS_PER_FRAME_MIN, WATERFALL_ROWS_PER_FRAME_MAX);
    waterfallRowsSpin->setSuffix(QStringLiteral(" rows/frame"));
    waterfallRowsSpin->setSingleStep(1);
    waterfallRowsSpin->setValue((std::clamp)(waterfallRowsPerFrame,
                                             WATERFALL_ROWS_PER_FRAME_MIN,
                                             WATERFALL_ROWS_PER_FRAME_MAX));
    waterfallRowsSpin->setToolTip(uiText(
        QStringLiteral("waterfall_speed_tooltip"),
        QStringLiteral("Visual waterfall scroll speed. Higher values move more rows per FFT frame without increasing FFT load.")));

    QSpinBox *scanMeasurementUpdateSpin = new QSpinBox(&dialog);
    scanMeasurementUpdateSpin->setRange(SCAN_MEASUREMENT_MIN_UPDATE_MS,
                                        SCAN_MEASUREMENT_MAX_UPDATE_MS);
    scanMeasurementUpdateSpin->setSuffix(QStringLiteral(" ms"));
    scanMeasurementUpdateSpin->setSingleStep(20);
    scanMeasurementUpdateSpin->setValue((std::clamp)(scanMeasurementUpdateIntervalMs,
                                                     SCAN_MEASUREMENT_MIN_UPDATE_MS,
                                                     SCAN_MEASUREMENT_MAX_UPDATE_MS));
    scanMeasurementUpdateSpin->setToolTip(uiText(
        QStringLiteral("scan_measurement_update_interval_tooltip"),
        QStringLiteral("How often the spectrum measurement overlay accumulates bins. Higher values reduce CPU/UI load and keep the waterfall smoother.")));

    QSpinBox *agileLiveRetuneIntervalSpin = new QSpinBox(&dialog);
    agileLiveRetuneIntervalSpin->setRange(AGILE_LIVE_RETUNE_MIN_COMMAND_INTERVAL_MS,
                                          AGILE_LIVE_RETUNE_MAX_COMMAND_INTERVAL_MS);
    agileLiveRetuneIntervalSpin->setSuffix(QStringLiteral(" ms"));
    agileLiveRetuneIntervalSpin->setSingleStep(20);
    agileLiveRetuneIntervalSpin->setValue((std::clamp)(agileLiveRetuneCommandIntervalMs,
                                                       AGILE_LIVE_RETUNE_MIN_COMMAND_INTERVAL_MS,
                                                       AGILE_LIVE_RETUNE_MAX_COMMAND_INTERVAL_MS));
    agileLiveRetuneIntervalSpin->setToolTip(uiText(
        QStringLiteral("agile_live_retune_interval_tooltip"),
        QStringLiteral("Minimum interval between live Agile center-frequency commands. Lower values make tuning more responsive but can overload USB and UI.")));

    QPushButton *helpButton = new QPushButton(
        uiText(QStringLiteral("program_help"), QStringLiteral("Feature guide...")),
        &dialog);
    helpButton->setToolTip(uiText(
        QStringLiteral("program_help_tooltip"),
        QStringLiteral("Open a practical guide to Obrii SDR controls, scanning, recordings, GNSS/QTH, network mode and mouse shortcuts.")));

    QPushButton *fobosDetailsButton = new QPushButton(
        uiText(QStringLiteral("show_fobos_details"), QStringLiteral("Show Fobos Details")),
        &dialog);
    fobosDetailsButton->setToolTip(uiText(
        QStringLiteral("show_fobos_details_tooltip"),
        QStringLiteral("Show Standard and Agile Fobos API versions and the currently detected receiver list.")));

    generalLayout->addRow(uiText(QStringLiteral("language"), QStringLiteral("Lang:")), languageCombo);
    generalLayout->addRow(uiText(QStringLiteral("fine_tune"), QStringLiteral("Fine tune")), fineTuneModeCombo);
    generalLayout->addRow(uiText(QStringLiteral("fft_window"), QStringLiteral("FFT window")), fftWindowCombo);
    generalLayout->addRow(uiText(QStringLiteral("fft_backend"), QStringLiteral("FFT backend")), fftBackendCombo);
    generalLayout->addRow(uiText(QStringLiteral("spectrum_update_interval"), QStringLiteral("Spectrum/waterfall update")), spectrumUpdateSpin);
    generalLayout->addRow(uiText(QStringLiteral("waterfall_speed"), QStringLiteral("Waterfall speed")), waterfallRowsSpin);
    generalLayout->addRow(uiText(QStringLiteral("scan_measurement_update_interval"), QStringLiteral("Measurement accumulation")), scanMeasurementUpdateSpin);
    generalLayout->addRow(uiText(QStringLiteral("agile_live_retune_interval"), QStringLiteral("Agile live retune interval")), agileLiveRetuneIntervalSpin);
    generalLayout->addRow(helpButton);
    generalLayout->addRow(fobosDetailsButton);
    auto *publicSourcesButton = new QPushButton(uiText("public_data_sources", "Online data sources..."), &dialog);
    generalLayout->addRow(publicSourcesButton);
    connect(publicSourcesButton, &QPushButton::clicked, &dialog, [this, &dialog] {
        spaceevents::configureSources(&dialog, normalizedUiLanguage(uiLanguage) == QStringLiteral("uk"));
    });
    rootLayout->addLayout(generalLayout);

    QGroupBox *calibrationBox = new QGroupBox(
        uiText(QStringLiteral("receiver_calibration"), QStringLiteral("Receiver calibration")),
        &dialog);
    QFormLayout *calibrationLayout = new QFormLayout(calibrationBox);
    QDoubleSpinBox *frequencyCalibrationSpin = new QDoubleSpinBox(calibrationBox);
    frequencyCalibrationSpin->setRange(-10000000.0, 10000000.0);
    frequencyCalibrationSpin->setDecimals(3);
    frequencyCalibrationSpin->setSingleStep(1.0);
    frequencyCalibrationSpin->setSuffix(QStringLiteral(" Hz"));
    frequencyCalibrationSpin->setKeyboardTracking(false);
    frequencyCalibrationSpin->setValue(frequencyCalibrationOffsetHz);
    frequencyCalibrationSpin->setToolTip(uiText(
        QStringLiteral("frequency_calibration_offset_tooltip"),
        QStringLiteral("Global correction added to receiver hardware tuning while displayed frequency remains unchanged. Frequency-dependent table corrections from Presets are added to this value.")));

    QDoubleSpinBox *amplitudeCalibrationSpin = new QDoubleSpinBox(calibrationBox);
    amplitudeCalibrationSpin->setRange(-200.0, 200.0);
    amplitudeCalibrationSpin->setDecimals(2);
    amplitudeCalibrationSpin->setSingleStep(0.5);
    amplitudeCalibrationSpin->setSuffix(QStringLiteral(" dB"));
    amplitudeCalibrationSpin->setKeyboardTracking(false);
    amplitudeCalibrationSpin->setValue(amplitudeCalibrationOffsetDb);
    amplitudeCalibrationSpin->setToolTip(uiText(
        QStringLiteral("amplitude_calibration_offset_tooltip"),
        QStringLiteral("Global correction added to displayed spectrum levels and measurements. Frequency-dependent table corrections from Presets are added to this value; IQ samples and audio gain are unchanged.")));

    QPushButton *resetCalibrationButton = new QPushButton(
        uiText(QStringLiteral("reset_calibration"), QStringLiteral("Reset calibration")),
        calibrationBox);
    calibrationLayout->addRow(
        uiText(QStringLiteral("frequency_calibration_offset"), QStringLiteral("Frequency offset")),
        frequencyCalibrationSpin);
    calibrationLayout->addRow(
        uiText(QStringLiteral("amplitude_calibration_offset"), QStringLiteral("Amplitude offset")),
        amplitudeCalibrationSpin);
    calibrationLayout->addRow(resetCalibrationButton);
    rootLayout->addWidget(calibrationBox);

    QGroupBox *settingsBackupBox = new QGroupBox(
        uiText(QStringLiteral("settings_backup"), QStringLiteral("Settings backup")),
        &dialog);
    QVBoxLayout *settingsBackupLayout = new QVBoxLayout(settingsBackupBox);
    QLabel *settingsBackupHint = new QLabel(
        uiText(QStringLiteral("settings_backup_hint"),
               QStringLiteral("Settings are stored in your user profile and survive application updates. Export ObriiSDR.ini for backup or transfer to another computer.")),
        settingsBackupBox);
    settingsBackupHint->setWordWrap(true);
    QHBoxLayout *settingsBackupButtons = new QHBoxLayout();
    QPushButton *exportSettingsButton = new QPushButton(
        uiText(QStringLiteral("export_settings"), QStringLiteral("Export settings...")),
        settingsBackupBox);
    QPushButton *importSettingsButton = new QPushButton(
        uiText(QStringLiteral("import_settings"), QStringLiteral("Import settings...")),
        settingsBackupBox);
    settingsBackupButtons->addWidget(exportSettingsButton);
    settingsBackupButtons->addWidget(importSettingsButton);
    settingsBackupButtons->addStretch(1);
    settingsBackupLayout->addWidget(settingsBackupHint);
    settingsBackupLayout->addLayout(settingsBackupButtons);
    rootLayout->addWidget(settingsBackupBox);

    QGroupBox *quickOptionsBox = new QGroupBox(uiText(QStringLiteral("quick_options"), QStringLiteral("Quick options")), &dialog);
    QGridLayout *quickOptionsLayout = new QGridLayout(quickOptionsBox);
    QCheckBox *audioOption = new QCheckBox(uiText(QStringLiteral("audio"), QStringLiteral("Audio")), quickOptionsBox);
    QCheckBox *syncOption = new QCheckBox(uiText(QStringLiteral("sync"), QStringLiteral("Sync")), quickOptionsBox);
    QCheckBox *spectrum2Option = new QCheckBox(uiText(QStringLiteral("spectrum2"), QStringLiteral("Spectr 2")), quickOptionsBox);
    QCheckBox *colorOption = new QCheckBox(uiText(QStringLiteral("colorful"), QStringLiteral("Color spectrum")), quickOptionsBox);
    QCheckBox *generalBandMarkersOption = new QCheckBox(uiText(QStringLiteral("general_band_markers"), QStringLiteral("Band markers")), quickOptionsBox);
    QCheckBox *amateurBandMarkersOption = new QCheckBox(uiText(QStringLiteral("amateur_band_markers"), QStringLiteral("HAM bands")), quickOptionsBox);
    QCheckBox *compactBandMarkersOption = new QCheckBox(uiText(QStringLiteral("compact_band_markers"), QStringLiteral("Collapsed bands")), quickOptionsBox);
    QCheckBox *gpuWaterfallOption = new QCheckBox(uiText(QStringLiteral("gpu_waterfall"), QStringLiteral("GPU waterfall")), quickOptionsBox);
    gpuWaterfallOption->setToolTip(uiText(
        QStringLiteral("gpu_waterfall_tooltip"),
        QStringLiteral("Experimental: prepare the waterfall for GPU-backed rendering. CPU texture rendering remains the safe fallback.")));
    QComboBox *displayReductionCombo = new QComboBox(quickOptionsBox);
    displayReductionCombo->addItem(uiText(QStringLiteral("display_reduction_peak"),
                                           QStringLiteral("Peak (preserve narrow signals)")), 0);
    displayReductionCombo->addItem(uiText(QStringLiteral("display_reduction_average_db"),
                                           QStringLiteral("Average in dB")), 1);
    displayReductionCombo->addItem(uiText(QStringLiteral("display_reduction_average_power"),
                                           QStringLiteral("Average linear power (RMS)")), 2);
    displayReductionCombo->addItem(uiText(QStringLiteral("display_reduction_sample"),
                                           QStringLiteral("Center sample")), 3);
    displayReductionCombo->addItem(uiText(QStringLiteral("display_reduction_minimum"),
                                           QStringLiteral("Minimum")), 4);
    displayReductionCombo->setToolTip(uiText(
        QStringLiteral("display_reduction_tooltip"),
        QStringLiteral("How groups of FFT bins are reduced to screen pixels. Peak retains short narrow signals; linear power gives the physically meaningful average noise level.")));
    QCheckBox *alternativeInterfaceOption = new QCheckBox(
        uiText(QStringLiteral("alternative_interface"), QStringLiteral("Alternative interface")),
        quickOptionsBox);
    alternativeInterfaceOption->setToolTip(uiText(
        QStringLiteral("alternative_interface_tooltip"),
        QStringLiteral("Use a fixed front-facing 3D waterfall with the live spectrum overlaid at its near edge.")));
    QCheckBox *gnssUbxAutoEnableOption = new QCheckBox(uiText(QStringLiteral("gnss_ubx_auto_enable"),
                                                              QStringLiteral("Auto-enable UBX")),
                                                       quickOptionsBox);
    gnssUbxAutoEnableOption->setToolTip(uiText(
        QStringLiteral("gnss_ubx_auto_enable_tooltip"),
        QStringLiteral("After opening a GNSS serial port, automatically request u-blox NAV-PVT, NAV-SAT and NAV-DOP output.")));
    QCheckBox *loggingOption = new QCheckBox(uiText(QStringLiteral("logging"), QString::fromUtf8("Р›РѕРіСѓРІР°РЅРЅСЏ")), quickOptionsBox);
    loggingOption->setToolTip(uiText(QStringLiteral("logging_tooltip"),
                                     QStringLiteral("Write detailed diagnostic logs and DMR dumps")));
    QCheckBox *spectrumFpsOption = new QCheckBox(
        uiText(QStringLiteral("spectrum_fps_overlay"), QStringLiteral("Spectrum FPS")),
        quickOptionsBox);
    spectrumFpsOption->setToolTip(uiText(
        QStringLiteral("spectrum_fps_overlay_tooltip"),
        QStringLiteral("Show the actual rendered frame rate over the spectrum window.")));
    QCheckBox *waterfallFpsOption = new QCheckBox(
        uiText(QStringLiteral("waterfall_fps_overlay"), QStringLiteral("Waterfall FPS")),
        quickOptionsBox);
    waterfallFpsOption->setToolTip(uiText(
        QStringLiteral("waterfall_fps_overlay_tooltip"),
        QStringLiteral("Show the actual rendered frame rate over the 2D or 3D waterfall window.")));
    QCheckBox *audioQueueDropRowsOption = new QCheckBox(
        uiText(QStringLiteral("audio_queue_drop_rows"), QStringLiteral("Waterfall diagnostic rows")),
        quickOptionsBox);
    audioQueueDropRowsOption->setToolTip(uiText(
        QStringLiteral("audio_queue_drop_rows_tooltip"),
        QStringLiteral("Enable red rows for audio-queue overflow and gray rows for skipped FFT snapshots. Neither proves a USB loss. Off by default.")));
    audioQueueDropRowsOption->setChecked(showAudioQueueDropRows);
    QCheckBox *spectrumPeakMeterOption = new QCheckBox(
        uiText(QStringLiteral("spectrum_peak_meter"), QStringLiteral("Analog peak meter")),
        quickOptionsBox);
    spectrumPeakMeterOption->setToolTip(uiText(
        QStringLiteral("spectrum_peak_meter_tooltip"),
        QStringLiteral("Show an analog needle meter for the active spectrum marker, or the strongest visible peak when no marker is selected.")));
    QComboBox *spectrumPeakMeterStyleCombo = new QComboBox(quickOptionsBox);
    spectrumPeakMeterStyleCombo->addItem(
        uiText(QStringLiteral("spectrum_peak_meter_transparent"), QStringLiteral("Transparent")), 0);
    spectrumPeakMeterStyleCombo->addItem(
        uiText(QStringLiteral("spectrum_peak_meter_retro"), QStringLiteral("Amber retro backlight")), 1);
    spectrumPeakMeterStyleCombo->setToolTip(uiText(
        QStringLiteral("spectrum_peak_meter_style_tooltip"),
        QStringLiteral("Choose a transparent instrument overlay or an amber backlit retro scale.")));
    QCheckBox *extendedSpectrumInfoOption = new QCheckBox(
        uiText(QStringLiteral("extended_spectrum_info"), QStringLiteral("Extended spectrum info")),
        quickOptionsBox);
    extendedSpectrumInfoOption->setToolTip(uiText(
        QStringLiteral("extended_spectrum_info_tooltip"),
        QStringLiteral("Show detailed frequency, level, sample-rate, FFT-window, RBW and bin-width information under the FPS overlays.")));
    QCheckBox *extendedRecordingMetadataOption = new QCheckBox(
        uiText(QStringLiteral("extended_recording_metadata"), QStringLiteral("Extended recording metadata")),
        quickOptionsBox);
    extendedRecordingMetadataOption->setToolTip(uiText(
        QStringLiteral("extended_recording_metadata_tooltip"),
        QStringLiteral("Add versioned scientific metadata, calibration, FFT/RBW, spectrum markers and measurements to recording metadata. Location, device serials, keys and API tokens are excluded.")));
    QCheckBox *simplifiedAudioChannelizerOption = new QCheckBox(
        uiText(QStringLiteral("simplified_audio_channelizer"),
               QStringLiteral("Simplified operating mode")),
        quickOptionsBox);
    simplifiedAudioChannelizerOption->setToolTip(uiText(
        QStringLiteral("simplified_audio_channelizer_tooltip"),
        QStringLiteral("Use the lower-cost existing audio path and disable optional DSP branch/stream-manager work (DDC and network research branches). Main spectrum/waterfall, audio, DMR, ordinary filters and demodulators remain available. The server is started explicitly by the user on the compute laptop, independently of this mode. No speed or real-time guarantee.")));
    audioOption->setChecked(audioCheckbox && audioCheckbox->isChecked());
    syncOption->setChecked(syncCheckbox && syncCheckbox->isChecked());
    syncOption->setEnabled(false);
    syncOption->setToolTip(syncCheckbox ? syncCheckbox->toolTip() : QString());
    spectrum2Option->setChecked(graphCheckbox && graphCheckbox->isChecked());
    colorOption->setChecked(colorCheckbox && colorCheckbox->isChecked());
    generalBandMarkersOption->setChecked(showGeneralBandMarkers);
    amateurBandMarkersOption->setChecked(showAmateurBandMarkers);
    compactBandMarkersOption->setChecked(compactBandMarkers);
    gpuWaterfallOption->setChecked(experimentalGpuWaterfall);
    displayReductionCombo->setCurrentIndex((std::max)(0, displayReductionCombo->findData(spectrumDisplayReductionMode)));
    alternativeInterfaceOption->setChecked(alternativeInterfaceMode);
    gnssUbxAutoEnableOption->setChecked(gnssUbxAutoEnable);
    loggingOption->setChecked(diagnosticVerboseLogging);
    spectrumFpsOption->setChecked(showSpectrumFps);
    waterfallFpsOption->setChecked(showWaterfallFps);
    spectrumPeakMeterOption->setChecked(showSpectrumPeakMeter);
    spectrumPeakMeterStyleCombo->setCurrentIndex((std::max)(0, spectrumPeakMeterStyleCombo->findData(spectrumPeakMeterStyle)));
    extendedSpectrumInfoOption->setChecked(showExtendedSpectrumInfo);
    extendedRecordingMetadataOption->setChecked(extendedRecordingMetadataEnabled);
    simplifiedAudioChannelizerOption->setChecked(pendingSettings.simplifiedAudioChannelizer);
    quickOptionsLayout->addWidget(audioOption, 0, 0);
    quickOptionsLayout->addWidget(syncOption, 0, 1);
    quickOptionsLayout->addWidget(spectrum2Option, 1, 0);
    quickOptionsLayout->addWidget(colorOption, 1, 1);
    quickOptionsLayout->addWidget(generalBandMarkersOption, 2, 0);
    quickOptionsLayout->addWidget(amateurBandMarkersOption, 2, 1);
    quickOptionsLayout->addWidget(compactBandMarkersOption, 2, 2);
    quickOptionsLayout->addWidget(gpuWaterfallOption, 3, 0);
    quickOptionsLayout->addWidget(gnssUbxAutoEnableOption, 3, 1);
    quickOptionsLayout->addWidget(loggingOption, 3, 2);
    quickOptionsLayout->addWidget(spectrumFpsOption, 4, 0);
    quickOptionsLayout->addWidget(waterfallFpsOption, 4, 1);
    quickOptionsLayout->addWidget(extendedSpectrumInfoOption, 4, 2);
    quickOptionsLayout->addWidget(spectrumPeakMeterOption, 5, 0);
    quickOptionsLayout->addWidget(spectrumPeakMeterStyleCombo, 5, 1, 1, 2);
    quickOptionsLayout->addWidget(simplifiedAudioChannelizerOption, 6, 0, 1, 3);
    quickOptionsLayout->addWidget(extendedRecordingMetadataOption, 7, 0, 1, 3);
    quickOptionsLayout->addWidget(alternativeInterfaceOption, 8, 0, 1, 3);
    quickOptionsLayout->addWidget(new QLabel(uiText(QStringLiteral("display_reduction"),
                                                     QStringLiteral("FFT display reduction:")),
                                             quickOptionsBox), 9, 0);
    quickOptionsLayout->addWidget(displayReductionCombo, 9, 1, 1, 2);
    quickOptionsLayout->addWidget(audioQueueDropRowsOption, 10, 0, 1, 3);
    rootLayout->addWidget(quickOptionsBox);

    const bool ukrainian = normalizedUiLanguage(uiLanguage) == QStringLiteral("uk");
    QGroupBox *layoutProfilesBox = new QGroupBox(
        ukrainian ? QStringLiteral("Профілі інтерфейсу") : QStringLiteral("Interface profiles"),
        &dialog);
    QVBoxLayout *layoutProfilesLayout = new QVBoxLayout(layoutProfilesBox);
    QLabel *layoutProfilesHint = new QLabel(
        ukrainian
            ? QStringLiteral("Зберігає розмір і компонування вікна, док-панелі, розгорнуті розділи та основний режим відображення. Налаштування приймача й калібрування не змінюються.")
            : QStringLiteral("Stores window geometry, dock panels, expanded sections and the primary display mode. Receiver and calibration settings are not changed."),
        layoutProfilesBox);
    layoutProfilesHint->setWordWrap(true);
    QHBoxLayout *layoutProfilesControls = new QHBoxLayout();
    QComboBox *layoutProfileCombo = new QComboBox(layoutProfilesBox);
    QPushButton *saveLayoutProfileButton = new QPushButton(
        ukrainian ? QStringLiteral("Зберегти поточний") : QStringLiteral("Save current"),
        layoutProfilesBox);
    QPushButton *loadLayoutProfileButton = new QPushButton(
        ukrainian ? QStringLiteral("Завантажити") : QStringLiteral("Load"),
        layoutProfilesBox);
    QPushButton *deleteLayoutProfileButton = new QPushButton(
        ukrainian ? QStringLiteral("Видалити") : QStringLiteral("Delete"),
        layoutProfilesBox);
    layoutProfilesControls->addWidget(layoutProfileCombo, 1);
    layoutProfilesControls->addWidget(saveLayoutProfileButton);
    layoutProfilesControls->addWidget(loadLayoutProfileButton);
    layoutProfilesControls->addWidget(deleteLayoutProfileButton);
    layoutProfilesLayout->addWidget(layoutProfilesHint);
    layoutProfilesLayout->addLayout(layoutProfilesControls);
    rootLayout->addWidget(layoutProfilesBox);

    auto populateLayoutProfiles = [layoutProfileCombo,
                                   loadLayoutProfileButton,
                                   deleteLayoutProfileButton,
                                   ukrainian](const QString &selectedName = QString()) {
        const QSignalBlocker blocker(layoutProfileCombo);
        layoutProfileCombo->clear();
        const QVector<InterfaceLayoutProfile> profiles = loadInterfaceLayoutProfiles();
        for (const auto &profile : profiles) layoutProfileCombo->addItem(profile.name, profile.name);
        const int selectedIndex = layoutProfileCombo->findData(selectedName);
        if (selectedIndex >= 0) layoutProfileCombo->setCurrentIndex(selectedIndex);
        const bool available = layoutProfileCombo->count() > 0;
        loadLayoutProfileButton->setEnabled(available);
        deleteLayoutProfileButton->setEnabled(available);
        layoutProfileCombo->setPlaceholderText(
            ukrainian ? QStringLiteral("Немає збережених профілів")
                      : QStringLiteral("No saved profiles"));
    };
    populateLayoutProfiles();

    auto applyLanguage = [this, languageCombo]() {
        const QString nextLanguage = languageCombo->currentData().toString();
        uiLanguage = normalizedUiLanguage(nextLanguage);
        applyUiLanguage();
        savePersistentSettings();
    };
    auto applyFineTuneMode = [this, fineTuneModeCombo]() {
        fineTuneControlMode = fineTuneModeCombo->currentData().toInt();
        if (fineTuneControlMode != FINE_TUNE_MODE_DIAL) {
            fineTuneControlMode = FINE_TUNE_MODE_SCALE;
        }
        updateFineTuneControlMode();
        savePersistentSettings();
    };
    auto applyFftWindow = [this, fftWindowCombo]() {
        pendingSettings.fftWindowType = normalizedFftWindowType(
            fftWindowCombo->currentData().toInt());
        savePersistentSettings();
    };
    auto applyFftBackend = [this, fftBackendCombo]() {
        fftBackendPreference = normalizedFftBackendPreference(
            fftBackendCombo->currentData().toInt());
        savePersistentSettings();
    };
    auto applySpectrumUpdateInterval = [this, spectrumUpdateSpin]() {
        int value = spectrumUpdateSpin->value();
        if (value > SPECTRUM_UPDATE_AUTO_MS && value < SPECTRUM_UPDATE_MIN_MS) {
            value = SPECTRUM_UPDATE_MIN_MS;
            QSignalBlocker blocker(spectrumUpdateSpin);
            spectrumUpdateSpin->setValue(value);
        }
        spectrumUpdateIntervalMs = value;
        updateSpectrumTimerInterval();
        if (wirelessLabDialog) wirelessLabDialog->setSpectrumUpdateInterval(value);
        savePersistentSettings();
    };
    auto applyWaterfallRowsPerFrame = [this, waterfallRowsSpin]() {
        waterfallRowsPerFrame = (std::clamp)(waterfallRowsSpin->value(),
                                             WATERFALL_ROWS_PER_FRAME_MIN,
                                             WATERFALL_ROWS_PER_FRAME_MAX);
        if (waterfallWidget) {
            waterfallWidget->setRowsPerFrame(waterfallRowsPerFrame);
        }
        savePersistentSettings();
    };
    auto applyScanMeasurementUpdateInterval = [this, scanMeasurementUpdateSpin]() {
        scanMeasurementUpdateIntervalMs =
            (std::clamp)(scanMeasurementUpdateSpin->value(),
                         SCAN_MEASUREMENT_MIN_UPDATE_MS,
                         SCAN_MEASUREMENT_MAX_UPDATE_MS);
        scanMeasurementUpdateClock.invalidate();
        scanMeasurementOverlayClock.invalidate();
        scanMeasurementOverlayCache.clear();
        savePersistentSettings();
    };
    auto applyAgileLiveRetuneInterval = [this, agileLiveRetuneIntervalSpin]() {
        agileLiveRetuneCommandIntervalMs =
            (std::clamp)(agileLiveRetuneIntervalSpin->value(),
                         AGILE_LIVE_RETUNE_MIN_COMMAND_INTERVAL_MS,
                         AGILE_LIVE_RETUNE_MAX_COMMAND_INTERVAL_MS);
        savePersistentSettings();
        qDebug() << "[LiveTune] Agile live retune command interval"
                 << agileLiveRetuneCommandIntervalMs;
    };
    auto applyFrequencyCalibration = [this, frequencyCalibrationSpin]() {
        frequencyCalibrationOffsetHz = frequencyCalibrationSpin->value();
        applyReceiverCalibrationLive();
        savePersistentSettings();
    };
    auto applyAmplitudeCalibration = [this, amplitudeCalibrationSpin]() {
        amplitudeCalibrationOffsetDb = amplitudeCalibrationSpin->value();
        savePersistentSettings();
    };

    connect(languageCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog, [applyLanguage](int) {
        applyLanguage();
    });
    connect(fineTuneModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog, [applyFineTuneMode](int) {
        applyFineTuneMode();
    });
    connect(fftWindowCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog, [applyFftWindow](int) {
        applyFftWindow();
    });
    connect(fftBackendCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog, [applyFftBackend](int) {
        applyFftBackend();
    });
    connect(spectrumUpdateSpin, QOverload<int>::of(&QSpinBox::valueChanged), &dialog, [applySpectrumUpdateInterval](int) {
        applySpectrumUpdateInterval();
    });
    connect(waterfallRowsSpin, QOverload<int>::of(&QSpinBox::valueChanged), &dialog, [applyWaterfallRowsPerFrame](int) {
        applyWaterfallRowsPerFrame();
    });
    connect(scanMeasurementUpdateSpin, QOverload<int>::of(&QSpinBox::valueChanged), &dialog, [applyScanMeasurementUpdateInterval](int) {
        applyScanMeasurementUpdateInterval();
    });
    connect(agileLiveRetuneIntervalSpin, QOverload<int>::of(&QSpinBox::valueChanged), &dialog, [applyAgileLiveRetuneInterval](int) {
        applyAgileLiveRetuneInterval();
    });
    connect(frequencyCalibrationSpin, &QDoubleSpinBox::editingFinished, &dialog, applyFrequencyCalibration);
    connect(amplitudeCalibrationSpin, &QDoubleSpinBox::editingFinished, &dialog, applyAmplitudeCalibration);
    connect(resetCalibrationButton, &QPushButton::clicked, &dialog,
            [frequencyCalibrationSpin,
             amplitudeCalibrationSpin,
             applyFrequencyCalibration,
             applyAmplitudeCalibration]() {
                frequencyCalibrationSpin->setValue(0.0);
                amplitudeCalibrationSpin->setValue(0.0);
                applyFrequencyCalibration();
                applyAmplitudeCalibration();
            });
    connect(helpButton, &QPushButton::clicked, &dialog, [this]() {
        openApplicationHelp();
    });
    connect(fobosDetailsButton, &QPushButton::clicked, &dialog, [this]() {
        listFobosDevices();
    });
    connect(exportSettingsButton, &QPushButton::clicked, &dialog, [this]() {
        exportSettingsBackup();
    });
    connect(importSettingsButton, &QPushButton::clicked, &dialog, [this]() {
        importSettingsBackup();
    });
    connect(saveLayoutProfileButton, &QPushButton::clicked, &dialog,
            [this, &dialog, layoutProfileCombo, populateLayoutProfiles, ukrainian]() {
                bool accepted = false;
                const QString name = QInputDialog::getText(
                    &dialog,
                    ukrainian ? QStringLiteral("Зберегти профіль інтерфейсу")
                              : QStringLiteral("Save interface profile"),
                    ukrainian ? QStringLiteral("Назва профілю:") : QStringLiteral("Profile name:"),
                    QLineEdit::Normal,
                    layoutProfileCombo->currentData().toString(),
                    &accepted).trimmed();
                if (!accepted || name.isEmpty()) return;

                QVector<InterfaceLayoutProfile> profiles = loadInterfaceLayoutProfiles();
                auto existing = std::find_if(profiles.begin(), profiles.end(), [&name](const auto &profile) {
                    return profile.name.compare(name, Qt::CaseInsensitive) == 0;
                });
                if (existing != profiles.end()) {
                    const auto answer = QMessageBox::question(
                        &dialog,
                        ukrainian ? QStringLiteral("Замінити профіль?") : QStringLiteral("Replace profile?"),
                        ukrainian
                            ? QStringLiteral("Профіль з такою назвою вже існує. Замінити його поточним компонуванням?")
                            : QStringLiteral("A profile with this name already exists. Replace it with the current layout?"));
                    if (answer != QMessageBox::Yes) return;
                } else {
                    profiles.push_back({});
                    existing = profiles.end() - 1;
                }

                QJsonObject sections;
                const QString prefix = QStringLiteral("uiSectionHeader_");
                for (QToolButton *header : findChildren<QToolButton*>()) {
                    if (header->objectName().startsWith(prefix)) {
                        sections.insert(header->objectName().mid(prefix.size()), header->isChecked());
                    }
                }
                QJsonObject view;
                view.insert(QStringLiteral("sections"), sections);
                view.insert(QStringLiteral("alternativeInterface"), alternativeInterfaceMode);
                view.insert(QStringLiteral("secondSpectrum"), graphCheckbox && graphCheckbox->isChecked());
                view.insert(QStringLiteral("colorSpectrum"), colorCheckbox && colorCheckbox->isChecked());
                view.insert(QStringLiteral("waterfallDisplayMode"), waterfallDisplayMode);
                view.insert(QStringLiteral("waterfall3DFixedPlane"), waterfall3DFixedPlane);
                view.insert(QStringLiteral("waterfall3DAlternativeView"), waterfall3DAlternativeView);
                view.insert(QStringLiteral("controlsVisible"), controlsDock && controlsDock->isVisible());
                view.insert(QStringLiteral("digitalVisible"), digitalDock && digitalDock->isVisible());
                view.insert(QStringLiteral("videoVisible"), videoDock && videoDock->isVisible());
                existing->name = name;
                existing->geometry = saveGeometry();
                existing->windowState = saveState(1);
                existing->view = view;
                storeInterfaceLayoutProfiles(profiles);
                populateLayoutProfiles(name);
            });
    connect(loadLayoutProfileButton, &QPushButton::clicked, &dialog,
            [this, layoutProfileCombo, alternativeInterfaceOption]() {
                const QString name = layoutProfileCombo->currentData().toString();
                const QVector<InterfaceLayoutProfile> profiles = loadInterfaceLayoutProfiles();
                const auto profile = std::find_if(profiles.cbegin(), profiles.cend(), [&name](const auto &item) {
                    return item.name == name;
                });
                if (profile == profiles.cend()) return;

                if (!profile->geometry.isEmpty()) restoreGeometry(profile->geometry);
                if (!profile->windowState.isEmpty()) restoreState(profile->windowState, 1);
                const QJsonObject view = profile->view;
                const QJsonObject sections = view.value(QStringLiteral("sections")).toObject();
                const QString prefix = QStringLiteral("uiSectionHeader_");
                for (QToolButton *header : findChildren<QToolButton*>()) {
                    if (!header->objectName().startsWith(prefix)) continue;
                    const QString key = header->objectName().mid(prefix.size());
                    if (sections.contains(key)) header->setChecked(sections.value(key).toBool());
                }
                if (graphCheckbox && view.contains(QStringLiteral("secondSpectrum")))
                    graphCheckbox->setChecked(view.value(QStringLiteral("secondSpectrum")).toBool());
                if (colorCheckbox && view.contains(QStringLiteral("colorSpectrum")))
                    colorCheckbox->setChecked(view.value(QStringLiteral("colorSpectrum")).toBool());
                if (waterfallDisplayModeCombo && view.contains(QStringLiteral("waterfallDisplayMode"))) {
                    const int mode = view.value(QStringLiteral("waterfallDisplayMode")).toInt(waterfallDisplayMode);
                    const int index = waterfallDisplayModeCombo->findData(mode);
                    if (index >= 0) waterfallDisplayModeCombo->setCurrentIndex(index);
                }
                if (waterfall3DFixedPlaneCheckbox && view.contains(QStringLiteral("waterfall3DFixedPlane")))
                    waterfall3DFixedPlaneCheckbox->setChecked(
                        view.value(QStringLiteral("waterfall3DFixedPlane")).toBool());
                if (waterfall3DAlternativeViewCheckbox &&
                    view.contains(QStringLiteral("waterfall3DAlternativeView"))) {
                    waterfall3DAlternativeViewCheckbox->setChecked(
                        view.value(QStringLiteral("waterfall3DAlternativeView")).toBool());
                }
                if (view.contains(QStringLiteral("alternativeInterface"))) {
                    const bool oldAlternative =
                        view.value(QStringLiteral("alternativeInterface")).toBool();
                    if (!view.contains(QStringLiteral("waterfall3DAlternativeView"))) {
                        if (waterfall3DAlternativeViewCheckbox)
                            waterfall3DAlternativeViewCheckbox->setChecked(oldAlternative);
                        alternativeInterfaceOption->setChecked(false);
                    } else {
                        alternativeInterfaceOption->setChecked(oldAlternative);
                    }
                }
                if (controlsDock && view.contains(QStringLiteral("controlsVisible")))
                    controlsDock->setVisible(view.value(QStringLiteral("controlsVisible")).toBool());
                if (digitalDock && view.contains(QStringLiteral("digitalVisible")))
                    digitalDock->setVisible(view.value(QStringLiteral("digitalVisible")).toBool());
                if (videoDock && view.contains(QStringLiteral("videoVisible")))
                    videoDock->setVisible(view.value(QStringLiteral("videoVisible")).toBool());
                savePersistentSettings();
            });
    connect(deleteLayoutProfileButton, &QPushButton::clicked, &dialog,
            [&dialog, layoutProfileCombo, populateLayoutProfiles, ukrainian]() {
                const QString name = layoutProfileCombo->currentData().toString();
                if (name.isEmpty()) return;
                const auto answer = QMessageBox::question(
                    &dialog,
                    ukrainian ? QStringLiteral("Видалити профіль?") : QStringLiteral("Delete profile?"),
                    ukrainian ? QStringLiteral("Видалити профіль «%1»?").arg(name)
                              : QStringLiteral("Delete profile '%1'?").arg(name));
                if (answer != QMessageBox::Yes) return;
                QVector<InterfaceLayoutProfile> profiles = loadInterfaceLayoutProfiles();
                profiles.erase(std::remove_if(profiles.begin(), profiles.end(), [&name](const auto &profile) {
                    return profile.name == name;
                }), profiles.end());
                storeInterfaceLayoutProfiles(profiles);
                populateLayoutProfiles();
            });
    connect(audioOption, &QCheckBox::toggled, &dialog, [this](bool checked) {
        if (audioCheckbox) {
            audioCheckbox->setChecked(checked);
        }
        savePersistentSettings();
    });
    connect(spectrum2Option, &QCheckBox::toggled, &dialog, [this](bool checked) {
        if (graphCheckbox) {
            graphCheckbox->setChecked(checked);
        }
        savePersistentSettings();
    });
    connect(colorOption, &QCheckBox::toggled, &dialog, [this](bool checked) {
        if (colorCheckbox) {
            colorCheckbox->setChecked(checked);
        }
        savePersistentSettings();
    });
    connect(generalBandMarkersOption, &QCheckBox::toggled, &dialog, [this](bool checked) {
        showGeneralBandMarkers = checked;
        updateGraphBandMarkers();
        savePersistentSettings();
    });
    connect(amateurBandMarkersOption, &QCheckBox::toggled, &dialog, [this](bool checked) {
        showAmateurBandMarkers = checked;
        updateGraphBandMarkers();
        savePersistentSettings();
    });
    connect(compactBandMarkersOption, &QCheckBox::toggled, &dialog, [this](bool checked) {
        compactBandMarkers = checked;
        updateGraphBandMarkers();
        savePersistentSettings();
    });
    connect(gpuWaterfallOption, &QCheckBox::toggled, &dialog, [this](bool checked) {
        experimentalGpuWaterfall = checked;
        if (waterfallWidget) {
            waterfallWidget->setRenderBackend(checked
                                                  ? MyWaterfallWidget::RenderBackend::GpuPrepared
                                                  : MyWaterfallWidget::RenderBackend::CpuTexture);
        }
        savePersistentSettings();
    });
    connect(displayReductionCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            &dialog,
            [this, displayReductionCombo](int) {
                spectrumDisplayReductionMode = (std::clamp)(displayReductionCombo->currentData().toInt(), 0, 4);
                savePersistentSettings();
            });
    connect(alternativeInterfaceOption, &QCheckBox::toggled, &dialog, [this](bool checked) {
        alternativeInterfaceMode = checked;
        applyAlternativeInterfaceMode();
        savePersistentSettings();
    });
    connect(gnssUbxAutoEnableOption, &QCheckBox::toggled, &dialog, [this](bool checked) {
        gnssUbxAutoEnable = checked;
        savePersistentSettings();
    });
    connect(loggingOption, &QCheckBox::toggled, &dialog, [this](bool checked) {
        diagnosticVerboseLogging = checked;
        setFobosVerboseLoggingEnabled(checked);
        qDebug() << "[Log] Verbose diagnostic logging"
                 << (checked ? "enabled" : "disabled");
        savePersistentSettings();
    });
    connect(spectrumFpsOption, &QCheckBox::toggled, &dialog, [this](bool checked) {
        showSpectrumFps = checked;
        if (graphWidget) {
            graphWidget->setFpsOverlayEnabled(checked);
        }
        savePersistentSettings();
    });
    connect(audioQueueDropRowsOption, &QCheckBox::toggled, &dialog, [this](bool checked) {
        showAudioQueueDropRows = checked;
        savePersistentSettings();
    });
    connect(waterfallFpsOption, &QCheckBox::toggled, &dialog, [this](bool checked) {
        showWaterfallFps = checked;
        if (waterfallWidget) {
            waterfallWidget->setFpsOverlayEnabled(checked);
        }
        savePersistentSettings();
    });
    connect(spectrumPeakMeterOption, &QCheckBox::toggled, &dialog, [this](bool checked) {
        showSpectrumPeakMeter = checked;
        if (graphWidget) graphWidget->setAnalogPeakMeterEnabled(checked);
        if (dspFlowPanel) dspFlowPanel->setAnalogPeakMeterEnabled(checked);
        savePersistentSettings();
    });
    connect(spectrumPeakMeterStyleCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            &dialog, [this, spectrumPeakMeterStyleCombo](int) {
                spectrumPeakMeterStyle = std::clamp(spectrumPeakMeterStyleCombo->currentData().toInt(), 0, 1);
                if (graphWidget) graphWidget->setAnalogPeakMeterStyle(spectrumPeakMeterStyle);
                if (dspFlowPanel) dspFlowPanel->setAnalogPeakMeterStyle(spectrumPeakMeterStyle);
                savePersistentSettings();
            });
    connect(extendedSpectrumInfoOption, &QCheckBox::toggled, &dialog, [this](bool checked) {
        showExtendedSpectrumInfo = checked;
        if (graphWidget) {
            graphWidget->setExtendedInfoOverlayEnabled(checked);
        }
        if (waterfallWidget) {
            waterfallWidget->setExtendedInfoOverlayEnabled(checked);
        }
        savePersistentSettings();
    });
    connect(extendedRecordingMetadataOption, &QCheckBox::toggled, &dialog, [this](bool checked) {
        extendedRecordingMetadataEnabled = checked;
        savePersistentSettings();
    });
    connect(simplifiedAudioChannelizerOption, &QCheckBox::toggled, &dialog, [this](bool checked) {
        pendingSettings.simplifiedAudioChannelizer = checked;
        if (audioProcessor) {
            audioProcessor->configure(audioProcessorSettings());
        }
        savePersistentSettings();
        if (isNetworkClientMode()) {
            scheduleRemoteSettingsCommand();
        } else if (networkMode == NetworkMode::Server) {
            sendServerStateToClients();
        }
    });

    QDialogButtonBox *buttonBox = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    if (QPushButton *closeButton = buttonBox->button(QDialogButtonBox::Close)) {
        closeButton->setText(uiText(QStringLiteral("close"), QStringLiteral("Close")));
    }
    rootLayout->addWidget(buttonBox);

    connect(buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    dialog.exec();
}

void YourClassName::exportSettingsBackup() {
    savePersistentSettings();

    const QString sourcePath = persistentSettingsFilePath();
    QFileInfo sourceInfo(sourcePath);
    if (!sourceInfo.exists()) {
        QMessageBox::warning(this,
                             uiText(QStringLiteral("settings_backup"), QStringLiteral("Settings backup")),
                             uiText(QStringLiteral("settings_export_failed"),
                                    QStringLiteral("Settings export failed: %1"))
                                 .arg(sourcePath));
        return;
    }

    const QString defaultName =
        QStringLiteral("ObriiSDR-settings-%1.ini")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")));
    const QString defaultPath = QDir(sourceInfo.absolutePath()).absoluteFilePath(defaultName);
    const QString targetPath = QFileDialog::getSaveFileName(
        this,
        uiText(QStringLiteral("export_settings"), QStringLiteral("Export settings...")),
        defaultPath,
        uiText(QStringLiteral("settings_ini_filter"), QStringLiteral("INI settings (*.ini);;All files (*.*)")));
    if (targetPath.isEmpty()) {
        return;
    }

    QFile::remove(targetPath);
    if (!QFile::copy(sourcePath, targetPath)) {
        QMessageBox::warning(this,
                             uiText(QStringLiteral("settings_backup"), QStringLiteral("Settings backup")),
                             uiText(QStringLiteral("settings_export_failed"),
                                    QStringLiteral("Settings export failed: %1"))
                                 .arg(targetPath));
        return;
    }

    QMessageBox::information(this,
                             uiText(QStringLiteral("settings_backup"), QStringLiteral("Settings backup")),
                             uiText(QStringLiteral("settings_export_done"),
                                    QStringLiteral("Settings exported: %1"))
                                 .arg(QDir::toNativeSeparators(targetPath)));
}

void YourClassName::importSettingsBackup() {
    const QString currentPath = persistentSettingsFilePath();
    const QString sourcePath = QFileDialog::getOpenFileName(
        this,
        uiText(QStringLiteral("import_settings"), QStringLiteral("Import settings...")),
        QFileInfo(currentPath).absolutePath(),
        uiText(QStringLiteral("settings_ini_filter"), QStringLiteral("INI settings (*.ini);;All files (*.*)")));
    if (sourcePath.isEmpty()) {
        return;
    }

    if (QFileInfo(sourcePath).canonicalFilePath() == QFileInfo(currentPath).canonicalFilePath()) {
        QMessageBox::information(this,
                                 uiText(QStringLiteral("settings_backup"), QStringLiteral("Settings backup")),
                                 uiText(QStringLiteral("settings_import_same_file"),
                                        QStringLiteral("Selected file is already the active settings file.")));
        return;
    }

    const QMessageBox::StandardButton answer =
        QMessageBox::question(this,
                              uiText(QStringLiteral("settings_import_confirm_title"),
                                     QStringLiteral("Import settings?")),
                              uiText(QStringLiteral("settings_import_confirm"),
                                     QStringLiteral("Importing settings will replace the active per-user ObriiSDR.ini. A timestamped backup of the current file will be created first.")),
                              QMessageBox::Yes | QMessageBox::No,
                              QMessageBox::No);
    if (answer != QMessageBox::Yes) {
        return;
    }

    savePersistentSettings();

    const QFileInfo currentInfo(currentPath);
    if (!currentInfo.absoluteDir().exists()) {
        currentInfo.absoluteDir().mkpath(QStringLiteral("."));
    }

    QString backupPath;
    if (QFileInfo::exists(currentPath)) {
        backupPath = QDir(currentInfo.absolutePath()).absoluteFilePath(
            QStringLiteral("ObriiSDR-settings-before-import-%1.ini")
                .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"))));
        if (!QFile::copy(currentPath, backupPath)) {
            QMessageBox::warning(this,
                                 uiText(QStringLiteral("settings_backup"), QStringLiteral("Settings backup")),
                                 uiText(QStringLiteral("settings_backup_failed"),
                                        QStringLiteral("Could not create current settings backup: %1"))
                                     .arg(backupPath));
            return;
        }
    }

    QFile::remove(currentPath);
    if (!QFile::copy(sourcePath, currentPath)) {
        if (!backupPath.isEmpty()) {
            QFile::copy(backupPath, currentPath);
        }
        QMessageBox::warning(this,
                             uiText(QStringLiteral("settings_backup"), QStringLiteral("Settings backup")),
                             uiText(QStringLiteral("settings_import_failed"),
                                    QStringLiteral("Settings import failed: %1"))
                                 .arg(sourcePath));
        return;
    }

    loadPersistentSettings();
    publishSettingsToGlobals();
    updateUiFromPendingSettings();
    settingRange();
    updateGraphBandMarkers();
    updateFrequencyPresetControls();
    updateQthControls();

    QString message = uiText(QStringLiteral("settings_import_done"),
                             QStringLiteral("Settings imported. Backup created: %1"))
                          .arg(backupPath.isEmpty()
                                   ? uiText(QStringLiteral("none"), QStringLiteral("none"))
                                   : QDir::toNativeSeparators(backupPath));
    if (!isIdle()) {
        message += QStringLiteral("\n\n");
        message += uiText(QStringLiteral("settings_import_restart_hint"),
                          QStringLiteral("Some receiver settings are applied fully after Stop/Start or app restart."));
    }
    QMessageBox::information(this,
                             uiText(QStringLiteral("settings_backup"), QStringLiteral("Settings backup")),
                             message);
}

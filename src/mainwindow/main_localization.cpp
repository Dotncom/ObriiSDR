#include "main.h"
#include "multivfowidget.h"
#include "audiofilterchainwidget.h"
#include "dspflowpanel.h"

#include "appconstants.h"
#include "dmrprivacyutils.h"
#include "gnssserialutils.h"
#include "receiverdeviceutils.h"
#include "scanvisualutils.h"
#include "tuningutils.h"

#include <QFile>
#include <QJsonDocument>
#include <QSignalBlocker>
#include <QTableWidget>

QString YourClassName::normalizedUiLanguage(const QString &language) const {
    const QString code = language.trimmed();
    if (!code.isEmpty() && uiTranslations.value(code).isObject()) {
        return code;
    }
    if (uiTranslations.value(QStringLiteral("en")).isObject()) {
        return QStringLiteral("en");
    }
    for (const QString &fallbackCode : uiLanguageOrder) {
        if (uiTranslations.value(fallbackCode).isObject()) {
            return fallbackCode;
        }
    }
    return QStringLiteral("en");
}

void YourClassName::populateLanguageCombo(QComboBox *combo) const {
    if (!combo) {
        return;
    }
    const QSignalBlocker blocker(combo);
    combo->clear();
    for (const QString &code : uiLanguageOrder) {
        combo->addItem(uiLanguageNames.value(code, code.toUpper()), code);
    }
    if (combo->count() == 0) {
        combo->addItem(QStringLiteral("English"), QStringLiteral("en"));
    }
}

QString YourClassName::uiText(const QString &key, const QString &fallback) const {
    const QJsonObject language = uiTranslations.value(normalizedUiLanguage(uiLanguage)).toObject();
    const QString translated = language.value(key).toString();
    if (!translated.isEmpty()) {
        return translated;
    }
    const QString english = uiTranslations.value(QStringLiteral("en")).toObject().value(key).toString();
    return english.isEmpty() ? fallback : english;
}

QString YourClassName::localizedStatusText(const QString &status) const {
    const auto prefixStatus = [this, &status](const QString &prefix,
                                             const QString &key,
                                             const QString &fallback,
                                             const QString &separator) -> QString {
        if (!status.startsWith(prefix)) {
            return QString();
        }
        const QString base = uiText(key, fallback);
        const QString suffix = status.mid(prefix.size());
        return suffix.isEmpty() ? base : QStringLiteral("%1%2%3").arg(base, separator, suffix);
    };

    if (status == QStringLiteral("Recording: idle")) {
        return uiText(QStringLiteral("recording_idle"), status);
    }
    if (status == QStringLiteral("Playback: idle")) {
        return uiText(QStringLiteral("playback_idle"), status);
    }
    if (status == QStringLiteral("Spectrum frames: idle")) {
        return uiText(QStringLiteral("spectrum_frames_idle"), status);
    }
    if (status == QStringLiteral("Spectrum frames: buffer off")) {
        return uiText(QStringLiteral("spectrum_frames_buffer_off"), status);
    }
    if (status == QStringLiteral("Spectrum recording stopped: no frames")) {
        return uiText(QStringLiteral("spectrum_recording_stopped_no_frames"), status);
    }
    if (status == QStringLiteral("Recording blocked: Channel IQ cannot run during Full IQ streaming")) {
        return uiText(QStringLiteral("recording_blocked_channel_iq_full_iq"), status);
    }
    if (status == QStringLiteral("Recording stopped: Channel IQ cannot run during Full IQ streaming")) {
        return uiText(QStringLiteral("recording_stopped_channel_iq_full_iq"), status);
    }
    if (status == QStringLiteral("Recording stopped: no data")) {
        return uiText(QStringLiteral("recording_stopped_no_data"), status);
    }
    if (status == QStringLiteral("IQ recording stopped: sample rate changed")) {
        return uiText(QStringLiteral("iq_recording_stopped_rate_changed"), status);
    }
    if (status == QStringLiteral("Playback blocked: stop receiver first")) {
        return uiText(QStringLiteral("playback_blocked_stop_receiver"), status);
    }
    if (status == QStringLiteral("Playback blocked: stop recording first")) {
        return uiText(QStringLiteral("playback_blocked_stop_recording"), status);
    }
    if (status == QStringLiteral("Playback: no file selected")) {
        return uiText(QStringLiteral("playback_no_file_selected"), status);
    }
    if (status == QStringLiteral("Playback failed: audio WAV must be mono 48 kHz")) {
        return uiText(QStringLiteral("playback_audio_wav_mono"), status);
    }
    if (status == QStringLiteral("Playback: stopped")) {
        return uiText(QStringLiteral("playback_stopped"), status);
    }

    const QStringList prefixTranslations = {
        prefixStatus(QStringLiteral("Recording failed: "), QStringLiteral("recording_failed"), QStringLiteral("Recording failed"), QStringLiteral(": ")),
        prefixStatus(QStringLiteral("Recording audio: "), QStringLiteral("recording_audio"), QStringLiteral("Recording audio"), QStringLiteral(": ")),
        prefixStatus(QStringLiteral("Recording channel IQ: waiting for IQ frames"),
                     QStringLiteral("recording_channel_iq_waiting"),
                     QStringLiteral("Recording channel IQ: waiting for IQ frames"),
                     QString()),
        prefixStatus(QStringLiteral("Recording channel IQ: "), QStringLiteral("recording_channel_iq"), QStringLiteral("Recording channel IQ"), QStringLiteral(": ")),
        prefixStatus(QStringLiteral("IQ recording failed: "), QStringLiteral("iq_recording_failed"), QStringLiteral("IQ recording failed"), QStringLiteral(": ")),
        prefixStatus(QStringLiteral("Recording saved: "), QStringLiteral("recording_saved"), QStringLiteral("Recording saved"), QStringLiteral(": ")),
        prefixStatus(QStringLiteral("Spectrum recording failed: "), QStringLiteral("spectrum_recording_failed"), QStringLiteral("Spectrum recording failed"), QStringLiteral(": ")),
        prefixStatus(QStringLiteral("Spectrum recording: "), QStringLiteral("spectrum_recording"), QStringLiteral("Spectrum recording"), QStringLiteral(": ")),
        prefixStatus(QStringLiteral("Spectrum saved: "), QStringLiteral("spectrum_saved"), QStringLiteral("Spectrum saved"), QStringLiteral(": ")),
        prefixStatus(QStringLiteral("Playback failed: "), QStringLiteral("playback_failed"), QStringLiteral("Playback failed"), QStringLiteral(": ")),
        prefixStatus(QStringLiteral("Playback: "), QStringLiteral("playback"), QStringLiteral("Playback"), QStringLiteral(": "))
    };

    for (const QString &translated : prefixTranslations) {
        if (!translated.isEmpty()) {
            return translated;
        }
    }
    return status;
}

void YourClassName::markTranslatable(QWidget *widget, const QString &key, const QString &fallback) {
    if (!widget) {
        return;
    }
    widget->setProperty("i18nKey", key);
    widget->setProperty("i18nFallback", fallback);
    const QString text = uiText(key, fallback);
    if (auto *label = qobject_cast<QLabel*>(widget)) {
        label->setText(text);
    } else if (auto *button = qobject_cast<QAbstractButton*>(widget)) {
        button->setText(text);
    } else if (auto *group = qobject_cast<QGroupBox*>(widget)) {
        group->setTitle(text);
    } else if (auto *dock = qobject_cast<QDockWidget*>(widget)) {
        dock->setWindowTitle(text);
    }
}

void YourClassName::setComboItemText(QComboBox *combo,
                                     const QVariant &data,
                                     const QString &key,
                                     const QString &fallback) {
    if (!combo) {
        return;
    }
    const int index = combo->findData(data);
    if (index >= 0) {
        combo->setItemText(index, uiText(key, fallback));
    }
}

void YourClassName::applyUiLanguage() {
    const auto widgets = findChildren<QWidget*>();
    for (QWidget *widget : widgets) {
        const QString key = widget->property("i18nKey").toString();
        if (key.isEmpty()) {
            continue;
        }
        markTranslatable(widget, key, widget->property("i18nFallback").toString());
    }

    if (audioFilterChainWidget) {
        audioFilterChainWidget->setLanguage(normalizedUiLanguage(uiLanguage) == QStringLiteral("uk"));
    }
    if (multiVfoWidget) {
        multiVfoWidget->setLanguage(normalizedUiLanguage(uiLanguage) == QStringLiteral("uk"));
    }
    if (dspFlowPanel) {
        dspFlowPanel->setLanguage(normalizedUiLanguage(uiLanguage) == QStringLiteral("uk"));
    }
    if (dspPathButton) {
        dspPathButton->setToolTip(normalizedUiLanguage(uiLanguage) == QStringLiteral("uk")
                                      ? QStringLiteral("Відкрити конструктор DSP-шляху")
                                      : QStringLiteral("Open the DSP path designer"));
    }

    if (languageComboBox) {
        QSignalBlocker blocker(languageComboBox);
        const int index = languageComboBox->findData(uiLanguage);
        if (index >= 0) {
            languageComboBox->setCurrentIndex(index);
        }
    }

    syncFftResolutionControls();

    setComboItemText(clkBox, 0, QStringLiteral("internal"), QStringLiteral("Internal"));
    setComboItemText(clkBox, 1, QStringLiteral("external"), QStringLiteral("External"));
    setComboItemText(waterfallDisplayModeCombo,
                     static_cast<int>(MyWaterfallWidget::DisplayMode::Waterfall2D),
                     QStringLiteral("waterfall_display_2d"),
                     QStringLiteral("2D waterfall"));
    setComboItemText(waterfallDisplayModeCombo,
                     static_cast<int>(MyWaterfallWidget::DisplayMode::Waterfall3D),
                     QStringLiteral("waterfall_display_3d"),
                     QStringLiteral("3D waterfall"));
    setComboItemText(waterfallDisplayModeCombo,
                     static_cast<int>(MyWaterfallWidget::DisplayMode::Waterfall3DWithMini),
                     QStringLiteral("waterfall_display_3d_mini"),
                     QStringLiteral("3D + mini waterfall"));
    if (waterfallDisplayModeCombo) {
        waterfallDisplayModeCombo->setToolTip(waterfall3DAlternativeView
            ? uiText(QStringLiteral("alternative_interface_mode_locked"),
                     QStringLiteral("Alternative interface supports fixed 3D and 3D with a mini waterfall."))
            : uiText(QStringLiteral("waterfall_display_mode_tooltip"),
                     QStringLiteral("Choose the main waterfall visualization mode.")));
    }
    if (alternativeSpectrumGradientCheckbox) {
        alternativeSpectrumGradientCheckbox->setToolTip(uiText(
            QStringLiteral("alternative_spectrum_gradient_tooltip"),
            QStringLiteral("Fill below the spectrum contour in Alternative interface mode, or fill the visible front face of a fixed 3D plane.")));
    }
    if (alternativeSpectrumGradientOpacitySlider) {
        alternativeSpectrumGradientOpacitySlider->setToolTip(uiText(
            QStringLiteral("alternative_spectrum_gradient_opacity_tooltip"),
            QStringLiteral("Set gradient opacity from transparent to solid.")));
    }
    if (waterfall3DResolutionCombo) {
        waterfall3DResolutionCombo->setToolTip(uiText(
            QStringLiteral("waterfall_3d_resolution_tooltip"),
            QStringLiteral("3D frequency columns as a fraction of the visible 2D waterfall width.")));
    }
    if (waterfall3DHistoryRowsSpin) {
        waterfall3DHistoryRowsSpin->setSuffix(
            uiText(QStringLiteral("waterfall_3d_rows_suffix"), QStringLiteral(" rows")));
        waterfall3DHistoryRowsSpin->setToolTip(uiText(
            QStringLiteral("waterfall_3d_history_rows_tooltip"),
            QStringLiteral("Number of recent waterfall rows retained in the 3D volume. Larger values use more CPU and memory.")));
    }
    if (waterfall3DSliceStepSpin) {
        waterfall3DSliceStepSpin->setToolTip(uiText(
            QStringLiteral("waterfall_3d_slice_step_tooltip"),
            QStringLiteral("Frequency points moved by each wheel step while an Alt slice is held.")));
    }
    if (waterfall3DSliceWidthSpin) {
        waterfall3DSliceWidthSpin->setToolTip(uiText(
            QStringLiteral("waterfall_3d_slice_width_tooltip"),
            QStringLiteral("Number of adjacent frequency points displayed in the held Alt slice; 1 shows one line.")));
    }
    if (waterfall3DSpectrumSliceStepSpin) {
        waterfall3DSpectrumSliceStepSpin->setSuffix(
            uiText(QStringLiteral("waterfall_3d_rows_suffix"), QStringLiteral(" rows")));
        waterfall3DSpectrumSliceStepSpin->setToolTip(uiText(
            QStringLiteral("waterfall_3d_spectrum_slice_step_tooltip"),
            QStringLiteral("Rows moved by each wheel step while an Alt + right-button spectrum slice is held.")));
    }
    if (waterfall3DSpectrumSliceRowsSpin) {
        waterfall3DSpectrumSliceRowsSpin->setSuffix(
            uiText(QStringLiteral("waterfall_3d_rows_suffix"), QStringLiteral(" rows")));
        waterfall3DSpectrumSliceRowsSpin->setToolTip(uiText(
            QStringLiteral("waterfall_3d_spectrum_slice_rows_tooltip"),
            QStringLiteral("Number of adjacent time rows displayed by the Alt + right-button spectrum slice.")));
    }
    if (waterfall3DSpectrumSliceCaptureCheckbox) {
        waterfall3DSpectrumSliceCaptureCheckbox->setText(uiText(
            QStringLiteral("waterfall_3d_spectrum_slice_capture"),
            QStringLiteral("Capture")));
        waterfall3DSpectrumSliceCaptureCheckbox->setToolTip(uiText(
            QStringLiteral("waterfall_3d_spectrum_slice_capture_tooltip"),
            QStringLiteral("Follow the selected data rows as they move through the rolling 3D history. Off keeps the slice at a fixed model position and shows the current rows there.")));
    }
    if (waterfall3DSpectrumSliceCaptureFixedCheckbox) {
        waterfall3DSpectrumSliceCaptureFixedCheckbox->setText(uiText(
            QStringLiteral("waterfall_3d_spectrum_slice_capture_fixed"),
            QStringLiteral("Fix capture")));
        waterfall3DSpectrumSliceCaptureFixedCheckbox->setToolTip(uiText(
            QStringLiteral("waterfall_3d_spectrum_slice_capture_fixed_tooltip"),
            QStringLiteral("Freeze a copy of the captured rows at the picked model position until those rows expire from the rolling history.")));
    }
    if (waterfall3DFixedPlaneCheckbox) {
        waterfall3DFixedPlaneCheckbox->setText(uiText(
            QStringLiteral("waterfall_3d_fixed_plane"),
            QStringLiteral("Fix plane")));
        waterfall3DFixedPlaneCheckbox->setToolTip(uiText(
            QStringLiteral("waterfall_3d_fixed_plane_tooltip"),
            QStringLiteral("Use the fixed front-facing 3D plane from Alternative interface mode and lock camera controls.")));
    }
    if (waterfall3DMonochromeCheckbox) {
        waterfall3DMonochromeCheckbox->setText(uiText(
            QStringLiteral("waterfall_3d_monochrome"),
            QStringLiteral("Monochrome blue 3D")));
        waterfall3DMonochromeCheckbox->setToolTip(uiText(
            QStringLiteral("waterfall_3d_monochrome_tooltip"),
            QStringLiteral("Render the 3D surface in a calm solid blue instead of amplitude colors.")));
    }
    if (waterfall3DHeightGradientCheckbox) {
        waterfall3DHeightGradientCheckbox->setText(uiText(
            QStringLiteral("waterfall_3d_height_gradient"),
            QStringLiteral("Height gradient")));
        waterfall3DHeightGradientCheckbox->setToolTip(uiText(
            QStringLiteral("waterfall_3d_height_gradient_tooltip"),
            QStringLiteral("Color the entire 3D geometry by absolute height, from a dark base to the peak color.")));
    }
    if (waterfall3DMonochromeColorLabel) {
        waterfall3DMonochromeColorLabel->setText(uiText(
            QStringLiteral("waterfall_3d_monochrome_color"),
            QStringLiteral("3D color:")));
    }
    if (waterfall3DMonochromeColorButton) {
        waterfall3DMonochromeColorButton->setToolTip(uiText(
            QStringLiteral("waterfall_3d_monochrome_color_tooltip"),
            QStringLiteral("Choose the base color used by monochrome 3D rendering.")));
    }
    if (waterfall3DMonochromeGradientDepthLabel) {
        waterfall3DMonochromeGradientDepthLabel->setText(uiText(
            QStringLiteral("waterfall_3d_monochrome_gradient_depth"),
            QStringLiteral("Gradient depth:")));
    }
    if (waterfall3DMonochromeGradientDepthSlider) {
        waterfall3DMonochromeGradientDepthSlider->setToolTip(uiText(
            QStringLiteral("waterfall_3d_monochrome_gradient_depth_tooltip"),
            QStringLiteral("Set the brightness range between the base and the tops of monochrome 3D geometry.")));
    }
    if (waterfall3DAlternativeViewCheckbox) {
        waterfall3DAlternativeViewCheckbox->setText(uiText(
            QStringLiteral("waterfall_3d_alternative_view"),
            QStringLiteral("Alternative view")));
        waterfall3DAlternativeViewCheckbox->setToolTip(uiText(
            QStringLiteral("waterfall_3d_alternative_view_tooltip"),
            QStringLiteral("Use the fixed front-facing 3D waterfall with the live spectrum at its near edge.")));
    }
    if (waterfall3DVncSliceInputCheckbox) {
        waterfall3DVncSliceInputCheckbox->setText(uiText(
            QStringLiteral("waterfall_3d_vnc_slice_input"),
            QStringLiteral("VNC slice control")));
        waterfall3DVncSliceInputCheckbox->setToolTip(uiText(
            QStringLiteral("waterfall_3d_vnc_slice_input_tooltip"),
            QStringLiteral("Use left/right mouse buttons for frequency/time slices without keyboard modifiers. Disable it to restore normal tuning, panning and context-menu actions.")));
    }
    setComboItemText(modeBox, static_cast<int>(INPUT_HF_NOISE_CANCEL),
                     QStringLiteral("hf_cancel_lab"),
                     QStringLiteral("HF interference lab"));
    setComboItemText(dmrLabCallTypeCombo,
                     QStringLiteral("group"),
                     QStringLiteral("dmr_group"),
                     QStringLiteral("Group"));
    setComboItemText(dmrLabCallTypeCombo,
                     QStringLiteral("private"),
                     QStringLiteral("dmr_private"),
                     QStringLiteral("Private"));
    setComboItemText(dmrLabCallTypeCombo,
                     QStringLiteral("all_call"),
                     QStringLiteral("dmr_all"),
                     QStringLiteral("All"));
    setComboItemText(dmrAmbeLayoutCombo,
                     DMR_AMBE_LAYOUT_AUTO,
                     QStringLiteral("auto"),
                     QStringLiteral("Auto"));
    setComboItemText(dmrBackendCombo,
                     DMR_BACKEND_FOBOS_MBELIB,
                     QStringLiteral("dmr_backend_fobos_mbelib"),
                     QStringLiteral("Obrii SDR + mbelib"));
    setComboItemText(dmrBackendCombo,
                     DMR_BACKEND_FOBOS_OPENDMR,
                     QStringLiteral("dmr_backend_fobos_opendmr"),
                     QStringLiteral("Obrii SDR + OpenDMR/OP25"));
    setComboItemText(dmrBackendCombo,
                     DMR_BACKEND_GOPHERTRUNK,
                     QStringLiteral("dmr_backend_gopher_future"),
                     QStringLiteral("GopherTrunk bridge"));
    setComboItemText(dmrPrivacyModeCombo,
                     DMR_PRIVACY_NONE,
                     QStringLiteral("dmr_privacy_none"),
                     QStringLiteral("No privacy"));
    setComboItemText(dmrPrivacyModeCombo, DMR_PRIVACY_HYTERA_BASIC,
                     QStringLiteral("dmr_privacy_hytera_basic"),
                     QStringLiteral("Hytera Basic (beta)"));
    if (dmrPrivacyModeCombo)
        dmrPrivacyModeCombo->setToolTip(uiText(QStringLiteral("dmr_privacy_mode_tooltip"),
            QStringLiteral("The selected mode and saved key automatically apply to the selected decoder. No privacy disables decryption. No key guessing is performed.")));
    if (dmrBackendCombo) {
        dmrBackendCombo->setToolTip(uiText(
            QStringLiteral("dmr_backend_tooltip"),
            QStringLiteral("Choose which DMR decoder path receives the selected channel.")));
    }
    setComboItemText(recordingModeCombo,
                     static_cast<int>(RecordingManager::Mode::AudioWav),
                     QStringLiteral("audio_wav"),
                     QStringLiteral("Audio WAV"));
    setComboItemText(recordingModeCombo,
                     static_cast<int>(RecordingManager::Mode::ChannelIqWav),
                     QStringLiteral("channel_iq_wav"),
                     QStringLiteral("Channel IQ WAV"));
    setComboItemText(spectrumEventModeCombo,
                     0,
                     QStringLiteral("spectrum_event_spectrum_only"),
                     QStringLiteral("Spectrum only"));
    setComboItemText(spectrumEventModeCombo,
                     1,
                     QStringLiteral("spectrum_event_channel_iq"),
                     QStringLiteral("Spectrum + Channel IQ"));
    setComboItemText(spectrumEventModeCombo,
                     2,
                     QStringLiteral("spectrum_event_full_iq"),
                     QStringLiteral("Spectrum + Full IQ short"));
    if (spectrumFrameRecordButton) {
        spectrumFrameRecordButton->setToolTip(uiText(
            QStringLiteral("spectrum_rec_tooltip"),
            QStringLiteral("Record compact FFT spectrum frames for event replay and measurement.")));
    }
    if (spectrumFrameReplayButton) {
        spectrumFrameReplayButton->setToolTip(uiText(
            QStringLiteral("spectrum_replay_tooltip"),
            QStringLiteral("Open a spectrum-frame recording and inspect the waterfall timeline.")));
    }
    if (spectrumEventModeCombo) {
        spectrumEventModeCombo->setToolTip(uiText(
            QStringLiteral("spectrum_event_mode_tooltip"),
            QStringLiteral("Choose what is saved when the spectrum event trigger is pressed.")));
    }
    if (spectrumFrameBufferCheckbox) {
        spectrumFrameBufferCheckbox->setToolTip(uiText(
            QStringLiteral("spectrum_buffer_tooltip"),
            QStringLiteral("Keep a rolling pre-trigger spectrum buffer in RAM.")));
    }
    if (spectrumFrameBinsCombo) {
        spectrumFrameBinsCombo->setToolTip(uiText(
            QStringLiteral("spectrum_bins_tooltip"),
            QStringLiteral("Maximum bins stored per spectrum frame. Higher values preserve detail but increase file size.")));
    }
    if (spectrumFramePrebufferSpin) {
        spectrumFramePrebufferSpin->setToolTip(uiText(
            QStringLiteral("spectrum_prebuffer_tooltip"),
            QStringLiteral("Seconds of spectrum frames kept before the hotkey trigger.")));
    }
    setComboItemText(scanVisualModeCombo,
                     static_cast<int>(ScanVisualMode::CompressedMosaic),
                     QStringLiteral("scan_visual_compressed"),
                     QStringLiteral("Compressed/Mosaic"));
    setComboItemText(scanVisualModeCombo,
                     static_cast<int>(ScanVisualMode::FloatingTrueAxis),
                     QStringLiteral("scan_visual_true_axis"),
                     QStringLiteral("Floating/True axis"));
    setComboItemText(scanVisualModeCombo,
                     static_cast<int>(ScanVisualMode::PassComposite),
                     QStringLiteral("scan_visual_pass_composite"),
                     QStringLiteral("Pass composite"));
    if (scanVisualModeCombo) {
        scanVisualModeCombo->setToolTip(uiText(
            QStringLiteral("scan_visual_mode_tooltip"),
            QStringLiteral("Choose how scan centers are stitched on the spectrum and waterfall.")));
    }
    setComboItemText(qthSourceCombo,
                     QStringLiteral("manual"),
                     QStringLiteral("manual"),
                     QStringLiteral("Manual"));
    setComboItemText(qthSourceCombo,
                     QStringLiteral("nmea"),
                     QStringLiteral("nmea_gps"),
                     QStringLiteral("NMEA GPS"));
    setComboItemText(qthSourceCombo,
                     QStringLiteral("os"),
                     QStringLiteral("os_location"),
                     QStringLiteral("OS location"));
    setComboItemText(gnssPositionPolicyCombo,
                     QStringLiteral("auto"),
                     QStringLiteral("gnss_position_policy_auto"),
                     QStringLiteral("Auto"));
    setComboItemText(gnssPositionPolicyCombo,
                     QStringLiteral("ubx_preferred"),
                     QStringLiteral("gnss_position_policy_ubx_preferred"),
                     QStringLiteral("UBX preferred"));
    setComboItemText(gnssPositionPolicyCombo,
                     QStringLiteral("nmea_only"),
                     QStringLiteral("gnss_position_policy_nmea_only"),
                     QStringLiteral("NMEA only"));
    setComboItemText(gnssPositionPolicyCombo,
                     QStringLiteral("ubx_only"),
                     QStringLiteral("gnss_position_policy_ubx_only"),
                     QStringLiteral("UBX only"));
    if (gnssPositionPolicyCombo) {
        gnssPositionPolicyCombo->setToolTip(uiText(
            QStringLiteral("gnss_position_policy_tooltip"),
            QStringLiteral("Choose which external GNSS stream is allowed to update the current QTH position.")));
    }
    setComboItemText(gnssTimeZoneCombo,
                     100000,
                     QStringLiteral("local_time"),
                     QStringLiteral("Local"));
    if (gnssTimeZoneCombo) {
        gnssTimeZoneCombo->setToolTip(uiText(
            QStringLiteral("gnss_timezone_tooltip"),
            QStringLiteral("Time zone used only for displaying GNSS UTC time. Raw NMEA/UBX data remains unchanged.")));
    }
    if (qthSourceCombo) {
        qthSourceCombo->setItemData(1,
                                    uiText(QStringLiteral("nmea_paste_tooltip"),
                                           QStringLiteral("Paste NMEA GGA/RMC text from the clipboard and use it as the current QTH.")),
                                    Qt::ToolTipRole);
        qthSourceCombo->setItemData(2,
                                    uiText(QStringLiteral("gps_source_future_tooltip"),
                                           QStringLiteral("OS location input is planned; manual coordinates are used for now.")),
                                    Qt::ToolTipRole);
    }
    if (qthPasteNmeaButton) {
        qthPasteNmeaButton->setToolTip(uiText(
            QStringLiteral("nmea_paste_tooltip"),
            QStringLiteral("Paste NMEA GGA/RMC text from the clipboard and use it as the current QTH.")));
    }
    if (qthCopyButton) {
        qthCopyButton->setToolTip(uiText(
            QStringLiteral("qth_position_tooltip"),
            QStringLiteral("Show the last valid QTH position on the map and copy its Maidenhead locator.")));
    }
    if (qthClearButton) {
        qthClearButton->setToolTip(uiText(
            QStringLiteral("clear_qth_tooltip"),
            QStringLiteral("Hide the current real QTH position from the map and clear the live location state.")));
    }
    if (gnssMonitorResetButton) {
        gnssMonitorResetButton->setToolTip(uiText(
            QStringLiteral("gnss_iq_monitor_reset_tooltip"),
            QStringLiteral("Reset only the GNSS IQ monitor peak history and accumulated SDR statistics. It does not clear the QTH position.")));
    }
    if (gnssSerialPortEdit) {
        gnssSerialPortEdit->setToolTip(uiText(
            QStringLiteral("gnss_serial_port_tooltip"),
            QStringLiteral("Serial port for an external NMEA GNSS receiver, for example COM4 or /dev/ttyUSB0.")));
    }
    if (gnssSerialBaudSpin) {
        gnssSerialBaudSpin->setToolTip(uiText(
            QStringLiteral("gnss_serial_baud_tooltip"),
            QStringLiteral("NMEA serial baud rate. Most NEO-M8N modules use 9600 by default.")));
    }
    if (gnssSerialButton) {
        gnssSerialButton->setToolTip(uiText(
            QStringLiteral("gnss_serial_connect_tooltip"),
            QStringLiteral("Open the serial NMEA stream and use valid GGA/RMC fixes as the current QTH.")));
    }
    if (gnssNmeaLogButton) {
        gnssNmeaLogButton->setToolTip(uiText(
            QStringLiteral("nmea_log_tooltip"),
            QStringLiteral("Start or stop writing live NMEA sentences to recordings/nmea.")));
    }
    if (gnssNmeaReplayButton) {
        gnssNmeaReplayButton->setToolTip(uiText(
            QStringLiteral("nmea_replay_tooltip"),
            QStringLiteral("Replay a saved NMEA log through the same parser, map and satellite diagnostics.")));
    }
    if (gnssAcquisitionPlotDialog) {
        gnssAcquisitionPlotDialog->setWindowTitle(uiText(
            QStringLiteral("gnss_acq_plot_title"),
            QStringLiteral("GNSS acquisition diagnostics")));
    }
    if (gnssPlotButton) {
        gnssPlotButton->setToolTip(uiText(
            QStringLiteral("gnss_acq_plot_tooltip"),
            QStringLiteral("GPS acquisition diagnostics: PRN/Doppler heatmap, best code-phase correlation and peak history.")));
    }
    if (gnssSatelliteDialog) {
        gnssSatelliteDialog->setWindowTitle(uiText(
            QStringLiteral("gnss_satellite_window_title"),
            QStringLiteral("GNSS satellites")));
    }
    if (gnssSatelliteTableDialog) {
        gnssSatelliteTableDialog->setWindowTitle(uiText(
            QStringLiteral("gnss_satellite_table_window_title"),
            QStringLiteral("GNSS satellite table")));
    }
    if (gnssSatellitesButton) {
        gnssSatellitesButton->setToolTip(uiText(
            QStringLiteral("gnss_satellites_tooltip"),
            QStringLiteral("Open the live GNSS satellite list and sky view in a separate resizable window.")));
    }
    if (gnssSatelliteTable) {
        gnssSatelliteTable->setHorizontalHeaderLabels({
            uiText(QStringLiteral("use"), QStringLiteral("Use")),
            uiText(QStringLiteral("source_short"), QStringLiteral("Src")),
            uiText(QStringLiteral("system"), QStringLiteral("System")),
            QStringLiteral("SVID"),
            uiText(QStringLiteral("elevation_short"), QStringLiteral("El")),
            uiText(QStringLiteral("azimuth_short"), QStringLiteral("Az")),
            QStringLiteral("C/N0"),
            uiText(QStringLiteral("fix"), QStringLiteral("Fix")),
            uiText(QStringLiteral("age"), QStringLiteral("Age")),
            uiText(QStringLiteral("quality"), QStringLiteral("Quality"))
        });
        gnssSatelliteTable->setToolTip(uiText(
            QStringLiteral("gnss_satellite_table_tooltip"),
            QStringLiteral("Live GNSS satellites from NMEA GSV/GSA and UBX NAV-SAT. Click headers to sort; right-click Use for bulk selection.")));
    }
    if (gnssSatelliteStatusLabel) {
        gnssSatelliteStatusLabel->setToolTip(uiText(
            QStringLiteral("gnss_satellite_status_tooltip"),
            QStringLiteral("Live GNSS fix summary from NMEA and UBX: source, fix type, satellites, DOP, altitude, speed and UTC.")));
    }
    if (gnssSatelliteTableCheckbox) {
        QSignalBlocker blocker(gnssSatelliteTableCheckbox);
        gnssSatelliteTableCheckbox->setText(uiText(QStringLiteral("show_table"), QStringLiteral("Table")));
        gnssSatelliteTableCheckbox->setChecked(gnssSatelliteTableVisible);
        gnssSatelliteTableCheckbox->setToolTip(uiText(
            QStringLiteral("gnss_satellite_table_toggle_tooltip"),
            QStringLiteral("Open or hide the live satellite table in a separate resizable window.")));
    }
    if (gnssSatelliteSkyLabel) {
        gnssSatelliteSkyLabel->setToolTip(uiText(
            QStringLiteral("gnss_sky_tooltip"),
            QStringLiteral("Sky view from NMEA GSV or UBX NAV-SAT azimuth/elevation values. Color follows C/N0; white outline means the satellite is used in the module fix.")));
    }
    if (gnssDopplerSpanSpin) {
        gnssDopplerSpanSpin->setToolTip(uiText(
            QStringLiteral("gnss_doppler_span_tooltip"),
            QStringLiteral("GPS C/A acquisition Doppler search half-span. Lower values are faster; wider values tolerate larger clock/frequency error.")));
    }
    if (gnssDopplerStepSpin) {
        gnssDopplerStepSpin->setToolTip(uiText(
            QStringLiteral("gnss_doppler_step_tooltip"),
            QStringLiteral("GPS C/A acquisition Doppler bin spacing. Smaller values are slower but can refine weak candidates.")));
    }
    if (gnssOfflineAcquireButton) {
        gnssOfflineAcquireButton->setToolTip(uiText(
            QStringLiteral("gps_ca_replay_scan_tooltip"),
            QStringLiteral("Run GPS L1 C/A acquisition directly on the selected Channel IQ WAV recording.")));
    }
    if (gnssPositionSelfTestButton) {
        gnssPositionSelfTestButton->setToolTip(uiText(
            QStringLiteral("gnss_position_self_test_tooltip"),
            QStringLiteral("Solve a synthetic multi-satellite pseudorange fix and show the result on the QTH map.")));
    }
    if (playbackFileCombo &&
        playbackFileCombo->count() == 1 &&
        playbackFileCombo->itemData(0).toString().isEmpty()) {
        playbackFileCombo->setItemText(0,
                                       uiText(QStringLiteral("no_wav_recordings_found"),
                                              QStringLiteral("No WAV recordings found")));
    }

    if (volumeLabel) {
        volumeLabel->setText(QStringLiteral("%1: %2%").arg(uiText(QStringLiteral("volume"), QStringLiteral("Volume"))).arg(volumePercent));
    }
    if (lnaGainLabel) {
        lnaGainLabel->setText(QStringLiteral("%1: %2").arg(uiText(QStringLiteral("lna_gain"), QStringLiteral("LNA Gain"))).arg(pendingSettings.lnaGain));
    }
    if (vgaGainLabel) {
        vgaGainLabel->setText(QStringLiteral("%1: %2").arg(uiText(QStringLiteral("vga_gain"), QStringLiteral("VGA Gain"))).arg(pendingSettings.vgaGain));
    }
    if (rtlAgcCheckbox) {
        rtlAgcCheckbox->blockSignals(true);
        rtlAgcCheckbox->setChecked(pendingSettings.rtlAgc);
        rtlAgcCheckbox->blockSignals(false);
    }
    if (rtlGainSlider) {
        rtlGainSlider->blockSignals(true);
        rtlGainSlider->setValue((std::clamp)(pendingSettings.rtlTunerGainTenthsDb, 0, 496));
        rtlGainSlider->blockSignals(false);
    }
    if (rtlGainLabel) {
        rtlGainLabel->setText(QStringLiteral("%1: %2 dB")
                                  .arg(uiText(QStringLiteral("rtl_gain"), QStringLiteral("RTL gain")))
                                  .arg(pendingSettings.rtlTunerGainTenthsDb / 10.0, 0, 'f', 1));
    }
    updateReceiverSpecificControls();
    if (scaleLabel) {
        scaleLabel->setText(QStringLiteral("%1: %2").arg(uiText(QStringLiteral("scale"), QStringLiteral("Scale")),
                                                        formatScalePercent(currentScale)));
    }
    if (additionalScaleDivisorLabel) {
        additionalScaleDivisorLabel->setText(
            QStringLiteral("%1: 1:%2")
                .arg(uiText(QStringLiteral("additional_scale"), QStringLiteral("Extra zoom")))
                .arg((std::clamp)(additionalScaleDivisor,
                                  1,
                                  ADDITIONAL_SCALE_DIVISOR_MAX)));
    }
    if (contrastLabel) {
        contrastLabel->setText(QStringLiteral("%1: %2").arg(uiText(QStringLiteral("contrast"), QStringLiteral("Contrast"))).arg(contrast));
    }
    if (sensitivityLabel) {
        sensitivityLabel->setText(QStringLiteral("%1: %2").arg(uiText(QStringLiteral("sensitivity"), QStringLiteral("Sensitivity"))).arg(sensitivity));
    }
    if (levelMinLabel) {
        levelMinLabel->setText(levelLabelText(uiText(QStringLiteral("min"), QStringLiteral("Min")), displayLevelMin));
    }
    if (levelMaxLabel) {
        levelMaxLabel->setText(levelLabelText(uiText(QStringLiteral("max"), QStringLiteral("Max")), displayLevelMax));
    }
    updateFineTuneLabel();
    if (scanMeasurementBaselineButton) {
        scanMeasurementBaselineButton->setText(scanMeasurementBaselineRecording
                                                   ? uiText(QStringLiteral("stop_bg"), QStringLiteral("Stop BG"))
                                                   : uiText(QStringLiteral("bg_rec"), QStringLiteral("BG Rec")));
    }
    updateSpectrumScienceUi();
    if (recordingStatusLabel) {
        const QString rawStatus = recordingStatusLabel->property("statusRawText").toString();
        const QString localized = localizedStatusText(rawStatus.isEmpty()
                                                          ? QStringLiteral("Recording: idle")
                                                          : rawStatus);
        recordingStatusLabel->setText(localized);
        recordingStatusLabel->setToolTip(localized);
    }
    if (spectrumFrameRecordingStatusLabel) {
        const QString rawStatus = spectrumFrameRecordingStatusLabel->property("statusRawText").toString();
        const QString localized = localizedStatusText(rawStatus.isEmpty()
                                                          ? QStringLiteral("Spectrum frames: idle")
                                                          : rawStatus);
        spectrumFrameRecordingStatusLabel->setText(localized);
        spectrumFrameRecordingStatusLabel->setToolTip(localized);
    }
    if (playbackStatusLabel) {
        const QString rawStatus = playbackStatusLabel->property("statusRawText").toString();
        const QString localized = localizedStatusText(rawStatus.isEmpty()
                                                          ? QStringLiteral("Playback: idle")
                                                          : rawStatus);
        playbackStatusLabel->setText(localized);
        playbackStatusLabel->setToolTip(localized);
    }

    updateAudioFilterLabels();
    updateHfNoiseCancelControls();
    updateQthControls();
    applySpectrumHunterTranslations();
    updateAgileScanControls();
    updateScanMeasurementStatus();
    updateSpurSuppressionStatus();
    updateVideoProcessorMode();
    updateDigitalDecoderMode();
    updateNetworkButtonText();
}

void YourClassName::applySpectrumHunterTranslations() {
    auto applyHunter = [this](SpectrumHunterControls *controls,
                              const QString &titleKey,
                              const QString &titleFallback,
                              const QString &tooltipKey,
                              const QString &tooltipFallback) {
        if (!controls) {
            return;
        }
        controls->setUiText(
            uiText(titleKey, titleFallback),
            uiText(tooltipKey, tooltipFallback),
            uiText(QStringLiteral("detect"), QStringLiteral("Detect")),
            uiText(QStringLiteral("use_scan"), QStringLiteral("Use scan")),
            uiText(QStringLiteral("use_scan_tooltip"),
                   QStringLiteral("Copy this preset into Agile scan ranges and step")),
            uiText(QStringLiteral("tune"), QStringLiteral("Tune")),
            uiText(QStringLiteral("tune_candidate_tooltip"),
                   QStringLiteral("Tune to the best detected candidate center")),
            uiText(QStringLiteral("follow"), QStringLiteral("Follow")),
            uiText(QStringLiteral("follow_candidate_tooltip"),
                   QStringLiteral("Keep tuning to the selected detected candidate as the scan updates")),
            uiText(QStringLiteral("previous_candidate_tooltip"),
                   QStringLiteral("Tune to previous detected candidate")),
            uiText(QStringLiteral("next_candidate_tooltip"),
                   QStringLiteral("Tune to next detected candidate")),
            uiText(QStringLiteral("min_width_short"), QStringLiteral("Min W:")),
            uiText(QStringLiteral("max_width_short"), QStringLiteral("Max W:")),
            uiText(QStringLiteral("threshold_short"), QStringLiteral("Thr:")));
    };

    applyHunter(dmrHunterControls,
                QStringLiteral("dmr_hunter"),
                QStringLiteral("DMR Hunter"),
                QStringLiteral("dmr_hunter_tooltip"),
                QStringLiteral("Detect DMR-like TDMA bursts in the current spectrum"));
    applyHunter(fpvHunterControls,
                QStringLiteral("fpv_hunter"),
                QStringLiteral("FPV Hunter"),
                QStringLiteral("fpv_hunter_tooltip"),
                QStringLiteral("Detect wide FPV-like video carriers in the current spectrum"));
    if (fpvHunterControls) {
        fpvHunterControls->setAutoScanUiText(
            uiText(QStringLiteral("fpv_auto_scan_decode"),
                   QStringLiteral("Auto scan + decode")),
            uiText(QStringLiteral("fpv_auto_scan_decode_tooltip"),
                   QStringLiteral("Scan the selected preset, stop on a stable analog video carrier, verify sync, then resume after signal loss")),
            uiText(QStringLiteral("dwell_short"), QStringLiteral("Dwell:")),
            uiText(QStringLiteral("loss_short"), QStringLiteral("Loss:")),
            uiText(QStringLiteral("hsync_short"), QStringLiteral("HSync:")));
    }
    applyHunter(digitalVideoHunterControls,
                QStringLiteral("digital_video_hunter"),
                QStringLiteral("Digital Video Hunter"),
                QStringLiteral("digital_video_hunter_tooltip"),
                QStringLiteral("Detect wide digital video / OFDM-like carriers in the current spectrum"));
}

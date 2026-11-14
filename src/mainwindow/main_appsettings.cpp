#include "main.h"
#include "core/fobosifselection.h"
#include "core/fobosruntimeif.h"
#include <QStandardItemModel>
#include <QStyle>
#include <QStyleFactory>
#include <QTableWidget>
#include <QHeaderView>
#include "multivfowidget.h"

#include "appconstants.h"
#include "apphelp.h"
#include "appsettingsutils.h"
#include "diagnosticlogging.h"
#include "dspflowpanel.h"
#include "spaceeventsfeed.h"
#include "wirelesslabdialog.h"
#include "ffttuningdialog.h"
#include "multisdrdialog.h"
#include "spectrumfftworker.h"
#include "playbackmanager.h"

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
#include <QScrollArea>
#include <QScreen>
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

void YourClassName::syncSpectrumPersistenceControls() {
    if (!std::isfinite(spectrumPersistenceSeconds)) spectrumPersistenceSeconds = 1.6;
    spectrumPersistenceSeconds = (std::clamp)(spectrumPersistenceSeconds, 0.1, 30.0);
    spectrumPersistenceIntensity = (std::clamp)(spectrumPersistenceIntensity, 10, 400);
    spectrumPersistenceThreshold = (std::clamp)(spectrumPersistenceThreshold, 0, 90);
    spectrumPersistenceSmoothingMs = (std::clamp)(spectrumPersistenceSmoothingMs, 0, 2000);
    spectrumPersistenceStyle = (std::clamp)(spectrumPersistenceStyle, 0, 1);
    if (spectrumPersistenceStyleCombo) {
        const QSignalBlocker blocker(spectrumPersistenceStyleCombo);
        spectrumPersistenceStyleCombo->setCurrentIndex(spectrumPersistenceStyle);
        spectrumPersistenceStyleCombo->setEnabled(spectrumDisplayMode == 1);
    }
    if (spectrumPersistenceTimeSpin) {
        const QSignalBlocker blocker(spectrumPersistenceTimeSpin);
        spectrumPersistenceTimeSpin->setValue(spectrumPersistenceSeconds);
        spectrumPersistenceTimeSpin->setEnabled(spectrumDisplayMode == 1);
    }
    if (spectrumPersistenceIntensitySpin) {
        const QSignalBlocker blocker(spectrumPersistenceIntensitySpin);
        spectrumPersistenceIntensitySpin->setValue(spectrumPersistenceIntensity);
        spectrumPersistenceIntensitySpin->setEnabled(spectrumDisplayMode == 1);
    }
    if (spectrumPersistenceThresholdSpin) {
        const QSignalBlocker blocker(spectrumPersistenceThresholdSpin);
        spectrumPersistenceThresholdSpin->setValue(spectrumPersistenceThreshold);
        spectrumPersistenceThresholdSpin->setEnabled(spectrumDisplayMode == 1);
    }
    if (spectrumPersistenceSmoothingSpin) {
        const QSignalBlocker blocker(spectrumPersistenceSmoothingSpin);
        spectrumPersistenceSmoothingSpin->setValue(spectrumPersistenceSmoothingMs);
        spectrumPersistenceSmoothingSpin->setEnabled(spectrumDisplayMode == 1 && spectrumPersistenceStyle == 0);
    }
    if (graphWidget) graphWidget->setPersistenceStyle(spectrumPersistenceStyle);
    if (graphWidget) graphWidget->setPersistenceParameters(spectrumPersistenceSeconds, spectrumPersistenceIntensity, spectrumPersistenceThreshold, spectrumPersistenceSmoothingMs);
    refreshDspControlStates();
}

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
    showApplicationHelpDialog(parentWidget ? parentWidget : static_cast<QWidget*>(this), uiLanguage);
}

void YourClassName::openApplicationSettings() {
    if (auto *existing = findChild<QDialog *>(QStringLiteral("applicationSettingsDialog"))) {
        existing->show(); existing->raise(); existing->activateWindow(); return;
    }
    updateSpurSuppressionStatus();
    QDialog &dialog = *new QDialog(this);
    dialog.setObjectName(QStringLiteral("applicationSettingsDialog"));
    dialog.setAttribute(Qt::WA_DeleteOnClose);
    dialog.setModal(false);
    dialog.setWindowModality(Qt::NonModal);
    dialog.setWindowTitle(uiText(QStringLiteral("settings"), QStringLiteral("Settings...")));
    dialog.setMinimumWidth(420);

    auto *outerLayout = new QVBoxLayout(&dialog);
    auto *scrollArea = new QScrollArea(&dialog);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    auto *content = new QWidget(scrollArea);
    QVBoxLayout *rootLayout = new QVBoxLayout(content);
    scrollArea->setWidget(content);
    outerLayout->addWidget(scrollArea);
    dialog.resize(720, (std::min)(850, QApplication::primaryScreen()->availableGeometry().height() - 80));

    QFormLayout *generalLayout = new QFormLayout();
    QComboBox *languageCombo = new QComboBox(&dialog);
    populateLanguageCombo(languageCombo);
    languageCombo->setCurrentIndex(languageCombo->findData(uiLanguage));
    if (languageCombo->currentIndex() < 0) {
        languageCombo->setCurrentIndex(0);
    }
    const QString defaultStyle = qApp->property("obriiDefaultStyle").toString();
    auto *styleCombo = new QComboBox(&dialog);
    styleCombo->setObjectName(QStringLiteral("applicationStyleCombo"));
    styleCombo->addItem(uiText("ui_style_default", "Default (%1)").arg(defaultStyle), QString());
    QStringList styles = QStyleFactory::keys();
    styles.sort(Qt::CaseInsensitive);
    const QString savedStyle = QSettings(persistentSettingsFilePath(), QSettings::IniFormat)
                                   .value(QStringLiteral("ui/style")).toString();
    for (const QString &style : styles) {
        styleCombo->addItem(style, style);
        if (savedStyle.compare(style, Qt::CaseInsensitive) == 0)
            styleCombo->setCurrentIndex(styleCombo->count() - 1);
    }
    connect(styleCombo, QOverload<int>::of(&QComboBox::activated), &dialog,
            [this, styleCombo, defaultStyle](int) {
        const QString chosen = styleCombo->currentData().toString();
        const QString key = chosen.isEmpty() ? defaultStyle : chosen;
        if (QApplication::style()->objectName().compare(key, Qt::CaseInsensitive) != 0 &&
            !QApplication::setStyle(key)) {
            QMessageBox::warning(styleCombo, uiText("ui_style", "Interface style"),
                uiText("ui_style_unavailable", "Qt could not load the selected style."));
            const QString current = QApplication::style()->objectName();
            const int index = styleCombo->findText(current, Qt::MatchFixedString);
            styleCombo->setCurrentIndex(current.compare(defaultStyle, Qt::CaseInsensitive) == 0 ? 0 : index);
            return;
        }
        QSettings(persistentSettingsFilePath(), QSettings::IniFormat).setValue(QStringLiteral("ui/style"), chosen);
    });

    QComboBox *fineTuneModeCombo = new QComboBox(&dialog);
    fineTuneModeCombo->addItem(uiText(QStringLiteral("fine_tune_scale"), QStringLiteral("Horizontal scale (mouse wheel)")),
                               FINE_TUNE_MODE_SCALE);
    fineTuneModeCombo->addItem(uiText(QStringLiteral("fine_tune_dial"), QStringLiteral("Round dial")),
                               FINE_TUNE_MODE_DIAL);
    const int fineTuneIndex = fineTuneModeCombo->findData(fineTuneControlMode);
    fineTuneModeCombo->setCurrentIndex(fineTuneIndex >= 0 ? fineTuneIndex : 0);

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
        QStringLiteral("Minimum interval between live Fobos center-frequency commands in safe mode. Standard and Agile use their corresponding receiver libraries.")));

    const bool tuningUk = normalizedUiLanguage(uiLanguage) == QStringLiteral("uk");
    auto *retuneHeader = new QToolButton(&dialog);
    retuneHeader->setText(tuningUk ? QStringLiteral("Перестроювання Фобоса") : QStringLiteral("Fobos retuning"));
    retuneHeader->setObjectName(QStringLiteral("liveRetuneOptionsHeader"));
    retuneHeader->setCheckable(true);
    retuneHeader->setArrowType(Qt::RightArrow);
    retuneHeader->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    auto *retuneOptions = new QWidget(&dialog);
    retuneOptions->setObjectName(QStringLiteral("liveRetuneOptions"));
    retuneOptions->setToolTip(tuningUk
        ? QStringLiteral("Для звичайного RF-приймання зі Standard або Agile. Власна прошивка не потрібна. Сканування й прямі КХ-входи використовують свої режими керування.")
        : QStringLiteral("For normal RF reception with Standard or Agile. Custom firmware is not required. Scanning and direct HF inputs use their own control paths."));
    auto *retuneLayout = new QFormLayout(retuneOptions);
    auto *retuneMode = new QComboBox(retuneOptions);
    retuneMode->setObjectName(QStringLiteral("liveRetuneMode"));
    retuneMode->addItem(tuningUk ? QStringLiteral("Безпечний — після завершення руху")
                               : QStringLiteral("Safe — after movement stops"), false);
    retuneMode->addItem(tuningUk ? QStringLiteral("Плавний — під час руху")
                               : QStringLiteral("Continuous — while moving"), true);
    retuneMode->setCurrentIndex(agileContinuousRetuneEnabled ? 1 : 0);
    auto *retuneStep = new QDoubleSpinBox(retuneOptions);
    retuneStep->setObjectName(QStringLiteral("liveRetuneStepPercent"));
    retuneStep->setRange(0.1, 100.0);
    retuneStep->setDecimals(1);
    retuneStep->setSingleStep(0.5);
    retuneStep->setSuffix(QStringLiteral(" %"));
    retuneStep->setValue(agileContinuousRetuneStepPercent);
    retuneStep->setEnabled(agileContinuousRetuneEnabled);
    retuneStep->setToolTip(tuningUk
        ? QStringLiteral("Відсоток семплрейту: 5% при 50 MS/s = 2,5 МГц. Під час руху команди надсилаються з цим кроком; після зупинки — точна вибрана частота. Проміжні точки не накопичуються в черзі.")
        : QStringLiteral("Percent of sample rate: 5% at 50 MS/s = 2.5 MHz. Commands follow this step while moving; stopping applies the exact selected frequency. Intermediate targets are not queued."));
    agileLiveRetuneIntervalSpin->setEnabled(!agileContinuousRetuneEnabled);
    retuneLayout->addRow(tuningUk ? QStringLiteral("Режим") : QStringLiteral("Mode"), retuneMode);
    retuneLayout->addRow(tuningUk ? QStringLiteral("Крок від семплрейту") : QStringLiteral("Sample-rate step"), retuneStep);
    retuneLayout->addRow(tuningUk ? QStringLiteral("Інтервал у безпечному режимі") : QStringLiteral("Safe-mode interval"), agileLiveRetuneIntervalSpin);
    retuneOptions->hide();
    connect(retuneHeader, &QToolButton::toggled, &dialog, [retuneHeader, retuneOptions](bool expanded) {
        retuneHeader->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
        retuneOptions->setVisible(expanded);
    });
    connect(retuneMode, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog,
        [this, retuneMode, retuneStep, agileLiveRetuneIntervalSpin](int) {
            agileContinuousRetuneEnabled = retuneMode->currentData().toBool();
            retuneStep->setEnabled(agileContinuousRetuneEnabled);
            agileLiveRetuneIntervalSpin->setEnabled(!agileContinuousRetuneEnabled);
            if (queuedLiveCenterRetuneGeneration != 0 && !liveCenterRetuneThread)
                flushQueuedLiveAgileCenterRetune();
            savePersistentSettings();
        });
    connect(retuneStep, QOverload<double>::of(&QDoubleSpinBox::valueChanged), &dialog, [this](double percent) {
        agileContinuousRetuneStepPercent = percent;
        savePersistentSettings();
    });

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
    generalLayout->addRow(uiText("ui_style", "Interface style"), styleCombo);
    generalLayout->addRow(uiText(QStringLiteral("fine_tune"), QStringLiteral("Fine tune")), fineTuneModeCombo);
    generalLayout->addRow(uiText(QStringLiteral("scan_measurement_update_interval"), QStringLiteral("Measurement accumulation")), scanMeasurementUpdateSpin);
    generalLayout->addRow(retuneHeader);
    generalLayout->addRow(retuneOptions);
    generalLayout->addRow(helpButton);
    generalLayout->addRow(fobosDetailsButton);
    const bool dualUk = normalizedUiLanguage(uiLanguage) == QStringLiteral("uk");
    auto *dualLibraryBox = new QGroupBox(dualUk ? QStringLiteral("Бібліотека для Фобоса Dual")
                                                : QStringLiteral("Fobos Dual library"), &dialog);
    auto *dualLibraryLayout = new QVBoxLayout(dualLibraryBox);
    auto *dualLibraryCombo = new QComboBox(dualLibraryBox);
    dualLibraryCombo->setObjectName(QStringLiteral("fobosDualLibrary"));
    dualLibraryCombo->addItems(dualUk ? QStringList{QStringLiteral("Agile Обрію · AUTO DMA"),
        QStringLiteral("Штатна Agile"), QStringLiteral("Standard Обрію"), QStringLiteral("Standard RigExpert · виправлений Stop")}
        : QStringList{QStringLiteral("Obrii Agile · AUTO DMA"), QStringLiteral("Stock Agile"),
        QStringLiteral("Obrii Standard"), QStringLiteral("RigExpert Standard · Stop fix")});
    auto *dualLibraryStatus = new QLabel(dualLibraryBox);
    dualLibraryStatus->setWordWrap(true);
    dualLibraryLayout->addWidget(dualLibraryCombo);
    dualLibraryLayout->addWidget(dualLibraryStatus);
    generalLayout->addRow(dualLibraryBox);
    const auto updateDualLibrary = [this, dualLibraryBox, dualLibraryCombo, dualLibraryStatus, dualUk] {
        const auto info = selectedFobosDeviceInfo();
        const bool dual = !isNetworkClientMode() && !isExternalReceiverBackendSelected() && info.present &&
            !info.serial.isEmpty() && isObriiDualFirmware(info.hardwareRevision, info.firmwareVersion);
        dualLibraryBox->setVisible(dual || dualFobosSwitchPending);
        dualLibraryCombo->setEnabled(dual && isIdle() && !deviceOpened &&
            !(processor && processor->isRunning()) && !dualFobosSwitchPending);
        dualLibraryStatus->setText(!dualFobosSwitchStatus.isEmpty() ? dualFobosSwitchStatus :
            (dualUk ? QStringLiteral("Зупини приймання перед зміною. Бібліотеки сумісності завантажуються з fobos-compat.")
                    : QStringLiteral("Stop reception before changing libraries. Compatibility DLLs load from fobos-compat.")));
        if (!dual || dualFobosSwitchPending) return;
        if (auto *model = qobject_cast<QStandardItemModel *>(dualLibraryCombo->model())) {
            model->item(1)->setEnabled(fobosStockLibraryAvailable(FobosApiKind::Agile));
            model->item(3)->setEnabled(fobosStockLibraryAvailable(FobosApiKind::Standard));
        }
        const int apiIndex = info.apiKind == FobosApiKind::Standard ? 2 : 0;
        dualLibraryCombo->setCurrentIndex(apiIndex + (dualFobosStockLibrary.value(info.serial) ? 1 : 0));
    };
    updateDualLibrary();
    connect(dualLibraryCombo, QOverload<int>::of(&QComboBox::activated), &dialog, [this, updateDualLibrary](int index) {
        selectDualFobosLibrary(index >= 2 ? FobosApiKind::Standard : FobosApiKind::Agile, (index % 2) != 0);
        updateDualLibrary();
    });
    auto *publicSourcesButton = new QPushButton(uiText("public_data_sources", "Online data sources..."), &dialog);
    generalLayout->addRow(publicSourcesButton);
    connect(publicSourcesButton, &QPushButton::clicked, &dialog, [this, &dialog] {
        spaceevents::configureSources(&dialog, normalizedUiLanguage(uiLanguage) == QStringLiteral("uk"));
    });
    rootLayout->addLayout(generalLayout);

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
    QCheckBox *generalBandMarkersOption = new QCheckBox(uiText(QStringLiteral("general_band_markers"), QStringLiteral("Band markers")), quickOptionsBox);
    QCheckBox *amateurBandMarkersOption = new QCheckBox(uiText(QStringLiteral("amateur_band_markers"), QStringLiteral("HAM bands")), quickOptionsBox);
    QCheckBox *compactBandMarkersOption = new QCheckBox(uiText(QStringLiteral("compact_band_markers"), QStringLiteral("Collapsed bands")), quickOptionsBox);
    QCheckBox *gpuWaterfallOption = new QCheckBox(uiText(QStringLiteral("gpu_waterfall"), QStringLiteral("GPU waterfall")), quickOptionsBox);
    gpuWaterfallOption->setToolTip(uiText(
        QStringLiteral("gpu_waterfall_tooltip"),
        QStringLiteral("Experimental: prepare the waterfall for GPU-backed rendering. CPU texture rendering remains the safe fallback.")));
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
    QCheckBox *correctedFobosCommandsOption = new QCheckBox(
        uiText(QStringLiteral("corrected_fobos_commands"), QStringLiteral("Use corrected commands")),
        quickOptionsBox);
    correctedFobosCommandsOption->setObjectName(QStringLiteral("correctedFobosCommandsOption"));
    correctedFobosCommandsOption->setToolTip(uiText(
        QStringLiteral("corrected_fobos_commands_tooltip"),
        QStringLiteral("Use IF2350 in the affected 3.4-3.6 and 4.0-4.8 GHz ranges. Requires the optional Agile firmware extension or the Obrii standard host IF library. Stop reception and scanning before switching.")));
    unsigned int ifMode = 0;
    unsigned int ifState = 0;
    const bool ifExtensionAvailable = isIdle() && !(processor && processor->isRunning()) &&
        hasActiveFobosDevice() && getActiveFobosIfModeSafely( &ifMode, &ifState) == FOBOS_ERR_OK;
    correctedFobosCommandsOption->setChecked(useCorrectedFobosCommands);
    correctedFobosCommandsOption->setProperty("ifAvailable", ifExtensionAvailable);
    correctedFobosCommandsOption->setEnabled(ifExtensionAvailable && isIdle() && ifState == 0);
    auto *ifFirmwareStatus = new QLabel(ifExtensionAvailable && activeFobosApiKind == FobosApiKind::Standard
        ? uiText("fobos_standard_if_found", "Standard firmware: host IF library available; no firmware update required.")
        : ifExtensionAvailable
        ? (isIdle() && ifState == 0
            ? uiText("obrii_firmware_found", "Compatible Obrii firmware detected.")
            : uiText("obrii_firmware_busy", "Obrii firmware detected. Stop reception and scanning to change IF mode."))
        : !isIdle() || (processor && processor->isRunning())
        ? uiText("fobos_if_readout_stopped", "IF readback: available after Stop")
        : uiText("obrii_firmware_required", "Requires compatible Obrii firmware. See Feature guide: receiver firmware update."), quickOptionsBox);
    ifFirmwareStatus->setWordWrap(true);
    ifFirmwareStatus->setObjectName(QStringLiteral("ifFirmwareStatus"));
    QCheckBox *adcProtectionOption = new QCheckBox(
        uiText(QStringLiteral("adc_overload_protection"), QStringLiteral("Automatic ADC overload protection (VGA)")), quickOptionsBox);
    adcProtectionOption->setObjectName(QStringLiteral("adcOverloadProtectionOption"));
    adcProtectionOption->setChecked(adcOverloadProtectionEnabled);
    adcProtectionOption->setToolTip(uiText(QStringLiteral("adc_overload_protection_tooltip"),
        QStringLiteral("For local Fobos Agile RF reception: reduce VGA after sustained raw ADC saturation. Never raises gain. Disabled for HF and scanning. Does not prevent overload before the ADC; reduce LNA or use an external attenuator if needed.")));
    audioOption->setChecked(audioCheckbox && audioCheckbox->isChecked());
    syncOption->setChecked(syncCheckbox && syncCheckbox->isChecked());
    syncOption->setEnabled(false);
    syncOption->setToolTip(syncCheckbox ? syncCheckbox->toolTip() : QString());
    generalBandMarkersOption->setChecked(showGeneralBandMarkers);
    amateurBandMarkersOption->setChecked(showAmateurBandMarkers);
    compactBandMarkersOption->setChecked(compactBandMarkers);
    gpuWaterfallOption->setChecked(experimentalGpuWaterfall);
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
    quickOptionsLayout->addWidget(audioQueueDropRowsOption, 10, 0, 1, 3);
    quickOptionsLayout->addWidget(correctedFobosCommandsOption, 11, 0, 1, 3);
    quickOptionsLayout->addWidget(adcProtectionOption, 12, 0, 1, 3);
    quickOptionsLayout->addWidget(ifFirmwareStatus, 13, 0, 1, 3);
    rootLayout->addWidget(quickOptionsBox);

    auto *ifTestBox = new QGroupBox(uiText("fobos_if_test_title", "Fobos intermediate frequency"), content);
    auto *ifTestLayout = new QFormLayout(ifTestBox);
    ifTestLayout->setRowWrapPolicy(QFormLayout::WrapAllRows);
    auto *ifTestCombo = new QComboBox(ifTestBox);
    ifTestCombo->setObjectName(QStringLiteral("fobosIfTestMode"));
    ifTestCombo->addItem(uiText("fobos_if_test_native", "Use selected original/corrected plan"), 0);
    ifTestCombo->addItem(QStringLiteral("2350 MHz"), 1);
    ifTestCombo->addItem(QStringLiteral("2450 MHz"), 2);
    ifTestCombo->addItem(uiText("fobos_if_test_auto", "Automatic IF table (50 MS/s)"), 3);
    unsigned int actualIfTest = 0, ifTestState = 0, row3IfMhz = 0, ifCapabilities = 0;
    const bool ifTestAvailable = ifExtensionAvailable &&
        getActiveFobosIfPlanSafely( &actualIfTest, &ifTestState, &row3IfMhz, &ifCapabilities) == FOBOS_ERR_OK && (ifCapabilities & 4u);
    const bool autoIfAvailable = ifTestAvailable && (ifCapabilities & 2u);
    if (auto *model = qobject_cast<QStandardItemModel *>(ifTestCombo->model()))
        model->item(3)->setEnabled(autoIfAvailable);
    ifTestCombo->setCurrentIndex(fobosIfTestMode);
    ifTestCombo->setProperty("ifAvailable", ifTestAvailable);
    ifTestCombo->setEnabled(ifTestAvailable && isIdle() && ifTestState == 0);
    auto *ifTestHint = new QLabel(uiText("fobos_if_test_hint",
        "Agile requires revision6; stock firmware uses the optional host IF library. Manual2350/2450 applies to every converted RF band. RF bypass and HF direct inputs have no fixed IF. Auto policy2 requires49–51 MS/s. Stop reception/scanning before selecting a mode. IF2450 may admit Wi-Fi."), ifTestBox);
    ifTestHint->setWordWrap(true);
    ifTestLayout->addRow(uiText("fobos_if_test_choice", "Intermediate frequency"), ifTestCombo);
    ifTestLayout->addRow(ifTestHint);
    auto *ifActual = new QLabel(ifTestBox);
    ifActual->setObjectName(QStringLiteral("fobosActualIfSettings"));
    ifActual->setWordWrap(true);
    ifTestLayout->addRow(ifActual);

    auto *ifTableButton = new QPushButton(uiText("fobos_if_table_button", "IF table..."), ifTestBox);
    ifTableButton->setEnabled(autoIfAvailable && FobosRuntimeIf::HasFactoryPolicy);
    ifTestLayout->addRow(ifTableButton);
    auto *ifEditorButton = new QPushButton(uiText("fobos_if_edit_rules", uiLanguage == QStringLiteral("uk")
        ? QStringLiteral("Налаштувати правила ПЧ…") : QStringLiteral("Edit IF rules…")), ifTestBox);
    ifEditorButton->setObjectName(QStringLiteral("fobosEditIfRules"));
    ifEditorButton->setEnabled(ifTestAvailable && (ifCapabilities & 8u) && FobosRuntimeIf::HasFactoryPolicy);
    ifTestLayout->addRow(ifEditorButton);
    connect(ifEditorButton, &QPushButton::clicked, &dialog, [this] { openFobosIfEditor(); });

    connect(ifTableButton, &QPushButton::clicked, &dialog, [this, &dialog] {
        QDialog tableDialog(&dialog);
        tableDialog.setWindowTitle(uiText("fobos_if_table_title", "IF table · policy2"));
        tableDialog.resize(900, 560);
        auto *layout = new QVBoxLayout(&tableDialog);
        const bool uk = uiLanguage == QStringLiteral("uk");
        const auto activeFrame = FobosRuntimeIf::validFrame(fobosActiveIfRulesFrame)
            ? fobosActiveIfRulesFrame : FobosRuntimeIf::builtinFrame();
        const auto activeRules = FobosRuntimeIf::rules(activeFrame);
        const bool customTable = fobosCustomIfRulesActive && FobosRuntimeIf::validFrame(fobosActiveIfRulesFrame);
        const double maximum = activeFobosApiKind == FobosApiKind::Standard ? RF_EXPERIMENTAL_MAX_FREQUENCY : 6900e6;
        QString explanation = (uk
            ? QStringLiteral("Інтервали [від; до), верхня апаратна межа %1 МГц включена. Авто використовує базовий план поза правилами та при Fs поза49–51 MS/s. Ручна ПЧ має пріоритет.")
            : QStringLiteral("Intervals are [from; to); the receiver limit%1 MHz is included. Auto retains the base plan outside rules and49–51 MS/s. Manual IF takes priority."))
            .arg(maximum / 1e6, 0, 'f', 0);
        if (customTable) explanation += (uk
            ? QStringLiteral("\nВласна таблиця, CRC32 %1. Її правила потребують власного A/B-порівняння; позначки перевірених вбудованих правил на неї не переносяться.")
            : QStringLiteral("\nCustom table, CRC32%1. These rules require their own A/B test; built-in qualification labels do not carry over."))
            .arg(QString::number(FobosRuntimeIf::rulesCrc(activeFrame), 16).rightJustified(8, QLatin1Char('0')));
        else explanation += (uk
            ? QStringLiteral("\nВбудована таблиця: нові правила за спостереженнями/моделлю потребують A/B-перевірки. На2929–2946 МГц завади можливі з обома ПЧ.")
            : QStringLiteral("\nBuilt-in table: new survey/model rules require A/B verification. Both IFs may be contaminated at2929–2946 MHz."));
        auto *note = new QLabel(explanation, &tableDialog);
        note->setWordWrap(true); layout->addWidget(note);
        QVector<double> cuts = {50e6,2200.5e6,2300.5e6,2550.5e6,3000.5e6,3100.5e6,
            3200.5e6,3400.5e6,3600.5e6,4000.5e6,4800.5e6,maximum + 1.0};
        for (const auto &rule : activeRules) {
            if (rule.fromHz > 50e6 && rule.fromHz <= maximum) cuts.append(rule.fromHz);
            if (rule.toHz > 50e6 && rule.toHz <= maximum + 1.0) cuts.append(rule.toHz);
        }
        std::sort(cuts.begin(),cuts.end());
        cuts.erase(std::unique(cuts.begin(),cuts.end()),cuts.end());
        auto *table = new QTableWidget(cuts.size()-1,5,&tableDialog);
        table->setHorizontalHeaderLabels({uiText("fobos_if_table_rf", "Center MHz"),
            uiText("fobos_if_table_original", "Original IF"),uiText("fobos_if_table_corrected", "Corrected IF"),
            uiText("fobos_if_table_auto_column", "Auto / chosen plan"),uiText("fobos_if_table_evidence", "Basis")});
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->verticalHeader()->hide();
        for (int index=0; index+1<cuts.size(); ++index) {
            const double middle=(std::min)((cuts[index]+cuts[index+1])*0.5,maximum);
            const auto text = [this](int mhz) { return mhz ? QString::number(mhz) : uiText("fobos_if_table_bypass", "RF bypass"); };
            QString basis=uiText("fobos_if_table_base", "Existing base plan");
            bool hasOverride = false;
            for (const auto &rule : activeRules)
                if (middle >= rule.fromHz && middle < rule.toHz) hasOverride = true;
            if (hasOverride) {
                if (customTable) basis = uk ? QStringLiteral("Власне правило; потрібен A/B-тест")
                                            : QStringLiteral("Custom rule; A/B test required");
                else {
                    basis=uiText("fobos_if_table_provisional", "Survey/model; awaiting A/B test");
#ifdef OBRII_LOCAL_FOBOS_IF_HEADER
                    if (middle>=OBRII_IF_AUTO_FROM_HZ && middle<OBRII_IF_AUTO_TO_HZ)
                        basis=uiText("fobos_if_table_measured", "Measured LO6 bypass");
#endif
                    if (middle>=2929e6 && middle<2946e6)
                        basis=uiText("fobos_if_table_conflict", "Both IFs may have interference");
                }
            }
            const QStringList values={QStringLiteral("%1–%2").arg(cuts[index]/1e6,0,'f',6).arg(cuts[index+1]/1e6,0,'f',6),
                text(obriiIfBaseMhz(middle,false)),text(obriiIfBaseMhz(middle,true)),
                text(FobosRuntimeIf::chooseEffectiveIf(activeFrame,middle,50e6,useCorrectedFobosCommands,3)),basis};
            for (int column=0; column<values.size(); ++column)
                table->setItem(index,column,new QTableWidgetItem(values[column]));
        }
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        table->horizontalHeader()->setStretchLastSection(true);
        layout->addWidget(table,1);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close,&tableDialog);
        connect(buttons,&QDialogButtonBox::rejected,&tableDialog,&QDialog::reject);
        layout->addWidget(buttons); tableDialog.exec();
    });
    rootLayout->addWidget(ifTestBox);
    rootLayout->addWidget(createFobosRuntimeControls(content));

    auto *spurBox = new QGroupBox(uiText("spur_settings", "Internal spur suppression"), content);
    auto *spurLayout = new QVBoxLayout(spurBox);
    auto *spurEnabled = new QCheckBox(spurSuppressionCheckbox->text(), spurBox);
    spurEnabled->setObjectName(QStringLiteral("spurSuppressionOption"));
    spurEnabled->setChecked(spurSuppressionEnabled);
    spurEnabled->setToolTip(spurSuppressionCheckbox->toolTip());
    auto *spurStatus = new QLabel(spurSuppressionStatusLabel->text(), spurBox);
    spurStatus->setWordWrap(true);
    auto *spurButtons = new QHBoxLayout;
    auto *spurCalibrate = new QPushButton(uiText("spur_calibrate_full", "Calibrate full RF range..."), spurBox);
    auto *spurClear = new QPushButton(spurClearButton->text(), spurBox);
    spurButtons->addWidget(spurCalibrate);
    spurButtons->addWidget(spurClear);
    spurLayout->addWidget(spurEnabled);
    auto *spurThreshold = new QDoubleSpinBox(spurBox);
    spurThreshold->setObjectName(QStringLiteral("spurDetectionThreshold"));
    spurThreshold->setRange(3.0, 30.0); spurThreshold->setDecimals(1); spurThreshold->setSuffix(QStringLiteral(" dB"));
    spurThreshold->setValue(spurDetectionThresholdDb); spurThreshold->setKeyboardTracking(false);
    spurThreshold->setToolTip(uiText("spur_threshold_hint", "Minimum calibrated prominence above the local floor. Higher values keep only stronger spurs immediately. Lower values require recalibration to discover additional weak lines; averaging confidence also applies."));
    auto *spurThresholdForm = new QFormLayout;
    spurThresholdForm->addRow(uiText("spur_threshold", "Spur detection threshold"), spurThreshold);
    spurLayout->addLayout(spurThresholdForm);
    spurLayout->addWidget(spurStatus);
    spurLayout->addLayout(spurButtons);
    rootLayout->addWidget(spurBox);
    connect(spurThreshold, QOverload<double>::of(&QDoubleSpinBox::valueChanged), &dialog, [this](double value) {
        spurDetectionThresholdDb = value; updateSpurSuppressionStatus(); savePersistentSettings();
    });
    connect(spurEnabled, &QCheckBox::toggled, spurSuppressionCheckbox, &QCheckBox::setChecked);
    connect(spurCalibrate, &QPushButton::clicked, &dialog, [this, spurEnabled, spurStatus] {
        startSpurCalibration();
        const QSignalBlocker blocker(spurEnabled);
        spurEnabled->setChecked(spurSuppressionEnabled);
        spurStatus->setText(spurSuppressionStatusLabel->text());
    });
    connect(spurClear, &QPushButton::clicked, &dialog, [this, spurEnabled, spurStatus] {
        clearSpurMask();
        const QSignalBlocker blocker(spurEnabled);
        spurEnabled->setChecked(spurSuppressionEnabled);
        spurStatus->setText(spurSuppressionStatusLabel->text());
    });
    connect(spurEnabled, &QCheckBox::toggled, &dialog, [this, spurStatus] {
        spurStatus->setText(spurSuppressionStatusLabel->text());
    });

    auto *noiseBox = new QGroupBox(uiText("receiver_noise_shape", "Broad noise shape (VGA / receiver)"), content);
    auto *noiseLayout = new QVBoxLayout(noiseBox);
    auto *noiseEnabled = new QCheckBox(uiText("receiver_noise_shape_enable", "Suppress calibrated broad noise shape"), noiseBox);
    noiseEnabled->setChecked(receiverNoiseBaselineMode == static_cast<int>(ReceiverNoiseBaselineMode::WideShape));
    noiseEnabled->setObjectName(QStringLiteral("receiverNoiseShapeOption"));
    auto *noiseStrength = new QSpinBox(noiseBox);
    noiseStrength->setRange(0, 100); noiseStrength->setSuffix(QStringLiteral(" %"));
    noiseStrength->setValue(static_cast<int>(receiverNoiseShapeStrength * 100));
    auto *noiseStrengthForm = new QFormLayout;
    noiseStrengthForm->addRow(uiText("receiver_noise_shape_strength", "Broad-shape suppression"), noiseStrength);
    auto *noiseDetails = new QPushButton(uiText("receiver_noise_shape_settings", "Profile and broad-noise settings..."), noiseBox);
    auto *noiseStatus = new QLabel(noiseBox); noiseStatus->setWordWrap(true);
    const auto refreshNoiseStatus = [this, noiseStatus] {
        const auto key = receiverNoiseBaselineKey(spectrumProcessingSettings());
        noiseStatus->setText(receiverNoiseBaseline.hasMatchingProfile(key, true)
            ? uiText("receiver_noise_shape_ready", "A matching broad profile is available for this gain and RF region.")
            : uiText("receiver_noise_shape_missing", "No broad profile for this gain/RF region. Capture it with a50-ohm load, or repeat spur calibration. FFT/window changes are accepted; gain changes need a matching profile."));
    };
    noiseLayout->addWidget(noiseEnabled); noiseLayout->addLayout(noiseStrengthForm);
    noiseLayout->addWidget(noiseStatus); noiseLayout->addWidget(noiseDetails);
    rootLayout->addWidget(noiseBox);
    refreshNoiseStatus();
    connect(spurCalibrate, &QPushButton::clicked, &dialog, [refreshNoiseStatus] { refreshNoiseStatus(); });
    connect(noiseEnabled, &QCheckBox::toggled, &dialog, [this](bool checked) {
        receiverNoiseBaselineMode = static_cast<int>(checked ? ReceiverNoiseBaselineMode::WideShape : ReceiverNoiseBaselineMode::Off);
        savePersistentSettings();
    });
    connect(noiseStrength, QOverload<int>::of(&QSpinBox::valueChanged), &dialog, [this](int value) {
        receiverNoiseShapeStrength = value / 100.0; savePersistentSettings();
    });
    connect(noiseDetails, &QPushButton::clicked, &dialog, [this, noiseEnabled, noiseStrength, refreshNoiseStatus] {
        openReceiverNoiseBaselineDialog();
        const QSignalBlocker a(noiseEnabled), b(noiseStrength);
        noiseEnabled->setChecked(receiverNoiseBaselineMode == static_cast<int>(ReceiverNoiseBaselineMode::WideShape));
        noiseStrength->setValue(static_cast<int>(receiverNoiseShapeStrength * 100));
        refreshNoiseStatus();
    });

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
    connect(languageCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog, [applyLanguage](int) {
        applyLanguage();
    });
    connect(fineTuneModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog, [applyFineTuneMode](int) {
        applyFineTuneMode();
    });
    connect(scanMeasurementUpdateSpin, QOverload<int>::of(&QSpinBox::valueChanged), &dialog, [applyScanMeasurementUpdateInterval](int) {
        applyScanMeasurementUpdateInterval();
    });
    connect(agileLiveRetuneIntervalSpin, QOverload<int>::of(&QSpinBox::valueChanged), &dialog, [applyAgileLiveRetuneInterval](int) {
        applyAgileLiveRetuneInterval();
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

    connect(correctedFobosCommandsOption, &QCheckBox::toggled, &dialog,
            [this, correctedFobosCommandsOption, &dialog](bool checked) {
        const bool previous = useCorrectedFobosCommands;
        if (checked == previous) return;
        if (!isIdle() || !hasActiveFobosDevice()) {
            const QSignalBlocker blocker(correctedFobosCommandsOption);
            correctedFobosCommandsOption->setChecked(previous);
            return;
        }
        useCorrectedFobosCommands = checked;
        if (!prepareFobosSessionFromSettings(QStringLiteral("IF mode change"))) {
            useCorrectedFobosCommands = previous;
            hardwareSettingsApplied = false;
            const bool restored = prepareFobosSessionFromSettings(QStringLiteral("IF mode rollback"));
            qWarning() << "[Fobos IF] mode change failed; rollback" << restored;
            const QSignalBlocker blocker(correctedFobosCommandsOption);
            correctedFobosCommandsOption->setChecked(previous);
            QMessageBox::warning(&dialog, uiText(QStringLiteral("error"), QStringLiteral("Error")),
                uiText(QStringLiteral("corrected_fobos_commands_failed"),
                       QStringLiteral("Could not change the receiver IF mode. Stop reception and scanning, and check the compatible Agile firmware and library.")));
            return;
        }
        savePersistentSettings();
    });
    connect(ifTestCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog,
            [this, ifTestCombo, &dialog](int index) {
        const int previous = fobosIfTestMode;
        const int requested = ifTestCombo->itemData(index).toInt();
        if (requested == previous) return;
        if (!isIdle() || !hasActiveFobosDevice()) {
            const QSignalBlocker blocker(ifTestCombo);
            ifTestCombo->setCurrentIndex(previous);
            return;
        }
        fobosIfTestMode = requested;
        if (!prepareFobosSessionFromSettings(QStringLiteral("global IF selection"))) {
            fobosIfTestMode = previous;
            hardwareSettingsApplied = false;
            const bool restored = prepareFobosSessionFromSettings(QStringLiteral("global IF rollback"));
            qWarning() << "[Fobos IF] test mode change failed; rollback" << restored;
            const QSignalBlocker blocker(ifTestCombo);
            ifTestCombo->setCurrentIndex(previous);
            QMessageBox::warning(&dialog, uiText("error", "Error"),
                uiText("fobos_if_test_failed", "Could not select the test IF. Stop reception/scanning and check Obrii firmware revision4+ and the matching Agile library."));
            return;
        }
        updateSpurSuppressionStatus();
        savePersistentSettings();
    });
    connect(adcProtectionOption, &QCheckBox::toggled, &dialog, [this](bool enabled) {
        adcOverloadProtectionEnabled = enabled;
        adcOverloadGuard.reset();
        savePersistentSettings();
    });

    QDialogButtonBox *buttonBox = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    if (QPushButton *closeButton = buttonBox->button(QDialogButtonBox::Close)) {
        closeButton->setText(uiText(QStringLiteral("close"), QStringLiteral("Close")));
    }
    outerLayout->addWidget(buttonBox);

    connect(buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    auto *availabilityTimer = new QTimer(&dialog);
    connect(availabilityTimer, &QTimer::timeout, &dialog, updateDualLibrary);
    connect(availabilityTimer, &QTimer::timeout, &dialog,
        [this, correctedFobosCommandsOption, ifTestCombo, ifFirmwareStatus] {
            const bool idle = isIdle() && !(processor && processor->isRunning()) &&
                hasActiveFobosDevice() && !isNetworkClientMode();
            const int ticks = correctedFobosCommandsOption->property("probeTicks").toInt() + 1;
            correctedFobosCommandsOption->setProperty("probeTicks", ticks % 4);
            if (idle && ticks >= 4) {
                unsigned int mode=0, state=0, mhz=0, capabilities=0;
                const bool extension = getActiveFobosIfModeSafely(&mode, &state) == FOBOS_ERR_OK;
                const bool plan = extension && getActiveFobosIfPlanSafely(&mode, &state, &mhz, &capabilities) == FOBOS_ERR_OK;
                correctedFobosCommandsOption->setProperty("ifAvailable", extension && state == 0);
                ifTestCombo->setProperty("ifAvailable", plan && state == 0 && (capabilities & 4u));
                if (auto *model=qobject_cast<QStandardItemModel *>(ifTestCombo->model()))
                    model->item(3)->setEnabled(plan && (capabilities & 2u));
                ifFirmwareStatus->setText(extension
                    ? (activeFobosApiKind == FobosApiKind::Standard
                        ? uiText("fobos_standard_if_found", "Standard firmware: host IF library available; no firmware update required.")
                        : uiText("obrii_firmware_found", "Compatible Obrii firmware detected."))
                    : uiText("obrii_firmware_required", "Requires compatible Obrii firmware. See Feature guide: receiver firmware update."));
            }
            correctedFobosCommandsOption->setEnabled(idle && correctedFobosCommandsOption->property("ifAvailable").toBool());
            ifTestCombo->setEnabled(idle && ifTestCombo->property("ifAvailable").toBool());
        });
    availabilityTimer->start(250);
    dialog.show();
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

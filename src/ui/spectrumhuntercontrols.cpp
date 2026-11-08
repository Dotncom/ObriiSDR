#include "spectrumhuntercontrols.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSizePolicy>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

SpectrumHunterControls::SpectrumHunterControls(const QString &title,
                                               const QString &detectTooltip,
                                               const QString &widthSuffix,
                                               double minWidthRange,
                                               double maxWidthRange,
                                               double minWidthValue,
                                               double maxWidthValue,
                                               double minThresholdDb,
                                               double maxThresholdDb,
                                               double thresholdDb,
                                               QWidget *parent)
    : QGroupBox(title, parent) {
    detectCheckbox = new QCheckBox(QStringLiteral("Detect"), this);
    detectCheckbox->setToolTip(detectTooltip);

    presetCombo = new QComboBox(this);
    presetCombo->setMinimumContentsLength(8);
    presetCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    presetCombo->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    applyPresetButton = new QPushButton(QStringLiteral("Use scan"), this);
    applyPresetButton->setToolTip(QStringLiteral("Copy this preset into Agile scan ranges and step"));
    applyPresetButton->setMaximumWidth(82);
    tuneButton = new QPushButton(QStringLiteral("Tune"), this);
    tuneButton->setToolTip(QStringLiteral("Tune to the best detected candidate center"));
    tuneButton->setMaximumWidth(58);
    followCheckbox = new QCheckBox(QStringLiteral("Follow"), this);
    followCheckbox->setToolTip(QStringLiteral("Keep tuning to the selected detected candidate as the scan updates"));
    previousCandidateButton = new QPushButton(QStringLiteral("<"), this);
    previousCandidateButton->setToolTip(QStringLiteral("Tune to previous detected candidate"));
    previousCandidateButton->setFixedWidth(28);
    nextCandidateButton = new QPushButton(QStringLiteral(">"), this);
    nextCandidateButton->setToolTip(QStringLiteral("Tune to next detected candidate"));
    nextCandidateButton->setFixedWidth(28);
    candidateIndexLabel = new QLabel(QStringLiteral("-/-"), this);
    candidateIndexLabel->setAlignment(Qt::AlignCenter);
    candidateIndexLabel->setMinimumWidth(34);
    candidateIndexLabel->setMaximumWidth(46);

    minWidthSpin = new QDoubleSpinBox(this);
    minWidthSpin->setRange(minWidthRange, maxWidthRange);
    minWidthSpin->setDecimals(widthSuffix.contains(QStringLiteral("kHz")) ? 1 : 1);
    minWidthSpin->setSingleStep(widthSuffix.contains(QStringLiteral("kHz")) ? 0.5 : 0.5);
    minWidthSpin->setSuffix(widthSuffix);
    minWidthSpin->setValue(minWidthValue);
    minWidthSpin->setMaximumWidth(98);

    maxWidthSpin = new QDoubleSpinBox(this);
    maxWidthSpin->setRange(minWidthRange, maxWidthRange);
    maxWidthSpin->setDecimals(widthSuffix.contains(QStringLiteral("kHz")) ? 1 : 1);
    maxWidthSpin->setSingleStep(widthSuffix.contains(QStringLiteral("kHz")) ? 1.0 : 1.0);
    maxWidthSpin->setSuffix(widthSuffix);
    maxWidthSpin->setValue(maxWidthValue);
    maxWidthSpin->setMaximumWidth(98);

    thresholdSpin = new QDoubleSpinBox(this);
    thresholdSpin->setRange(minThresholdDb, maxThresholdDb);
    thresholdSpin->setDecimals(1);
    thresholdSpin->setSingleStep(0.5);
    thresholdSpin->setSuffix(QStringLiteral(" dB"));
    thresholdSpin->setValue(thresholdDb);
    thresholdSpin->setMaximumWidth(88);

    statusLabel = new QLabel(QStringLiteral("%1: off").arg(title), this);
    statusLabel->setWordWrap(true);
    statusLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    statusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

    autoScanWidget = new QWidget(this);
    autoScanCheckbox = new QCheckBox(QStringLiteral("Auto scan + decode"), autoScanWidget);
    autoScanCheckbox->setToolTip(
        QStringLiteral("Scan the selected preset, stop on a stable analog video carrier, "
                       "verify sync, then resume scanning after the signal is lost"));
    autoScanDwellLabel = new QLabel(QStringLiteral("Dwell:"), autoScanWidget);
    autoScanDwellSpin = new QSpinBox(autoScanWidget);
    autoScanDwellSpin->setRange(50, 5000);
    autoScanDwellSpin->setSuffix(QStringLiteral(" ms"));
    autoScanDwellSpin->setValue(650);
    autoScanDwellSpin->setMaximumWidth(92);
    autoScanLossLabel = new QLabel(QStringLiteral("Loss:"), autoScanWidget);
    autoScanLossSpin = new QSpinBox(autoScanWidget);
    autoScanLossSpin->setRange(500, 30000);
    autoScanLossSpin->setSuffix(QStringLiteral(" ms"));
    autoScanLossSpin->setValue(2500);
    autoScanLossSpin->setMaximumWidth(100);
    autoScanLockLabel = new QLabel(QStringLiteral("HSync:"), autoScanWidget);
    autoScanLockSpin = new QSpinBox(autoScanWidget);
    autoScanLockSpin->setRange(1, 100);
    autoScanLockSpin->setSuffix(QStringLiteral(" %"));
    autoScanLockSpin->setValue(45);
    autoScanLockSpin->setMaximumWidth(76);
    autoScanStatusLabel = new QLabel(QStringLiteral("Auto scan: off"), autoScanWidget);
    autoScanStatusLabel->setWordWrap(true);
    autoScanStatusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto *autoScanLayout = new QGridLayout(autoScanWidget);
    autoScanLayout->setContentsMargins(0, 0, 0, 0);
    autoScanLayout->setHorizontalSpacing(4);
    autoScanLayout->setVerticalSpacing(3);
    autoScanLayout->addWidget(autoScanCheckbox, 0, 0, 1, 2);
    autoScanLayout->addWidget(autoScanDwellLabel, 0, 2);
    autoScanLayout->addWidget(autoScanDwellSpin, 0, 3);
    autoScanLayout->addWidget(autoScanLossLabel, 1, 0);
    autoScanLayout->addWidget(autoScanLossSpin, 1, 1);
    autoScanLayout->addWidget(autoScanLockLabel, 1, 2);
    autoScanLayout->addWidget(autoScanLockSpin, 1, 3);
    autoScanLayout->addWidget(autoScanStatusLabel, 2, 0, 1, 4);
    autoScanLayout->setColumnStretch(1, 1);
    autoScanLayout->setColumnStretch(3, 1);
    autoScanWidget->setVisible(false);

    auto *mainLayout = new QVBoxLayout(this);
    auto *presetLayout = new QHBoxLayout();
    auto *tuningLayout = new QGridLayout();
    auto *candidateLayout = new QHBoxLayout();

    presetLayout->addWidget(detectCheckbox);
    presetLayout->addWidget(presetCombo, 1);
    presetLayout->addWidget(applyPresetButton);

    candidateLayout->setContentsMargins(0, 0, 0, 0);
    candidateLayout->setSpacing(4);

    minWidthLabel = new QLabel(QStringLiteral("Min W:"), this);
    maxWidthLabel = new QLabel(QStringLiteral("Max W:"), this);
    thresholdLabel = new QLabel(QStringLiteral("Thr:"), this);

    tuningLayout->setHorizontalSpacing(4);
    tuningLayout->setVerticalSpacing(3);
    tuningLayout->addWidget(minWidthLabel, 0, 0);
    tuningLayout->addWidget(minWidthSpin, 0, 1);
    tuningLayout->addWidget(maxWidthLabel, 0, 2);
    tuningLayout->addWidget(maxWidthSpin, 0, 3);
    tuningLayout->addWidget(thresholdLabel, 1, 0);
    tuningLayout->addWidget(thresholdSpin, 1, 1);
    candidateLayout->addWidget(previousCandidateButton);
    candidateLayout->addWidget(candidateIndexLabel);
    candidateLayout->addWidget(nextCandidateButton);
    candidateLayout->addSpacing(4);
    candidateLayout->addWidget(followCheckbox);
    candidateLayout->addWidget(tuneButton);
    candidateLayout->addStretch(1);
    tuningLayout->addLayout(candidateLayout, 1, 2, 1, 2);
    tuningLayout->setColumnStretch(1, 1);
    tuningLayout->setColumnStretch(3, 1);

    mainLayout->addLayout(presetLayout);
    mainLayout->addLayout(tuningLayout);
    mainLayout->addWidget(autoScanWidget);
    mainLayout->addWidget(statusLabel);

    connect(detectCheckbox, &QCheckBox::toggled, this, &SpectrumHunterControls::detectToggled);
    connect(minWidthSpin,
            QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this,
            &SpectrumHunterControls::minWidthChanged);
    connect(maxWidthSpin,
            QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this,
            &SpectrumHunterControls::maxWidthChanged);
    connect(thresholdSpin,
            QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this,
            &SpectrumHunterControls::thresholdChanged);
    connect(applyPresetButton, &QPushButton::clicked, this, &SpectrumHunterControls::applyPresetRequested);
    connect(tuneButton, &QPushButton::clicked, this, &SpectrumHunterControls::tuneRequested);
    connect(followCheckbox, &QCheckBox::toggled, this, &SpectrumHunterControls::followToggled);
    connect(previousCandidateButton, &QPushButton::clicked, this, &SpectrumHunterControls::previousCandidateRequested);
    connect(nextCandidateButton, &QPushButton::clicked, this, &SpectrumHunterControls::nextCandidateRequested);
    connect(autoScanCheckbox, &QCheckBox::toggled, this, &SpectrumHunterControls::autoScanToggled);
    connect(autoScanDwellSpin,
            QOverload<int>::of(&QSpinBox::valueChanged),
            this,
            &SpectrumHunterControls::autoScanDwellChanged);
    connect(autoScanLossSpin,
            QOverload<int>::of(&QSpinBox::valueChanged),
            this,
            &SpectrumHunterControls::autoScanLossTimeoutChanged);
    connect(autoScanLockSpin,
            QOverload<int>::of(&QSpinBox::valueChanged),
            this,
            &SpectrumHunterControls::autoScanMinimumLockChanged);

    setCandidateNavigationVisible(false);
    setCandidateNavigationEnabled(false);
    setFollowVisible(false);
    setFollowEnabled(false);
}

void SpectrumHunterControls::addPreset(const QString &name, const QString &spec) {
    presetCombo->addItem(name, spec);
}

QString SpectrumHunterControls::currentPresetSpec() const {
    return presetCombo ? presetCombo->currentData().toString() : QString();
}

void SpectrumHunterControls::setDetectChecked(bool checked) {
    if (!detectCheckbox || detectCheckbox->isChecked() == checked) {
        return;
    }
    QSignalBlocker blocker(detectCheckbox);
    detectCheckbox->setChecked(checked);
}

void SpectrumHunterControls::setWidthValues(double minWidth, double maxWidth, double thresholdDb) {
    if (minWidthSpin) {
        QSignalBlocker blocker(minWidthSpin);
        minWidthSpin->setValue(minWidth);
    }
    if (maxWidthSpin) {
        QSignalBlocker blocker(maxWidthSpin);
        maxWidthSpin->setValue(maxWidth);
    }
    if (thresholdSpin) {
        QSignalBlocker blocker(thresholdSpin);
        thresholdSpin->setValue(thresholdDb);
    }
}

void SpectrumHunterControls::setControlsEnabled(bool enabled) {
    if (minWidthSpin) {
        minWidthSpin->setEnabled(enabled);
    }
    if (maxWidthSpin) {
        maxWidthSpin->setEnabled(enabled);
    }
    if (thresholdSpin) {
        thresholdSpin->setEnabled(enabled);
    }
}

void SpectrumHunterControls::setTuneEnabled(bool enabled) {
    if (tuneButton) {
        tuneButton->setEnabled(enabled);
    }
}

void SpectrumHunterControls::setFollowVisible(bool visible) {
    if (followCheckbox) {
        followCheckbox->setVisible(visible);
    }
}

void SpectrumHunterControls::setFollowChecked(bool checked) {
    if (!followCheckbox || followCheckbox->isChecked() == checked) {
        return;
    }
    QSignalBlocker blocker(followCheckbox);
    followCheckbox->setChecked(checked);
}

void SpectrumHunterControls::setFollowEnabled(bool enabled) {
    if (followCheckbox) {
        followCheckbox->setEnabled(enabled);
    }
}

void SpectrumHunterControls::setCandidateNavigationVisible(bool visible) {
    if (previousCandidateButton) {
        previousCandidateButton->setVisible(visible);
    }
    if (nextCandidateButton) {
        nextCandidateButton->setVisible(visible);
    }
    if (candidateIndexLabel) {
        candidateIndexLabel->setVisible(visible);
    }
}

void SpectrumHunterControls::setCandidateNavigationEnabled(bool enabled) {
    if (previousCandidateButton) {
        previousCandidateButton->setEnabled(enabled);
    }
    if (nextCandidateButton) {
        nextCandidateButton->setEnabled(enabled);
    }
}

void SpectrumHunterControls::setCandidateIndex(int index, int count) {
    if (!candidateIndexLabel) {
        return;
    }
    if (count <= 0 || index < 0) {
        candidateIndexLabel->setText(QStringLiteral("-/-"));
        return;
    }
    candidateIndexLabel->setText(QStringLiteral("%1/%2").arg(index + 1).arg(count));
}

void SpectrumHunterControls::setStatusText(const QString &text) {
    if (statusLabel) {
        statusLabel->setText(text);
    }
}

void SpectrumHunterControls::setAutoScanVisible(bool visible) {
    if (autoScanWidget) {
        autoScanWidget->setVisible(visible);
    }
}

void SpectrumHunterControls::setAutoScanChecked(bool checked) {
    if (!autoScanCheckbox || autoScanCheckbox->isChecked() == checked) {
        return;
    }
    const QSignalBlocker blocker(autoScanCheckbox);
    autoScanCheckbox->setChecked(checked);
}

void SpectrumHunterControls::setAutoScanValues(int dwellMs,
                                               int lossTimeoutMs,
                                               int minimumLockPercent) {
    if (autoScanDwellSpin) {
        const QSignalBlocker blocker(autoScanDwellSpin);
        autoScanDwellSpin->setValue(dwellMs);
    }
    if (autoScanLossSpin) {
        const QSignalBlocker blocker(autoScanLossSpin);
        autoScanLossSpin->setValue(lossTimeoutMs);
    }
    if (autoScanLockSpin) {
        const QSignalBlocker blocker(autoScanLockSpin);
        autoScanLockSpin->setValue(minimumLockPercent);
    }
}

void SpectrumHunterControls::setAutoScanStatusText(const QString &text) {
    if (autoScanStatusLabel) {
        autoScanStatusLabel->setText(text);
    }
}

void SpectrumHunterControls::setAutoScanUiText(const QString &enabledText,
                                               const QString &enabledTooltip,
                                               const QString &dwellText,
                                               const QString &lossText,
                                               const QString &lockText) {
    if (autoScanCheckbox) {
        autoScanCheckbox->setText(enabledText);
        autoScanCheckbox->setToolTip(enabledTooltip);
    }
    if (autoScanDwellLabel) {
        autoScanDwellLabel->setText(dwellText);
    }
    if (autoScanLossLabel) {
        autoScanLossLabel->setText(lossText);
    }
    if (autoScanLockLabel) {
        autoScanLockLabel->setText(lockText);
    }
}

void SpectrumHunterControls::setUiText(const QString &title,
                                       const QString &detectTooltip,
                                       const QString &detectText,
                                       const QString &useScanText,
                                       const QString &useScanTooltip,
                                       const QString &tuneText,
                                       const QString &tuneTooltip,
                                       const QString &followText,
                                       const QString &followTooltip,
                                       const QString &previousTooltip,
                                       const QString &nextTooltip,
                                       const QString &minWidthText,
                                       const QString &maxWidthText,
                                       const QString &thresholdText) {
    setTitle(title);
    if (detectCheckbox) {
        detectCheckbox->setText(detectText);
        detectCheckbox->setToolTip(detectTooltip);
    }
    if (applyPresetButton) {
        applyPresetButton->setText(useScanText);
        applyPresetButton->setToolTip(useScanTooltip);
    }
    if (tuneButton) {
        tuneButton->setText(tuneText);
        tuneButton->setToolTip(tuneTooltip);
    }
    if (followCheckbox) {
        followCheckbox->setText(followText);
        followCheckbox->setToolTip(followTooltip);
    }
    if (previousCandidateButton) {
        previousCandidateButton->setToolTip(previousTooltip);
    }
    if (nextCandidateButton) {
        nextCandidateButton->setToolTip(nextTooltip);
    }
    if (minWidthLabel) {
        minWidthLabel->setText(minWidthText);
    }
    if (maxWidthLabel) {
        maxWidthLabel->setText(maxWidthText);
    }
    if (thresholdLabel) {
        thresholdLabel->setText(thresholdText);
    }
}

#include "frequencycontrol.h"

#include <QComboBox>
#include <QApplication>
#include <QPointer>
#include <QLineEdit>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QPushButton>
#include <QSizePolicy>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <iterator>

namespace {

struct UnitInfo {
    const char *label;
    double factor;
    int decimals;
};

constexpr UnitInfo UNITS[] = {
    {"Hz", 1.0, 0},
    {"kHz", 1000.0, 3},
    {"MHz", 1000000.0, 6},
    {"GHz", 1000000000.0, 9},
};

QVector<QPair<QString, double>> defaultSteps() {
    return {
        {"1 Hz", 1.0},
        {"10 Hz", 10.0},
        {"100 Hz", 100.0},
        {"1 kHz", 1000.0},
        {"5 kHz", 5000.0},
        {"10 kHz", 10000.0},
        {"100 kHz", 100000.0},
        {"1 MHz", 1000000.0},
        {"5 MHz", 5000000.0},
        {"10 MHz", 10000000.0},
        {"50 MHz", 50000000.0},
        {"100 MHz", 100000000.0},
    };
}

} // namespace

FrequencyControl::FrequencyControl(QWidget *parent)
    : QWidget(parent) {
    valueSpin = new QDoubleSpinBox(this);
    valueSpin->setKeyboardTracking(false);
    valueSpin->setButtonSymbols(QAbstractSpinBox::NoButtons);
    valueSpin->setAlignment(Qt::AlignRight);
    valueSpin->setMinimumWidth(76);
    valueSpin->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);

    unitCombo = new QComboBox(this);
    for (const UnitInfo &unit : UNITS) {
        unitCombo->addItem(unit.label, unit.factor);
    }
    unitCombo->setMaximumWidth(56);
    unitCombo->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    stepCombo = new QComboBox(this);
    setStepPresets(defaultSteps());
    stepCombo->setMinimumContentsLength(5);
    stepCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    stepCombo->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);

    presetCombo = new QComboBox(this);
    presetCombo->addItem("Preset", QVariant());
    presetCombo->setEnabled(false);
    presetCombo->setMinimumContentsLength(5);
    presetCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    presetCombo->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);

    minusButton = new QPushButton("-", this);
    plusButton = new QPushButton("+", this);
    minusButton->setFixedWidth(24);
    plusButton->setFixedWidth(24);

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    QHBoxLayout *valueLayout = new QHBoxLayout();
    valueLayout->setContentsMargins(0, 0, 0, 0);
    valueLayout->setSpacing(3);
    valueLayout->addWidget(minusButton);
    valueLayout->addWidget(valueSpin, 1);
    valueLayout->addWidget(unitCombo);
    valueLayout->addWidget(plusButton);
    QHBoxLayout *presetLayout = new QHBoxLayout();
    presetLayout->setContentsMargins(0, 0, 0, 0);
    presetLayout->setSpacing(3);
    presetLayout->addWidget(stepCombo, 1);
    presetLayout->addWidget(presetCombo, 1);
    layout->addLayout(valueLayout);
    layout->addLayout(presetLayout);

    connect(valueSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double) {
        if (!updatingUi) {
            commitCurrentValue();
        }
    });
    connect(unitCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        unitSelectionLocked = true;
        updateSpinRange();
        updateDisplayedValue();
        emit selectionChanged();
    });
    connect(stepCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        updateSpinRange();
        emit selectionChanged();
    });
    connect(minusButton, &QPushButton::clicked, this, [this]() {
        adjustByStep(-1);
    });
    connect(plusButton, &QPushButton::clicked, this, [this]() {
        adjustByStep(1);
    });
    connect(presetCombo, QOverload<int>::of(&QComboBox::activated), this, [this](int index) {
        const QVariant data = presetCombo->itemData(index);
        if (!data.isValid()) {
            return;
        }
        setValueHz(data.toDouble());
        emit valueCommitted(currentValueHz);
    });

    unitCombo->setCurrentIndex(2);
    updateSpinRange();
    updateDisplayedValue();
}

double FrequencyControl::valueHz() const {
    return currentValueHz;
}

void FrequencyControl::setValueHz(double valueHz) {
    currentValueHz = (std::clamp)(valueHz, minimumValueHz, maximumValueHz);
    const int preferredUnit = unitSelectionLocked ? unitCombo->currentIndex()
                                                  : unitIndexForValue(currentValueHz);
    if (unitCombo->currentIndex() != preferredUnit) {
        QSignalBlocker unitBlocker(unitCombo);
        unitCombo->setCurrentIndex(preferredUnit);
        updateSpinRange();
    }
    updateDisplayedValue();
}

void FrequencyControl::commitPendingValue() {
    if (!valueSpin) {
        return;
    }

    const bool wasUpdatingUi = updatingUi;
    updatingUi = true;
    valueSpin->interpretText();
    currentValueHz = (std::clamp)(valueSpin->value() * unitFactor(), minimumValueHz, maximumValueHz);
    updateDisplayedValue();
    updatingUi = wasUpdatingUi;
}

void FrequencyControl::setRangeHz(double minimumHz, double maximumHz) {
    minimumValueHz = std::isfinite(minimumHz) ? minimumHz : 0.0;
    maximumValueHz = std::isfinite(maximumHz) ? maximumHz : minimumValueHz + 1.0;
    if (maximumValueHz <= minimumValueHz) {
        maximumValueHz = minimumValueHz + 1.0;
    }
    currentValueHz = (std::clamp)(currentValueHz, minimumValueHz, maximumValueHz);
    updateSpinRange();
    updateDisplayedValue();
}

void FrequencyControl::setStepPresets(const QVector<QPair<QString, double>> &stepsHz) {
    QSignalBlocker blocker(stepCombo);
    stepCombo->clear();
    for (const auto &step : stepsHz) {
        if (step.second > 0.0) {
            stepCombo->addItem(step.first, step.second);
        }
    }
    if (stepCombo->count() == 0) {
        stepCombo->addItem("1 kHz", 1000.0);
    }
}

void FrequencyControl::setValuePresets(const QVector<QPair<QString, double>> &valuesHz) {
    QSignalBlocker blocker(presetCombo);
    presetCombo->clear();
    presetCombo->addItem("Preset", QVariant());
    for (const auto &preset : valuesHz) {
        if (std::isfinite(preset.second)) {
            presetCombo->addItem(preset.first, preset.second);
        }
    }
    presetCombo->setEnabled(presetCombo->count() > 1);
}

int FrequencyControl::selectedUnitIndex() const {
    return unitCombo ? unitCombo->currentIndex() : 0;
}

void FrequencyControl::setSelectedUnitIndex(int index) {
    if (!unitCombo) {
        return;
    }
    const int clampedIndex = (std::clamp)(index, 0, unitCombo->count() - 1);
    if (unitCombo->currentIndex() == clampedIndex) {
        return;
    }
    QSignalBlocker blocker(unitCombo);
    unitCombo->setCurrentIndex(clampedIndex);
    unitSelectionLocked = true;
    updateSpinRange();
    updateDisplayedValue();
}

QString FrequencyControl::selectedStepName() const {
    return stepCombo ? stepCombo->currentText() : QString();
}

void FrequencyControl::setSelectedStepName(const QString &name) {
    if (!stepCombo || name.trimmed().isEmpty()) {
        return;
    }
    const int index = stepCombo->findText(name);
    if (index < 0) {
        return;
    }
    QSignalBlocker blocker(stepCombo);
    stepCombo->setCurrentIndex(index);
    updateSpinRange();
}

QString FrequencyControl::selectedValuePresetName() const {
    if (!presetCombo || !presetCombo->currentData().isValid()) {
        return QString();
    }
    return presetCombo->currentText();
}

void FrequencyControl::setSelectedValuePresetName(const QString &name) {
    if (!presetCombo) {
        return;
    }
    const int index = name.trimmed().isEmpty() ? 0 : presetCombo->findText(name);
    if (index < 0) {
        return;
    }
    QSignalBlocker blocker(presetCombo);
    presetCombo->setCurrentIndex(index);
}

void FrequencyControl::copySelectionTo(FrequencyControl &target) const {
    target.setSelectedUnitIndex(selectedUnitIndex());
    target.setSelectedStepName(selectedStepName());
    target.setSelectedValuePresetName(selectedValuePresetName());
}

void FrequencyControl::synchronizeFrom(const FrequencyControl &source, bool includeValue) {
    if (minimumValueHz != source.minimumValueHz || maximumValueHz != source.maximumValueHz) {
        setRangeHz(source.minimumValueHz, source.maximumValueHz);
    }
    const auto syncPresets = [](QComboBox *target, const QComboBox *original) {
        bool same = target->count() == original->count();
        for (int i = 0; same && i < original->count(); ++i) {
            same = target->itemText(i) == original->itemText(i) &&
                   target->itemData(i) == original->itemData(i);
        }
        if (!same) {
            const QSignalBlocker blocker(target);
            target->clear();
            for (int i = 0; i < original->count(); ++i) {
                target->addItem(original->itemText(i), original->itemData(i));
            }
            target->setCurrentIndex(original->currentIndex());
        }
        return !same;
    };
    const bool stepsChanged = syncPresets(stepCombo, source.stepCombo);
    syncPresets(presetCombo, source.presetCombo);
    presetCombo->setEnabled(source.presetCombo->isEnabled());
    if (stepsChanged) updateSpinRange();
    if (includeValue) {
        source.copySelectionTo(*this);
        if (currentValueHz != source.currentValueHz) setValueHz(source.currentValueHz);
    }
    setEnabled(source.isEnabled());
}

FrequencyControl *FrequencyControl::createLinkedControl(QWidget *parent, QTimer *syncTimer,
                                                       const std::function<void()> &apply) {
    auto *copy = new FrequencyControl(parent);
    copy->synchronizeFrom(*this, true);
    const QPointer<FrequencyControl> source(this);
    connect(copy, &FrequencyControl::selectionChanged, copy, [source, copy]() {
        if (source) copy->copySelectionTo(*source);
    });
    connect(copy, &FrequencyControl::valueCommitted, copy, [source, copy, apply](double value) {
        if (!source || !source->isEnabled()) return;
        copy->copySelectionTo(*source);
        source->setValueHz(value);
        const QPointer<FrequencyControl> guard(copy);
        apply();
        // The receiver can clamp or reject a request; show the accepted value immediately.
        if (guard && source) guard->synchronizeFrom(*source, true);
    });
    connect(this, &FrequencyControl::valueCommitted, copy, [source, copy](double) {
        if (source) copy->synchronizeFrom(*source, true);
    });
    connect(syncTimer, &QTimer::timeout, copy, [source, copy]() {
        if (!source) { copy->setEnabled(false); return; }
        QWidget *focus = QApplication::focusWidget();
        const auto *edit = copy->valueSpin->findChild<QLineEdit *>();
        const bool typing = edit && edit->isModified() &&
                            (focus == copy || copy->isAncestorOf(focus));
        copy->synchronizeFrom(*source, !typing);
    });
    return copy;
}

double FrequencyControl::unitFactor() const {
    const int index = (std::clamp)(unitCombo->currentIndex(), 0, static_cast<int>(std::size(UNITS)) - 1);
    return UNITS[index].factor;
}

int FrequencyControl::unitIndexForValue(double valueHz) const {
    const double absValue = std::abs(valueHz);
    if (absValue >= 1000000000.0) {
        return 3;
    }
    if (absValue >= 1000000.0) {
        return 2;
    }
    if (absValue >= 1000.0) {
        return 1;
    }
    return 0;
}

void FrequencyControl::updateSpinRange() {
    const int index = (std::clamp)(unitCombo->currentIndex(), 0, static_cast<int>(std::size(UNITS)) - 1);
    const double factor = UNITS[index].factor;
    const double stepHz = stepCombo->currentData().toDouble();
    QSignalBlocker blocker(valueSpin);
    valueSpin->setDecimals(UNITS[index].decimals);
    valueSpin->setRange(minimumValueHz / factor, maximumValueHz / factor);
    valueSpin->setSingleStep((std::max)(stepHz / factor, std::pow(10.0, -UNITS[index].decimals)));
}

void FrequencyControl::updateDisplayedValue() {
    updatingUi = true;
    QSignalBlocker blocker(valueSpin);
    valueSpin->setValue(currentValueHz / unitFactor());
    updatingUi = false;
}

void FrequencyControl::commitCurrentValue() {
    currentValueHz = (std::clamp)(valueSpin->value() * unitFactor(), minimumValueHz, maximumValueHz);
    updateDisplayedValue();
    emit valueCommitted(currentValueHz);
}

void FrequencyControl::adjustByStep(int direction) {
    const double stepHz = stepCombo->currentData().toDouble();
    if (stepHz <= 0.0) {
        return;
    }
    setValueHz(currentValueHz + direction * stepHz);
    emit valueCommitted(currentValueHz);
}

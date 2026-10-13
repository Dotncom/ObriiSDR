#ifndef FREQUENCYCONTROL_H
#define FREQUENCYCONTROL_H

#include <QWidget>
#include <QVector>
#include <QPair>
#include <QString>
#include <functional>

class QComboBox;
class QDoubleSpinBox;
class QPushButton;
class QTimer;

class FrequencyControl : public QWidget {
    Q_OBJECT

public:
    explicit FrequencyControl(QWidget *parent = nullptr);

    double valueHz() const;
    void setValueHz(double valueHz);
    void commitPendingValue();
    void setRangeHz(double minimumHz, double maximumHz);
    void setStepPresets(const QVector<QPair<QString, double>> &stepsHz);
    void setValuePresets(const QVector<QPair<QString, double>> &valuesHz);
    int selectedUnitIndex() const;
    void setSelectedUnitIndex(int index);
    QString selectedStepName() const;
    void setSelectedStepName(const QString &name);
    QString selectedValuePresetName() const;
    void setSelectedValuePresetName(const QString &name);
    FrequencyControl *createLinkedControl(QWidget *parent, QTimer *syncTimer,
                                         const std::function<void()> &apply);

signals:
    void valueCommitted(double valueHz);
    void selectionChanged();

private:
    double unitFactor() const;
    int unitIndexForValue(double valueHz) const;
    void updateSpinRange();
    void updateDisplayedValue();
    void commitCurrentValue();
    void adjustByStep(int direction);
    void synchronizeFrom(const FrequencyControl &source, bool includeValue);
    void copySelectionTo(FrequencyControl &target) const;

    QDoubleSpinBox *valueSpin = nullptr;
    QComboBox *unitCombo = nullptr;
    QComboBox *stepCombo = nullptr;
    QComboBox *presetCombo = nullptr;
    QPushButton *minusButton = nullptr;
    QPushButton *plusButton = nullptr;

    double currentValueHz = 0.0;
    double minimumValueHz = 0.0;
    double maximumValueHz = 7750000000.0;
    bool updatingUi = false;
    bool unitSelectionLocked = false;
};

#endif // FREQUENCYCONTROL_H

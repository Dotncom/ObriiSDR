#ifndef ANALYZERINSTRUMENTDIALOG_H
#define ANALYZERINSTRUMENTDIALOG_H

#include "analyzerinstrumentplan.h"

#include <QDateTime>
#include <QDialog>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;

class AnalyzerInstrumentDialog final : public QDialog {
public:
    explicit AnalyzerInstrumentDialog(bool ukrainianUi, QWidget *parent = nullptr);
    ~AnalyzerInstrumentDialog() override;

private:
    QString text(const char *english, const char *ukrainian) const;
    AnalyzerInstrumentPlan planFromUi() const;
    void applyPlan(const AnalyzerInstrumentPlan &plan);
    void updateValidation();
    void loadPlan();
    void savePlan();
    void saveSessionTemplate();
    void correctReflectionFile();
    void restoreLastPlan();
    void persistLastPlan() const;

    bool ukrainian = false;
    QComboBox *modeCombo = nullptr;
    QComboBox *sourceCombo = nullptr;
    QLineEdit *nameEdit = nullptr;
    QLineEdit *txDeviceEdit = nullptr;
    QLineEdit *rxDeviceEdit = nullptr;
    QDoubleSpinBox *startSpin = nullptr;
    QDoubleSpinBox *stopSpin = nullptr;
    QSpinBox *pointsSpin = nullptr;
    QDoubleSpinBox *sourceLevelSpin = nullptr;
    QDoubleSpinBox *dwellSpin = nullptr;
    QDoubleSpinBox *settleSpin = nullptr;
    QSpinBox *averagesSpin = nullptr;
    QCheckBox *bidirectionalCheck = nullptr;

    QCheckBox *receiverCheck = nullptr;
    QCheckBox *transmitterCheck = nullptr;
    QCheckBox *fullDuplexCheck = nullptr;
    QCheckBox *coherentCheck = nullptr;
    QCheckBox *timestampedCheck = nullptr;
    QCheckBox *externalGeneratorCheck = nullptr;
    QCheckBox *couplerCheck = nullptr;
    QSpinBox *receiverChannelsSpin = nullptr;
    QSpinBox *transmitterChannelsSpin = nullptr;
    QDoubleSpinBox *hardwareMinimumSpin = nullptr;
    QDoubleSpinBox *hardwareMaximumSpin = nullptr;
    QDoubleSpinBox *safeOutputSpin = nullptr;

    QLineEdit *calibrationIdEdit = nullptr;
    QDoubleSpinBox *impedanceSpin = nullptr;
    QCheckBox *normalizationCheck = nullptr;
    QCheckBox *openCheck = nullptr;
    QCheckBox *shortCheck = nullptr;
    QCheckBox *loadCheck = nullptr;
    QCheckBox *thruCheck = nullptr;
    QCheckBox *phaseCheck = nullptr;
    QPlainTextEdit *calibrationNotesEdit = nullptr;

    QCheckBox *scpiEnabledCheck = nullptr;
    QLineEdit *scpiResourceEdit = nullptr;
    QLineEdit *scpiIdnEdit = nullptr;
    QLineEdit *scpiFrequencyEdit = nullptr;
    QLineEdit *scpiPowerEdit = nullptr;
    QLineEdit *scpiOutputEdit = nullptr;
    QLineEdit *scpiTriggerEdit = nullptr;
    QLineEdit *scpiReadEdit = nullptr;
    QSpinBox *scpiTimeoutSpin = nullptr;

    QLineEdit *operatorEdit = nullptr;
    QLineEdit *dutEdit = nullptr;
    QPlainTextEdit *commentEdit = nullptr;
    QLabel *gridSummaryLabel = nullptr;
    QPlainTextEdit *validationEdit = nullptr;
    QPushButton *executeButton = nullptr;
    QDateTime calibrationCapturedUtc;
};

#endif

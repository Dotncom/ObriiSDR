#ifndef ANALYZERINSTRUMENTDIALOG_H
#define ANALYZERINSTRUMENTDIALOG_H

#include "analyzerinstrumentplan.h"
#include "analyzerinstrumentsimulation.h"

#include <QDateTime>
#include <QDialog>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class AnalyzerSimulationTable;

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
    void startSimulation();
    void finishSimulation(const QString &error);
    void saveSimulation();
    void saveSession(const AnalyzerMeasurementSession &session, const QString &title,
                     const QString &suggestedName);
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
    QDateTime calibrationCapturedUtc;
    AnalyzerMeasurementSession simulatedSession;
    AnalyzerInstrumentSimulation simulation;
    AnalyzerSimulationTable *simulationTable = nullptr;
    QLabel *simulationStatus = nullptr;
    QProgressBar *simulationProgress = nullptr;
    QPushButton *startSimulationButton = nullptr;
    QPushButton *stopSimulationButton = nullptr;
    QPushButton *saveSimulationButton = nullptr;
};

#endif

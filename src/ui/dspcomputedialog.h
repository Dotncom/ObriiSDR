#pragma once
#include "dspstreammanager.h"
#include "spectrumcomputeoptions.h"
#include "dspconnectionprobe.h"
#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QSpinBox;
class QDoubleSpinBox;
class QTabWidget;
class QPushButton;

// Draft controls shared by the manager and the main FFT block editor.
class SpectrumComputeControls final : public QWidget {
    Q_OBJECT
public:
    explicit SpectrumComputeControls(bool ukrainian, QWidget *parent = nullptr);
    SpectrumComputeOptions options() const;
    void setOptions(const SpectrumComputeOptions &options);
    void setStatus(const QString &status);
signals:
    void applyRequested();
private:
    QComboBox *mode_ = nullptr;
    QComboBox *backend_ = nullptr;
    QSpinBox *uploadLimit_ = nullptr;
    QCheckBox *fallback_ = nullptr;
    QLabel *status_ = nullptr;
};

class DspComputeDialog final : public QDialog {
    Q_OBJECT
public:
    DspComputeDialog(const DspStreamEndpoint &endpoint, bool ukrainian, QWidget *parent = nullptr);
    ~DspComputeDialog() override;
    DspStreamEndpoint endpoint() const { return endpoint_; }
    void setEndpoint(const DspStreamEndpoint &endpoint);
    SpectrumComputeOptions spectrumComputeOptions() const { return spectrumOptions_; }
    void setSpectrumComputeOptions(const SpectrumComputeOptions &options);
    void setSpectrumComputeStatus(const QString &status);
    void setProbeContext(double sampleRate, int fftLength, int updateMs, int overlapPercent);
signals:
    void endpointChanged();
    void spectrumComputeOptionsChanged();
private:
    void addProbeTab(QTabWidget *tabs, bool ukrainian);
    void refreshProbeResults();
    bool ukrainian_ = false;
    DspConnectionProbe probe_;
    QDoubleSpinBox *probeRate_ = nullptr;
    QSpinBox *probeLength_ = nullptr, *probeInterval_ = nullptr, *probeOverlap_ = nullptr;
    QPushButton *probeStart_ = nullptr, *probeCancel_ = nullptr;
    QLabel *probeTarget_ = nullptr, *probeStatus_ = nullptr, *probeTimings_ = nullptr;
    QLabel *probeUpload_ = nullptr, *probeDownload_ = nullptr, *probeEstimate_ = nullptr;
    double contextRate_ = -1;
    int contextLength_ = -1, contextInterval_ = -1, contextOverlap_ = -1;
    DspStreamEndpoint endpoint_;
    SpectrumComputeOptions spectrumOptions_;
    SpectrumComputeControls *spectrumControls_ = nullptr;
    QCheckBox *endpointEnabled_ = nullptr;
    QLineEdit *endpointAddress_ = nullptr;
    QSpinBox *endpointPort_ = nullptr;
    QLineEdit *endpointToken_ = nullptr;
    DspComputeServer server_;
};

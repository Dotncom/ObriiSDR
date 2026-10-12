#ifndef WIRELESS_LAB_DIALOG_H
#define WIRELESS_LAB_DIALOG_H
#include <QDialog>
#include <QByteArray>
#include <QElapsedTimer>
#include <QMap>
#include <QStringList>
#include <QVector>
#include <vector>

class WirelessCaptureWorker;
class WirelessPacketModel;
class WifiPhyWidget;
class QThread;
class QTimer;
class QComboBox;
class QLineEdit;
class QCheckBox;
class QPushButton;
class QLabel;
class QTableWidget;
class QSpinBox;

class WirelessLabDialog : public QDialog {
    Q_OBJECT
public:
    explicit WirelessLabDialog(bool ukrainian, QWidget *parent = nullptr);
    ~WirelessLabDialog() override;
    void appendSpectrum(const std::vector<float> &frequencies,
                        const std::vector<float> &levels, double sampleRate);
    void setAudioEnabled(bool enabled);
    void setAudioControlEnabled(bool enabled);
    void setSpectrumUpdateInterval(int intervalMs);
    void setLiveIqSource(double sampleRateHz, double centerFrequencyHz, bool available);
    void appendLiveIq(const QByteArray &iq, double sampleRateHz,
                      double centerFrequencyHz, int sampleCount);
    bool isLiveIqActive() const;
signals:
    void frequencyRequested(double frequencyHz);
    void audioEnabledRequested(bool enabled);
    void liveIqActiveChanged(bool active);
    void spectrumUpdateIntervalRequested(int intervalMs);
protected:
    void closeEvent(QCloseEvent *event) override;
    void showEvent(QShowEvent *event) override;
private:
    QString text(const char *en, const char *uk) const;
    void startAnalysis(bool live);
    void analyzeFile(const QString &path);
    void pollPackets();
    void appendPacketBatch(QVector<QStringList> batch);
    void clearPackets();
    void stopCapture();
    void refreshInterfaces();
    void updateRunning(bool running);
    bool ukrainian;
    WirelessCaptureWorker *worker;
    QThread *thread;
    QTimer *timer;
    WirelessPacketModel *model;
    QLineEdit *programEdit;
    QLineEdit *filterEdit;
    QLineEdit *captureFilterEdit;
    QComboBox *profileCombo;
    QComboBox *protocolCombo;
    QComboBox *interfaceCombo;
    QCheckBox *monitorCheckbox;
    QCheckBox *saveCheckbox;
    QCheckBox *decryptCheckbox;
    QCheckBox *audioCheckbox;
    QSpinBox *spectrumUpdateSpin;
    QPushButton *openButton;
    QPushButton *captureButton;
    QPushButton *stopButton;
    QPushButton *refreshButton;
    QPushButton *checkButton;
    QLabel *status;
    QLabel *stats;
    QLabel *rfStatus;
    QTableWidget *networks;
    QTableWidget *channels;
    QComboBox *bandCombo;
    WifiPhyWidget *phy;
    QVector<QStringList> pendingLivePackets;
    QElapsedTimer rfTimer;
    QString captureFile;
    bool decoderTime = false;
    quint64 packetCount = 0, retryCount = 0, protectedCount = 0, queueDrops = 0;
    double firstTime = 0.0, lastTime = 0.0;
    struct Network {
        QString ssid, frequency;
        quint64 packets = 0;
        double rssiSum = 0.0;
        int rssiCount = 0;
    };
    QMap<QString, Network> networkStats;
};
#endif

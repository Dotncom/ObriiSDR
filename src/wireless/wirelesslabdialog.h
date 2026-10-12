#ifndef WIRELESS_LAB_DIALOG_H
#define WIRELESS_LAB_DIALOG_H
#include <QDialog>
#include <QElapsedTimer>
#include <QMap>
#include <vector>

class WirelessCaptureWorker;
class WirelessPacketModel;
class QThread;
class QTimer;
class QComboBox;
class QLineEdit;
class QCheckBox;
class QPushButton;
class QLabel;
class QTableWidget;

class WirelessLabDialog : public QDialog {
    Q_OBJECT
public:
    explicit WirelessLabDialog(bool ukrainian, QWidget *parent = nullptr);
    ~WirelessLabDialog() override;
    void appendSpectrum(const std::vector<float> &frequencies,
                        const std::vector<float> &levels, double sampleRate);
signals:
    void frequencyRequested(double frequencyHz);
protected:
    void closeEvent(QCloseEvent *event) override;
private:
    QString text(const char *en, const char *uk) const;
    void startAnalysis(bool live);
    void pollPackets();
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
    QLineEdit *profileEdit;
    QComboBox *protocolCombo;
    QComboBox *interfaceCombo;
    QCheckBox *monitorCheckbox;
    QCheckBox *saveCheckbox;
    QPushButton *openButton;
    QPushButton *captureButton;
    QPushButton *stopButton;
    QPushButton *refreshButton;
    QLabel *status;
    QLabel *stats;
    QLabel *rfStatus;
    QTableWidget *networks;
    QTableWidget *channels;
    QComboBox *bandCombo;
    QElapsedTimer rfTimer;
    QString captureFile;
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

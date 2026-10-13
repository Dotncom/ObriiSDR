#ifndef WIFI_PHY_WIDGET_H
#define WIFI_PHY_WIDGET_H
#include <QWidget>
#include <QByteArray>
#include <QStringList>
#include "wifiphydiagnostics.h"
class QProcess;
class QLineEdit;
class QComboBox;
class QDoubleSpinBox;
class QPlainTextEdit;
class QLabel;
class QPushButton;
class QThread;
class QJsonObject;
class WifiLiveIqWorker;
class WifiPhyWidget : public QWidget {
    Q_OBJECT
public:
    explicit WifiPhyWidget(bool ukrainian, QWidget *parent = nullptr);
    ~WifiPhyWidget() override;
    void stop();
    void setLiveSource(double sampleRateHz, double centerFrequencyHz, bool available);
    void appendLiveIq(const QByteArray &iq, double sampleRateHz,
                      double centerFrequencyHz, int sampleCount);
    bool isLiveActive() const { return liveActive; }
    void setCaptureBusy(bool busy);
signals:
    void captureReady(const QString &path);
    void liveCaptureStarted(const QString &path);
    void liveActiveChanged(bool active);
    void livePacketRow(const QStringList &row);
private:
    QString text(const char *en, const char *uk) const;
    void start();
    void startLive();
    void stopLive();
    void updateLiveButtons();
    void saveSettings();
    void showPhyStats(const QJsonObject &stats);
    void readProcessOutput();
    bool ukrainian, cancelled = false;
    QProcess *process;
    QLineEdit *python, *input, *directory;
    QComboBox *format, *phyMode;
    QString legacyPythonPath, extendedPythonPath;
    QDoubleSpinBox *sampleRate, *center, *channel, *threshold;
    QLabel *status, *liveSourceStatus, *phyStatus;
    QPlainTextEdit *log;
    QPushButton *startButton, *stopButton, *openButton;
    QPushButton *liveStartButton, *liveStopButton;
    QThread *liveThread;
    WifiLiveIqWorker *liveWorker;
    double liveRate = 0.0, liveCenter = 0.0;
    bool liveAvailable = false, liveActive = false, liveStarting = false,
         liveStopping = false, captureBusy = false;
    QString result;
    QByteArray processOutputBuffer;
    WifiPhyDiagnostics filePhyDiagnostics;
};
#endif

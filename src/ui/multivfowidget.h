#ifndef MULTIVFOWIDGET_H
#define MULTIVFOWIDGET_H

#include <QElapsedTimer>
#include <QWidget>

#include <vector>

class QCheckBox;
class QLabel;
class QComboBox;
class QDialog;
class QGridLayout;
class QPushButton;
class QTableWidget;

class MultiVfoWidget final : public QWidget {
    Q_OBJECT

public:
    explicit MultiVfoWidget(QWidget *parent = nullptr);

    void setLanguage(bool ukrainian);
    void setReceiverContext(double centerFrequencyHz,
                            double sampleRate,
                            double listeningFrequencyHz,
                            double bandwidthHz,
                            int modulationType);
    void updateSpectrum(const std::vector<float> &frequencies,
                        const std::vector<float> &levels);
    QString configurationJson() const;
    bool setConfigurationJson(const QString &json);
    int addSelection(double lowFrequencyHz, double highFrequencyHz);

signals:
    void configurationChanged(const QString &json);
    void monitorRequested(double frequencyHz, double bandwidthHz, int modulationType);

private:
    enum Column {
        EnabledColumn = 0,
        MonitorColumn,
        NameColumn,
        FrequencyColumn,
        BandwidthColumn,
        ModulationColumn,
        SquelchColumn,
        LevelColumn,
        StateColumn,
        ColumnCount
    };

    void addChannel(double frequencyHz = 0.0,
                    double bandwidthHz = 12500.0,
                    int modulationType = 1,
                    const QString &name = QString(),
                    const QString &channelId = QString());
    void removeSelectedChannel();
    void moveSelectedChannel(int direction);
    void updateHeaders();
    void updateStatus();
    void emitConfigurationChanged();
    void requestMonitorForRow(int row);
    void enforceSingleMonitor(int selectedRow);
    int monitoredRow() const;
    void openMosaic();
    void openMosaicLevelEditor(int row);
    void rebuildMosaic();
    void updateMosaicConfiguration();

    QCheckBox *enabledCheckBox = nullptr;
    QLabel *engineLabel = nullptr;
    QLabel *statusLabel = nullptr;
    QTableWidget *table = nullptr;
    QPushButton *addButton = nullptr;
    QPushButton *removeButton = nullptr;
    QPushButton *upButton = nullptr;
    QPushButton *downButton = nullptr;
    QPushButton *monitorButton = nullptr;
    QComboBox *viewModeCombo = nullptr;
    QPushButton *mosaicButton = nullptr;
    QDialog *mosaicDialog = nullptr;
    QGridLayout *mosaicLayout = nullptr;
    std::vector<QWidget *> mosaicViews;
    bool ukrainian = false;
    bool loading = false;
    double receiverCenterHz = 100000000.0;
    double receiverSampleRate = 50000000.0;
    double receiverListeningHz = 100000000.0;
    double receiverBandwidthHz = 200000.0;
    int receiverModulationType = 7;
    std::vector<float> smoothedLevels;
    QElapsedTimer spectrumUpdateTimer;
};

#endif // MULTIVFOWIDGET_H

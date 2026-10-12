#ifndef FT8STATIONDIALOG_H
#define FT8STATIONDIALOG_H

#include "ft8stationutils.h"
#include "qthlocator.h"

#include <QDialog>
#include <QHash>
#include <QHostAddress>
#include <QVector>

#include <functional>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
class QHideEvent;
class QLabel;
class QLineEdit;
class QNetworkAccessManager;
class QNetworkReply;
class QPlainTextEdit;
class QProcess;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTabWidget;
class QTimer;
class QUdpSocket;

class Ft8StationDialog final : public QDialog {
    Q_OBJECT

public:
    using AutomaticCancelHandler = std::function<void()>;
    using AutomaticTransmitHandler = std::function<bool(
        const QString &message,
        double dialFrequencyHz,
        double audioFrequencyHz,
        int slotParity,
        QString *error)>;

    explicit Ft8StationDialog(const QString &language, QWidget *parent = nullptr);
    ~Ft8StationDialog() override;

    void setLanguage(const QString &language);
    void setDialFrequency(double frequencyHz);
    void setAutomaticTransmitHandler(AutomaticTransmitHandler handler);
    void setAutomaticCancelHandler(AutomaticCancelHandler handler);
    void addDecode(const QDateTime &utc,
                   double rfFrequencyHz,
                   double audioFrequencyHz,
                   double dtSeconds,
                   int snrDbEstimate,
                   double qualityDb,
                   int costasHits,
                   const QString &message);

signals:
    void transmitMessageRequested(const QString &message,
                                  double dialFrequencyHz,
                                  double audioFrequencyHz,
                                  int slotParity);
    void transmitterSetupRequested();
    void mapExportRequested(const QVector<qth::UserMarker> &markers);

protected:
    void closeEvent(QCloseEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    enum class AutoQsoStage {
        Idle,
        AwaitReport,
        AwaitRoger,
        AwaitSignoff
    };

    QString text(const QString &english, const QString &ukrainian) const;
    void buildUi();
    void updateTexts();
    void loadSettings();
    void saveSettings() const;
    void scheduleSettingsSave();
    void loadLog();
    void refreshLogTable();
    void selectCurrentDecode();
    void setQsoPartner(const Ft8MessageInfo &info,
                       const QDateTime &utc,
                       double rfFrequencyHz,
                       double audioFrequencyHz);
    QString standardMessage(int index) const;
    void sendStandardMessage(int index);
    Ft8QsoRecord currentQsoRecord() const;
    bool appendQso(const Ft8QsoRecord &record, QString *error);
    void logCurrentQso();
    void exportAdif();
    void openLogFolder();
    void uploadRecordToQrz(const Ft8QsoRecord &record);
    void uploadSelectedToQrz();
    void uploadLogToLotw();
    void sendWsjtDatagram(const QByteArray &packet);
    void sendWsjtHeartbeatAndStatus();
    void queuePskReport(const Ft8MessageInfo &info,
                        const QDateTime &utc,
                        double rfFrequencyHz);
    void flushPskReports();
    void setOnlineStatus(const QString &status, bool error = false);

    void addCountryRuleRow(const Ft8CallsignRule &rule);
    QVector<Ft8CallsignRule> countryRules() const;
    Ft8CallsignFilterResult filterCallsign(const QString &callsign) const;
    bool callsignAllowed(const QString &callsign, QString *reason = nullptr) const;
    void refreshDecodeFilters();
    void applyDecodeFilterAppearance(int row,
                                     const Ft8MessageInfo &info,
                                     const Ft8CallsignFilterResult &filter);
    void setAutomaticEnabled(bool enabled);
    void handleAutomaticDecode(const Ft8MessageInfo &info,
                               const QDateTime &utc,
                               double rfFrequencyHz,
                               double audioFrequencyHz,
                               int snrDbEstimate);
    bool requestAutomaticTransmission(const QString &message, int slotParity);
    void checkAutomaticTimeout();
    void resetAutomaticQso(const QString &status = QString(), bool disarm = false);
    void finishAutomaticQso(const QString &status, bool logQso);
    void updateAutomaticStatus(const QString &status = QString());
    QString formattedSignalReport(int snrDbEstimate) const;

    QString language;
    AutomaticTransmitHandler automaticTransmitHandler;
    AutomaticCancelHandler automaticCancelHandler;
    QVector<Ft8QsoRecord> logRecords;
    QVector<Ft8ReceptionReport> pendingPskReports;
    QHash<QString, qint64> lastPskReportByStation;
    QHash<QString, qint64> automaticContactByCall;
    QVector<qint64> automaticQsoStartsMs;
    quint32 pskSequence = 0;
    quint32 pskSessionId = 0;
    int pskPacketCount = 0;
    qint64 pskLastTemplateMs = 0;
    QDateTime qsoStartUtc;
    double currentDialFrequencyHz = 0.0;

    AutoQsoStage automaticStage = AutoQsoStage::Idle;
    QString automaticPartnerCall;
    QString automaticLastMessage;
    int automaticSlotParity = -1;
    int automaticRetryCount = 0;
    qint64 automaticLastTxMs = 0;
    qint64 automaticNextQsoAllowedMs = 0;

    QTabWidget *tabs = nullptr;
    QTableWidget *decodeTable = nullptr;
    QTableWidget *logTable = nullptr;
    QLineEdit *stationCallEdit = nullptr;
    QLineEdit *stationGridEdit = nullptr;
    QLineEdit *dxCallEdit = nullptr;
    QLineEdit *dxGridEdit = nullptr;
    QLineEdit *sentReportEdit = nullptr;
    QLineEdit *receivedReportEdit = nullptr;
    QLineEdit *commentEdit = nullptr;
    QDoubleSpinBox *dialFrequencySpin = nullptr;
    QSpinBox *txAudioSpin = nullptr;
    QComboBox *slotCombo = nullptr;
    QLabel *qsoStatusLabel = nullptr;
    QLabel *logPathLabel = nullptr;
    QLabel *onlineStatusLabel = nullptr;
    QPushButton *useDecodeButton = nullptr;
    QPushButton *logQsoButton = nullptr;
    QPushButton *exportButton = nullptr;
    QPushButton *openFolderButton = nullptr;
    QPushButton *mapExportButton = nullptr;
    QVector<QPushButton *> messageButtons;

    QGroupBox *automaticBox = nullptr;
    QCheckBox *automaticReplyCheck = nullptr;
    QSpinBox *automaticMinSnrSpin = nullptr;
    QSpinBox *automaticRetriesSpin = nullptr;
    QSpinBox *automaticTimeoutSpin = nullptr;
    QSpinBox *automaticHourlyLimitSpin = nullptr;
    QSpinBox *automaticCooldownSpin = nullptr;
    QPushButton *automaticStopButton = nullptr;
    QPushButton *transmitterSetupButton = nullptr;
    QLabel *automaticStatusLabel = nullptr;

    QPlainTextEdit *blockedCallsEdit = nullptr;
    QTableWidget *countryRuleTable = nullptr;
    QLabel *filterHelpLabel = nullptr;
    QLabel *blockedCallsLabel = nullptr;
    QLabel *countryHelpLabel = nullptr;
    QPushButton *countryAddButton = nullptr;
    QPushButton *countryRemoveButton = nullptr;
    QPushButton *countryImportButton = nullptr;
    QLineEdit *countrySearchEdit = nullptr;
    mutable QVector<Ft8CallsignRule> cachedCountryRules;
    mutable bool countryRulesDirty = true;

    QCheckBox *wsjtEnabledCheck = nullptr;
    QLineEdit *wsjtAddressEdit = nullptr;
    QSpinBox *wsjtPortSpin = nullptr;
    QCheckBox *pskEnabledCheck = nullptr;
    QLineEdit *antennaEdit = nullptr;
    QCheckBox *qrzAutoUploadCheck = nullptr;
    QLineEdit *qrzKeyEdit = nullptr;
    QPushButton *qrzUploadButton = nullptr;
    QLineEdit *tqslPathEdit = nullptr;
    QLineEdit *tqslLocationEdit = nullptr;
    QPushButton *tqslBrowseButton = nullptr;
    QPushButton *tqslUploadButton = nullptr;

    QUdpSocket *wsjtSocket = nullptr;
    QUdpSocket *pskSocket = nullptr;
    QTimer *wsjtHeartbeatTimer = nullptr;
    QTimer *pskFlushTimer = nullptr;
    QTimer *automaticTimer = nullptr;
    QTimer *settingsSaveTimer = nullptr;
    QNetworkAccessManager *network = nullptr;
    QProcess *tqslProcess = nullptr;
};

#endif // FT8STATIONDIALOG_H

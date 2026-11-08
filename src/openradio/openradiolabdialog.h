#ifndef OPENRADIOLABDIALOG_H
#define OPENRADIOLABDIALOG_H

#include <QDialog>
#include <QHash>
#include "ax25codec.h"
#include "kisscodec.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLineEdit;
class QPlainTextEdit;
class QSpinBox;
class QTcpServer;
class QTcpSocket;
class QLabel;
class Ax25LiveDecoder;
class KissVirtualTx;

class OpenRadioLabDialog : public QDialog {
    Q_OBJECT
public:
    explicit OpenRadioLabDialog(bool ukrainian, QWidget *parent = nullptr);
    void submitAfskAudioFrame(const QByteArray &pcmData, int sampleRate = 48000);

private:
    QString text(const char *english, const char *ukrainianText) const;
    QWidget *createOverviewPage();
    QWidget *createAx25Page();
    QWidget *createM17Page();
    QWidget *createTransportPage();
    void encodeAx25();
    void decodeAx25();
    bool buildAx25Frame(Ax25UiFrame *frame, QString *error) const;
    void exportAx25Wav();
    void decodeAx25Wav();
    void toggleKissReceiver();
    void generateM17();
    void saveM17Symbols();
    void openM17Symbols();
    void buildTransportEnvelope();

    bool ukrainian = false;
    QLineEdit *axSource = nullptr;
    QLineEdit *axDestination = nullptr;
    QLineEdit *axDigipeaters = nullptr;
    QPlainTextEdit *axPayload = nullptr;
    QPlainTextEdit *axHex = nullptr;
    QTcpServer *kissServer = nullptr;
    Ax25LiveDecoder *axLiveDecoder = nullptr;
    QSpinBox *kissPort = nullptr;
    QPushButton *kissToggle = nullptr;
    QLabel *kissStatus = nullptr;
    QCheckBox *kissTxEnabled = nullptr;
    QLineEdit *kissTxChannel = nullptr;
    QDoubleSpinBox *kissTxCenter = nullptr;
    QLabel *kissTxStatus = nullptr;
    KissVirtualTx *kissTx = nullptr;
    QHash<QTcpSocket *, KissStreamDecoder> kissClients;
    QComboBox *axWireFormat = nullptr;
    QLineEdit *m17Source = nullptr;
    QLineEdit *m17Destination = nullptr;
    QSpinBox *m17Can = nullptr;
    QComboBox *m17PayloadMode = nullptr;
    QPlainTextEdit *m17Payload = nullptr;
    QPlainTextEdit *m17Result = nullptr;
    QByteArray m17GeneratedSymbols;
    QComboBox *transportPayloadType = nullptr;
    QLineEdit *transportProtocol = nullptr;
    QLineEdit *transportStream = nullptr;
    QPlainTextEdit *transportPayload = nullptr;
    QPlainTextEdit *transportResult = nullptr;
};

#endif // OPENRADIOLABDIALOG_H

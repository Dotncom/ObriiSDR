#ifndef WIRELESS_PACKET_DETAILS_H
#define WIRELESS_PACKET_DETAILS_H
#include <QDialog>
#include <QJsonObject>

class QProcess;
class QTimer;
class QLabel;
class QTreeWidget;
class QPlainTextEdit;
namespace Wireless {
bool packetDetails(const QByteArray &json, QJsonObject *layers, QByteArray *raw, QString *error);
}
class WirelessPacketDetails : public QDialog {
public:
    explicit WirelessPacketDetails(bool ukrainian, QWidget *parent = nullptr);
    ~WirelessPacketDetails() override;
    void load(const QString &program, const QString &capture, quint64 frame,
              const QString &profile, bool decrypt);
private:
    QProcess *process;
    QTimer *timeout;
    QLabel *status;
    QTreeWidget *tree;
    QPlainTextEdit *bytes;
    QByteArray output, diagnostics;
    bool ukrainian, overflow = false, timedOut = false;
};
#endif

#ifndef OPENRADIOPROTOCOL_H
#define OPENRADIOPROTOCOL_H

#include <QList>
#include <QString>

enum class OpenRadioProtocol {
    M17,
    Ax25Kiss,
    Npr70,
    Aredn,
    FreeDv,
    Ardop,
    ExperimentalOfdmTdma
};

enum class OpenRadioReadiness {
    BuiltInFraming,
    ExternalBackend,
    HardwareDeferred
};

struct OpenRadioProtocolDescriptor {
    OpenRadioProtocol protocol = OpenRadioProtocol::Ax25Kiss;
    QString id;
    QString name;
    OpenRadioReadiness readiness = OpenRadioReadiness::HardwareDeferred;
    bool packet = false;
    bool voice = false;
    bool ipBearer = false;
    bool receive = false;
    bool transmit = false;
    bool requiresFullDuplex = false;
    int symbolRate = 0;
    int nominalBandwidthHz = 0;
    QString engine;
    QString limitation;
};

class OpenRadioProtocolCatalog {
public:
    static QList<OpenRadioProtocolDescriptor> protocols();
    static OpenRadioProtocolDescriptor descriptor(OpenRadioProtocol protocol);
    static QString readinessName(OpenRadioReadiness readiness, bool ukrainian);
};

#endif // OPENRADIOPROTOCOL_H

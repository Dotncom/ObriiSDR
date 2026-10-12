#include "wirelesspackettools.h"
#include <cmath>

namespace Wireless {
QString displayFilter(Protocol protocol) {
    switch (protocol) {
    case Protocol::Bluetooth: return QStringLiteral("btle || bluetooth");
    case Protocol::Gsm: return QStringLiteral("gsmtap || gsm_a || gsm_sms");
    case Protocol::Lte: return QStringLiteral("mac-lte || rlc-lte || pdcp-lte || lte_rrc");
    default: return QStringLiteral("wlan");
    }
}
QStringList tsharkFields() {
    return {QStringLiteral("frame.number"), QStringLiteral("frame.time_epoch"),
        QStringLiteral("_ws.col.Protocol"), QStringLiteral("_ws.col.Source"),
        QStringLiteral("_ws.col.Destination"), QStringLiteral("frame.len"),
        QStringLiteral("wlan.bssid"), QStringLiteral("wlan.ssid"),
        QStringLiteral("radiotap.channel.freq"), QStringLiteral("radiotap.dbm_antsignal"),
        QStringLiteral("wlan.fc.type_subtype"), QStringLiteral("wlan.fc.retry"),
        QStringLiteral("wlan.fc.protected"), QStringLiteral("wlan.fcs.status"),
        QStringLiteral("wlan.seq"), QStringLiteral("_ws.col.Info")};
}
QStringList tsharkOutputArguments() {
    QStringList result{QStringLiteral("-n"), QStringLiteral("-l"),
        QStringLiteral("-T"), QStringLiteral("fields"),
        QStringLiteral("-E"), QStringLiteral("separator=/t"),
        QStringLiteral("-E"), QStringLiteral("quote=d"),
        QStringLiteral("-E"), QStringLiteral("occurrence=f")};
    for (const QString &field : tsharkFields()) result << QStringLiteral("-e") << field;
    return result;
}
bool parsePacketLine(const QByteArray &line, QStringList &fields) {
    fields.clear();
    if (line.size() > 65536) return false;
    QString text = QString::fromUtf8(line);
    if (text.endsWith(QLatin1Char('\r'))) text.chop(1);
    QString value;
    bool quoted = false;
    for (int i = 0; i < text.size(); ++i) {
        const QChar c = text[i];
        if (c == QLatin1Char('"')) {
            if (quoted && i + 1 < text.size() && text[i + 1] == c) { value += c; ++i; }
            else quoted = !quoted;
        } else if (c == QLatin1Char('\t') && !quoted) {
            fields << value; value.clear();
        } else value += c;
    }
    fields << value;
    return !quoted && fields.size() == tsharkFields().size() &&
           !fields[0].isEmpty() && !fields[1].isEmpty();
}
int wifiChannel(int frequencyMhz) {
    if (frequencyMhz == 2484) return 14;
    if (frequencyMhz >= 2412 && frequencyMhz <= 2472 && (frequencyMhz - 2407) % 5 == 0)
        return (frequencyMhz - 2407) / 5;
    if (frequencyMhz == 5935) return 2;
    if (frequencyMhz >= 5955 && frequencyMhz <= 7115 && (frequencyMhz - 5950) % 5 == 0)
        return (frequencyMhz - 5950) / 5;
    if (frequencyMhz >= 5000 && frequencyMhz <= 5900 && frequencyMhz % 5 == 0)
        return (frequencyMhz - 5000) / 5;
    return -1;
}
bool validateIqStream(const BackendCapabilities &c, const IqStreamDescriptor &s,
                      double bandwidth, QString &error) {
    if (!c.receivesIq) error = QStringLiteral("Backend accepts packets, not raw SDR IQ");
    else if (!std::isfinite(s.sampleRateHz) || !std::isfinite(s.usableBandwidthHz) ||
             !std::isfinite(s.centerFrequencyHz) || !std::isfinite(bandwidth) ||
             s.sampleRateHz <= 0.0 || bandwidth <= 0.0 ||
             s.usableBandwidthHz > s.sampleRateHz ||
             s.sampleRateHz < bandwidth || s.usableBandwidthHz < bandwidth)
        error = QStringLiteral("Insufficient or invalid usable IQ bandwidth");
    else if ((c.maximumSampleRateHz > 0.0 && s.sampleRateHz > c.maximumSampleRateHz) ||
             (c.maximumBandwidthHz > 0.0 && bandwidth > c.maximumBandwidthHz))
        error = QStringLiteral("Backend rate/bandwidth capability exceeded");
    else { error.clear(); return true; }
    return false;
}
bool validateTransmission(const BackendCapabilities &c, const IqStreamDescriptor &s,
                          const PacketTxRequest &r, QString &error) {
    if (!c.transmitsPackets) error = QStringLiteral("Backend is receive-only");
    else if (r.simultaneousRxOnSameDevice && !c.fullDuplex)
        error = QStringLiteral("This device cannot receive and transmit simultaneously");
    else if (r.frame.isEmpty() || r.frame.size() > 65536) error = QStringLiteral("Invalid packet length");
    else if (r.timed && (!c.timedTransmit || !s.continuous || r.clockId.isEmpty() ||
                        r.clockId != s.clockId || r.epoch != s.epoch || r.targetSample < s.firstSample))
        error = QStringLiteral("Timed TX requires a matching continuous hardware clock and epoch");
    else { error.clear(); return true; }
    return false;
}
}

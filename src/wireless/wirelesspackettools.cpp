#include "wirelesspackettools.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>
#include <cmath>
#include <limits>

namespace Wireless {
QString displayFilter(Protocol protocol) {
    switch (protocol) {
    case Protocol::Bluetooth: return QStringLiteral("btle || bluetooth");
    case Protocol::Gsm: return QStringLiteral("gsmtap || gsm_a || gsm_sms");
    case Protocol::Lte: return QStringLiteral("mac-lte || rlc-lte || pdcp-lte || lte_rrc");
    case Protocol::Umts: return QStringLiteral("rrc || nbap || rnsap || fp");
    case Protocol::All: return {};
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
        QStringLiteral("wlan.seq"), QStringLiteral("_ws.col.Info"), QStringLiteral("frame.protocols")};
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
QString findTshark() {
    const QDir application(QCoreApplication::applicationDirPath());
#ifdef Q_OS_WIN
    for (const auto &path : {application.filePath("wireshark/tshark.exe"), QStringLiteral("C:/Program Files/Wireshark/tshark.exe")})
        if (QFileInfo::exists(path)) return path;
#else
    if (QFileInfo::exists(application.filePath("wireshark/tshark"))) return application.filePath("wireshark/tshark");
#endif
    return QStandardPaths::findExecutable(QStringLiteral("tshark"));
}
QString ssidText(const QString &field) {
    // Wireshark 4.x exports wlan.ssid (FT_BYTES) as hexadecimal, not as display text.
    const auto hex = field.toLatin1();
    if (hex.isEmpty() || hex.size() > 64 || hex.size() % 2) return field;
    for (char c : hex) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return field;
    const auto raw = QByteArray::fromHex(hex);
    const QString decoded = QString::fromUtf8(raw.constData(), raw.size());
    for (const auto c : decoded) if (!c.isPrint() || c == QChar::ReplacementCharacter) return QStringLiteral("0x") + field;
    return decoded;
}
QStringList availableProfiles() {
#ifdef Q_OS_WIN
    const QString root = QDir(qEnvironmentVariable("APPDATA")).filePath("Wireshark/profiles");
#else
    const QString root = QDir(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)).filePath("wireshark/profiles");
#endif
    return QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name);
}
QVector<CaptureInterface> parseInterfaces(const QString &report) {
    QVector<CaptureInterface> result;
    const QRegularExpression linePattern(QStringLiteral("^[0-9]+\\. (.+)$"));
    for (const auto &line : report.split(QLatin1Char('\n'))) {
        const auto match = linePattern.match(line.trimmed());
        if (!match.hasMatch()) continue;
        const QString label = match.captured(1);
        const int description = label.indexOf(QStringLiteral(" ("));
        const QString id = description >= 0 ? label.left(description) : label;
        if (!id.isEmpty() && result.size() < 256) result.append({id, label});
    }
    return result;
}
QStringList analysisArguments(const AnalysisRequest &r, QString *error) {
    if (error) error->clear();
    const auto fail = [error](const char *message) { if (error) *error = QString::fromLatin1(message); return QStringList{}; };
    if (r.live ? r.interfaceId.isEmpty() : r.inputFile.isEmpty()) return fail("Select a capture interface or an input file");
    if (!r.live && (!r.outputFile.isEmpty() || r.monitor)) return fail("File analysis cannot enable live capture options");
    if (!r.outputFile.isEmpty() && QFileInfo(r.outputFile).absoluteFilePath() == QFileInfo(r.inputFile).absoluteFilePath())
        return fail("Input and output capture files must differ");
    QStringList args = tsharkOutputArguments();
    if (!r.profile.isEmpty()) args << "-C" << r.profile;
    args << "-o" << (r.decryptWithProfile ? "wlan.enable_decryption:TRUE" : "wlan.enable_decryption:FALSE");
    if (r.live) {
        args << "-i" << r.interfaceId << "-a" << "duration:300";
        if (r.monitor) args << "-I";
        if (!r.captureFilter.isEmpty()) args << "-f" << r.captureFilter;
        if (!r.outputFile.isEmpty()) args << "-a" << "filesize:102400" << "-F" << "pcapng" << "-w" << r.outputFile << "-P";
    } else args << "-r" << r.inputFile;
    // TShark rejects a live display filter combined with -w. Reopen the saved file to filter it.
    if ((!r.live || r.outputFile.isEmpty()) && !r.displayFilter.isEmpty()) args << "-Y" << r.displayFilter;
    return args;
}
PayloadState payloadState(const QStringList &fields) {
    const auto layers = fields.value(16).split(QLatin1Char(':'));
    if (!layers.contains("wlan")) return PayloadState::NotWifi;
    if (fields.value(12) != "1") return PayloadState::Clear;
    // Successful higher-layer dissection is evidence of readable payload, not a claim about every protected frame.
    return layers.contains("llc") ? PayloadState::Decoded : PayloadState::Protected;
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
    if (c.planningOnly) error = QStringLiteral("Hardware template is not a probed/qualified device");
    else if (!c.receivesIq) error = QStringLiteral("Backend accepts packets, not raw SDR IQ");
    else if (s.activeChannels < 1 || s.activeChannels > c.receiveChannels || s.channelIndex < 0 || s.channelIndex >= s.activeChannels ||
             s.activeTransmitChannels < 0 || s.activeTransmitChannels > c.transmitChannels)
        error = QStringLiteral("Invalid or unsupported RX channel configuration");
    else if (!std::isfinite(s.sampleRateHz) || !std::isfinite(s.usableBandwidthHz) ||
             !std::isfinite(s.centerFrequencyHz) || !std::isfinite(bandwidth) ||
             s.sampleRateHz <= 0.0 || bandwidth <= 0.0 ||
             s.usableBandwidthHz > s.sampleRateHz ||
             s.sampleRateHz < bandwidth || s.usableBandwidthHz < bandwidth)
        error = QStringLiteral("Insufficient or invalid usable IQ bandwidth");
    else if ((c.maximumSampleRateHz > 0.0 && s.sampleRateHz > c.maximumSampleRateHz) ||
             ((s.activeChannels > 1 || s.activeTransmitChannels > 1) && c.maximumDualChannelSampleRateHz > 0.0 && s.sampleRateHz > c.maximumDualChannelSampleRateHz) ||
             (c.maximumBandwidthHz > 0.0 && (bandwidth > c.maximumBandwidthHz || s.usableBandwidthHz > c.maximumBandwidthHz)))
        error = QStringLiteral("Backend rate/bandwidth capability exceeded");
    else if ((c.minimumFrequencyHz > 0.0 && s.centerFrequencyHz < c.minimumFrequencyHz) ||
             (c.maximumFrequencyHz > 0.0 && s.centerFrequencyHz > c.maximumFrequencyHz))
        error = QStringLiteral("Frequency is outside the probed backend range");
    else { error.clear(); return true; }
    return false;
}
bool validateTransmission(const BackendCapabilities &c, const IqStreamDescriptor &s,
                          const PacketTxRequest &r, QString &error) {
    if (c.planningOnly) error = QStringLiteral("Hardware template cannot authorize transmission");
    else if (!c.transmitsPackets) error = QStringLiteral("Backend does not implement packet transmission");
    else if (r.channelIndex < 0 || r.channelIndex >= c.transmitChannels) error = QStringLiteral("Invalid TX channel");
    else if (r.simultaneousRxOnSameDevice && !c.fullDuplex)
        error = QStringLiteral("This device cannot receive and transmit simultaneously");
    else if (r.frame.isEmpty() || r.frame.size() > 65536) error = QStringLiteral("Invalid packet length");
    else if (r.timed && (!c.timedTransmit || (!r.useHardwareTicks && !s.continuous) || r.clockId.isEmpty() ||
                        r.clockId != s.clockId || r.epoch != s.epoch || (!r.useHardwareTicks && r.targetSample < s.firstSample)))
        error = QStringLiteral("Timed TX requires a matching continuous hardware clock and epoch");
    else if (r.timed && (c.requiresHardwareTime || r.useHardwareTicks) && (!r.useHardwareTicks || !s.hardwareTimeValid || !s.clockLocked || s.deviceSerial.isEmpty() ||
             !s.epoch || !std::isfinite(s.hardwareTicksPerSecond) || s.hardwareTicksPerSecond <= 0.0))
        error = QStringLiteral("Timed TX needs verified device ticks, a locked clock and serial identity");
    else if (r.timed && r.useHardwareTicks && (r.minimumLeadTicks > std::numeric_limits<std::uint64_t>::max() - s.hardwareTick ||
             r.targetHardwareTick < s.hardwareTick + r.minimumLeadTicks))
        error = QStringLiteral("Timed TX is late or its lead-time calculation overflowed");
    else { error.clear(); return true; }
    return false;
}
BackendCapabilities usrpB210PlanningCapabilities() {
    BackendCapabilities c;
    c.id = "usrp-b210-planning-only"; c.planningOnly = true;
    c.receivesIq = c.transmitsIq = true;
    c.fullDuplex = c.timedTransmit = c.requiresHardwareTime = true;
    c.receiveChannels = c.transmitChannels = 2;
    c.externalReference = c.externalPps = c.sharedRxLo = c.sharedTxLo = true;
    c.minimumFrequencyHz = 70e6; c.maximumFrequencyHz = 6e9;
    c.maximumSampleRateHz = 61.44e6; c.maximumDualChannelSampleRateHz = 30.72e6; c.maximumBandwidthHz = 56e6;
    // UHD must replace these nominal values with probed/coerced settings. No PHY/MAC TX is claimed here.
    return c;
}
}

#ifndef WIFI_PHY_DIAGNOSTICS_H
#define WIFI_PHY_DIAGNOSTICS_H

#include <QByteArray>
#include <QJsonObject>

struct WifiPhyDiagnostics {
    quint64 htSigCrcOk = 0;
    quint64 htDataCrcPass = 0;
    quint64 htDataCrcFail = 0;
    quint64 vhtSigCrcOk = 0;
    quint64 vhtDataCrcPass = 0;
    quint64 vhtDataCrcFail = 0;
    quint64 vhtBw20 = 0;
    quint64 vhtBw40 = 0;
    quint64 vhtBw80 = 0;
    quint64 vhtBw160 = 0;

    bool observe(const QByteArray &line) {
        if (line.startsWith("[HT-SIG]")) {
            if (line.contains("CRC-OK")) ++htSigCrcOk;
            return true;
        }
        if (line.startsWith("[VHT-SIG-A]")) {
            if (line.contains("CRC-OK")) {
                ++vhtSigCrcOk;
                const int start = line.indexOf(" bw") + 3;
                const int end = line.indexOf(' ', start);
                if (start >= 3 && end > start) {
                    const int bandwidth = line.mid(start, end - start).toInt();
                    if (bandwidth == 20) ++vhtBw20;
                    else if (bandwidth == 40) ++vhtBw40;
                    else if (bandwidth == 80) ++vhtBw80;
                    else if (bandwidth == 160) ++vhtBw160;
                }
            }
            return true;
        }
        if (line.startsWith("[HT-DATA]") || line.startsWith("[STBC-DATA]") ||
            line.startsWith("[LDPC-DATA]")) {
            if (line.contains("CRC-32 PASS")) ++htDataCrcPass;
            else if (line.contains("CRC-32 fail")) ++htDataCrcFail;
            return true;
        }
        if (line.startsWith("[VHT-DATA]")) {
            if (line.contains("CRC-32 PASS")) ++vhtDataCrcPass;
            else if (line.contains("CRC-32 fail")) ++vhtDataCrcFail;
            return true;
        }
        return false;
    }

    QJsonObject htJson() const {
        return {{QStringLiteral("ht_sig_crc_ok"), static_cast<double>(htSigCrcOk)},
                {QStringLiteral("ht_data_crc_pass"), static_cast<double>(htDataCrcPass)},
                {QStringLiteral("ht_data_crc_fail"), static_cast<double>(htDataCrcFail)}};
    }
    QJsonObject vhtJson() const {
        return {{QStringLiteral("vht_sig_crc_ok"), static_cast<double>(vhtSigCrcOk)},
                {QStringLiteral("vht_data_crc_pass"), static_cast<double>(vhtDataCrcPass)},
                {QStringLiteral("vht_data_crc_fail"), static_cast<double>(vhtDataCrcFail)},
                {QStringLiteral("bandwidth_mhz"), QJsonObject{
                    {QStringLiteral("20"), static_cast<double>(vhtBw20)},
                    {QStringLiteral("40"), static_cast<double>(vhtBw40)},
                    {QStringLiteral("80"), static_cast<double>(vhtBw80)},
                    {QStringLiteral("160"), static_cast<double>(vhtBw160)}}}};
    }
};

#endif

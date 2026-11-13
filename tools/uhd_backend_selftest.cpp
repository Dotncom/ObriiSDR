#include "backends/uhdbackend.h"

#include <QCoreApplication>
#include <QTextStream>
#include <algorithm>

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    QString error;
    const QStringList devices = enumerateUhdDeviceArgs(&error);
    QTextStream output(stdout);
    const auto ubx = validatedUhdGnssUbx("B5620A0400000E34\nb5620a0400000e35\n"
                                        "b5620a0401000f36\n b5620a0400000e34\n");
    if (ubx != QLatin1String("b5620a0400000e34")) {
        output << "UBX frame validation failed\n";
        return 1;
    }
    const auto sky = summarizeUhdGnssGsv(QStringLiteral(
        "$GPGSV,3,1,09,09,,,17,10,,,40,12,,,49,13,,,35,1*6F\n"
        "$GPGSV,3,2,09,15,,,44,17,,,45,19,,,44,24,,,50,1*64\n"
        "$GPGSV,3,3,09,25,,,40,1*6E\n"
        "$GPGSV,1,1,03,12,42,152,42,24,12,142,47,32,,,37,5*62\n"
        "$GPGSV,1,1,01,03,05,218,,0*59\n"
        "$GLGSV,1,1,01,67,17,271,35*51\n"
        "$GAGSV,1,1,00,2*76"));
    const auto incomplete = summarizeUhdGnssGsv(QStringLiteral("$GPGSV,3,2,09,15,,,44,17,,,45,19,,,44,24,,,50,1*64"));
    const auto empty = summarizeUhdGnssGsv(QStringLiteral("$GAGSV,1,1,00,2*76"));
    const auto invalid = summarizeUhdGnssGsv(QStringLiteral("$GAGSV,1,1,00,2*00"));
    const auto gps24 = std::find_if(sky.satellites.begin(), sky.satellites.end(), [](const auto &satellite) {
        return satellite.talker == QLatin1String("GP") && satellite.id == 24;
    });
    const auto gps03 = std::find_if(sky.satellites.begin(), sky.satellites.end(), [](const auto &satellite) {
        return satellite.talker == QLatin1String("GP") && satellite.id == 3;
    });
    if (sky.satellitesInView != 12 || sky.satellitesWithSignal != 11 ||
        sky.strongestCn0DbHz != 50 || sky.incomplete || sky.satellites.size() != 12 ||
        gps24 == sky.satellites.end() || gps24->elevationDeg != 12 || gps24->azimuthDeg != 142 || gps24->cn0DbHz != 50 ||
        gps03 == sky.satellites.end() || gps03->elevationDeg != 5 || gps03->azimuthDeg != 218 || gps03->cn0DbHz != -1 ||
        incomplete.satellitesInView != -1 || !incomplete.incomplete || !incomplete.satellites.empty() ||
        empty.satellitesInView != 0 || empty.incomplete || !empty.satellites.empty() ||
        invalid.satellitesInView != -1 || !invalid.incomplete || !invalid.satellites.empty()) {
        output << "GSV summary failed\n";
        return 1;
    }
    output << "GSV summary: OK\n";
    output << "UHD devices: " << devices.size() << '\n';
    for (int i = 0; i < devices.size(); ++i) output << i << ": " << devices.at(i) << '\n';
    if (!error.isEmpty()) output << "UHD status: " << error << '\n';
    UhdRxSession session;
    QString openError;
    if (devices.isEmpty() && session.open(0, 0, 1000000.0, 100000000.0,
                                          30.0, QString(), QString(), &openError)) {
        output << "Unexpected device open success\n";
        return 1;
    }
    output << "No-device handling: OK\n";
    return 0;
}

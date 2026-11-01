#include "backends/uhdbackend.h"

#include <QCoreApplication>
#include <QTextStream>

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    QString error;
    const QStringList devices = enumerateUhdDeviceArgs(&error);
    QTextStream output(stdout);
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

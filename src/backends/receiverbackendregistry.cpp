#include "receiverbackendregistry.h"

#include "fobosbackend.h"

#include <QDebug>

namespace {

ReceiverBackendRegistration makeRegistration(const QString &id,
                                             const QString &displayName,
                                             ReceiverBackendStreamKind streamKind,
                                             ReceiverBackendFeatures features,
                                             bool builtIn,
                                             bool implemented,
                                             const QString &status) {
    ReceiverBackendRegistration registration;
    registration.id = id;
    registration.displayName = displayName;
    registration.streamKind = streamKind;
    registration.features = features;
    registration.builtIn = builtIn;
    registration.implemented = implemented;
    registration.status = status;
    return registration;
}

} // namespace

QList<ReceiverBackendRegistration> receiverBackendRegistrySnapshot() {
    return {
        makeRegistration(QStringLiteral("fobos-standard"),
                         QStringLiteral("Fobos SDR"),
                         ReceiverBackendStreamKind::FobosStandard,
                         fobosBackendFeatures(FobosApiKind::Standard),
                         true,
                         true,
                         QStringLiteral("Ready")),
        makeRegistration(QStringLiteral("fobos-agile"),
                         QStringLiteral("Fobos SDR Agile"),
                         ReceiverBackendStreamKind::FobosAgile,
                         fobosBackendFeatures(FobosApiKind::Agile),
                         true,
                         true,
                         QStringLiteral("Ready")),
        makeRegistration(QStringLiteral("rtl-tcp"),
                         QStringLiteral("RTL-SDR via rtl_tcp"),
                         ReceiverBackendStreamKind::RtlTcp,
                         ReceiverBackendFeatures(),
                         false,
                         true,
                         QStringLiteral("Ready when rtl_tcp is running")),
        makeRegistration(QStringLiteral("rtl-sdr-native"),
                         QStringLiteral("RTL-SDR native"),
                         ReceiverBackendStreamKind::RtlSdrNative,
                         ReceiverBackendFeatures(),
                         false,
                         true,
                         QStringLiteral("Ready when rtlsdr.dll/librtlsdr.dll is available")),
        makeRegistration(QStringLiteral("soapy-sdr"),
                         QStringLiteral("SoapySDR"),
                         ReceiverBackendStreamKind::SoapySdr,
                         ReceiverBackendFeature::HardwareBandwidth |
                             ReceiverBackendFeature::PpmCorrection,
                         false,
                         true,
                         QStringLiteral("Ready when SoapySDR runtime and device modules are available")),
        makeRegistration(QStringLiteral("uhd-native"),
                         QStringLiteral("USRP UHD native RX"),
                         ReceiverBackendStreamKind::UhdNative,
                         ReceiverBackendFeatures(),
                         false,
                         true,
                         QStringLiteral("RX ready when UHD C runtime and device are available; hardware validation pending")),
        makeRegistration(QStringLiteral("bladerf-native"),
                         QStringLiteral("bladeRF native"),
                         ReceiverBackendStreamKind::BladeRfNative,
                         ReceiverBackendFeature::HardwareBandwidth |
                             ReceiverBackendFeature::PpmCorrection,
                         false,
                         false,
                         QStringLiteral("Disabled until validated with physical bladeRF hardware")),
        makeRegistration(QStringLiteral("virtual-signal"),
                         QStringLiteral("Virtual IQ receiver"),
                         ReceiverBackendStreamKind::VirtualSignal,
                         ReceiverBackendFeatures(),
                         true,
                         true,
                         QStringLiteral("Local IPC or TCP CF32 stream"))
    };
}

ReceiverBackendRegistration receiverBackendRegistrationForId(const QString &id) {
    const QList<ReceiverBackendRegistration> registrations = receiverBackendRegistrySnapshot();
    for (const ReceiverBackendRegistration &registration : registrations) {
        if (registration.id == id) {
            return registration;
        }
    }
    return {};
}

void logReceiverBackendRegistry() {
    qDebug() << "[Receiver backend] registry";
    const QList<ReceiverBackendRegistration> registrations = receiverBackendRegistrySnapshot();
    for (const ReceiverBackendRegistration &registration : registrations) {
        qDebug() << "[Receiver backend]"
                 << registration.id
                 << registration.displayName
                 << "implemented" << registration.implemented
                 << "builtIn" << registration.builtIn
                 << "status" << registration.status;
    }
}

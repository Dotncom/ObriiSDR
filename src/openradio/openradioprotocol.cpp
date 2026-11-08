#include "openradioprotocol.h"

QList<OpenRadioProtocolDescriptor> OpenRadioProtocolCatalog::protocols() {
    return {
        {OpenRadioProtocol::M17, QStringLiteral("m17"), QStringLiteral("M17"),
         OpenRadioReadiness::BuiltInFraming, true, true, false, true, true, false,
         4800, 9000, QStringLiteral("libm17 1.4.x"),
         QStringLiteral("Framing and symbols are available; live RF timing and Codec 2 audio require hardware validation.")},
        {OpenRadioProtocol::Ax25Kiss, QStringLiteral("ax25-kiss"), QStringLiteral("AX.25 / KISS"),
         OpenRadioReadiness::BuiltInFraming, true, false, false, true, true, false,
         1200, 3000, QStringLiteral("Obrii native framing + offline AFSK1200"),
         QStringLiteral("UI/KISS frames and offline AFSK1200 WAV are implemented; live RF/TNC and G3RUH are not validated.")},
        {OpenRadioProtocol::Npr70, QStringLiteral("npr70"), QStringLiteral("NPR-70"),
         OpenRadioReadiness::ExternalBackend, true, false, true, false, false, true,
         0, 500000, QStringLiteral("External npr70-compatible modem"),
         QStringLiteral("Control/TAP adapter contract only until interoperable hardware captures are available.")},
        {OpenRadioProtocol::Aredn, QStringLiteral("aredn"), QStringLiteral("AREDN / OpenWrt"),
         OpenRadioReadiness::ExternalBackend, true, true, true, false, false, true,
         0, 20000000, QStringLiteral("TAP/Ethernet boundary"),
         QStringLiteral("Uses supported Wi-Fi/OpenWrt hardware; it is not an SDR 802.11 MAC.")},
        {OpenRadioProtocol::FreeDv, QStringLiteral("freedv"), QStringLiteral("FreeDV / Codec 2"),
         OpenRadioReadiness::ExternalBackend, false, true, false, false, false, false,
         0, 3000, QStringLiteral("FreeDV/Codec 2 backend"),
         QStringLiteral("Backend API is reserved; runtime and real-air interoperability are not bundled yet.")},
        {OpenRadioProtocol::Ardop, QStringLiteral("ardop"), QStringLiteral("ARDOP"),
         OpenRadioReadiness::ExternalBackend, true, false, false, false, false, false,
         0, 2500, QStringLiteral("ARDOP modem backend"),
         QStringLiteral("Backend API is reserved; no native ARDOP waveform is claimed.")},
        {OpenRadioProtocol::ExperimentalOfdmTdma, QStringLiteral("open-ofdm-tdma"),
         QStringLiteral("Open OFDM / TDMA"), OpenRadioReadiness::HardwareDeferred,
         true, true, true, false, false, true, 0, 0,
         QStringLiteral("Future complex-IQ modem + TAP"),
         QStringLiteral("Research contract only; requires two interoperable implementations and legal RF profiles.")}
    };
}

OpenRadioProtocolDescriptor OpenRadioProtocolCatalog::descriptor(OpenRadioProtocol protocol) {
    for (const OpenRadioProtocolDescriptor &entry : protocols()) {
        if (entry.protocol == protocol) return entry;
    }
    return {};
}

QString OpenRadioProtocolCatalog::readinessName(OpenRadioReadiness readiness, bool ukrainian) {
    switch (readiness) {
    case OpenRadioReadiness::BuiltInFraming:
        return ukrainian ? QString::fromUtf8(u8"Вбудований framing") : QStringLiteral("Built-in framing");
    case OpenRadioReadiness::ExternalBackend:
        return ukrainian ? QString::fromUtf8(u8"Зовнішній backend") : QStringLiteral("External backend");
    case OpenRadioReadiness::HardwareDeferred:
        return ukrainian ? QString::fromUtf8(u8"Очікує обладнання") : QStringLiteral("Hardware deferred");
    }
    return {};
}

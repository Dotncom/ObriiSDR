#include "fobosreceivercore.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>

namespace {
int checks = 0, calls = 0, result = 0, selected = -1;
void *seen = nullptr;
double argument = 0.0;
uint32_t block = 0, bufferCount = 0;
float *seenIq = nullptr;
int standardToken = 1, agileToken = 2;
void check(bool value) { ++checks; if (!value) { std::fprintf(stderr, "check %d failed\n", checks); std::exit(1); } }
int note(void *native, int api, double value = 0) {
    ++calls; seen = native; selected = api; argument = value; return result;
}
}

int getFobosStandardDeviceCountSafely() { return 2; }
int getFobosAgileDeviceCountSafely() { return 3; }
int openFobosDeviceSafely(fobos_dev_t **dev, uint32_t index) {
    const int rc = note(nullptr, 0, index); if (!rc) *dev = reinterpret_cast<fobos_dev_t *>(&standardToken); return rc;
}
int openFobosAgileDeviceSafely(fobos_sdr_dev_t **dev, uint32_t index) {
    const int rc = note(nullptr, 1, index); if (!rc) *dev = reinterpret_cast<fobos_sdr_dev_t *>(&agileToken); return rc;
}
#define SIMPLE_WRAPPERS(SUFFIX, TYPE, API) \
int closeFobos##SUFFIX##DeviceSafely(TYPE *dev) { return note(dev, API); }
// The standard API names omit the Standard suffix.
SIMPLE_WRAPPERS(, fobos_dev_t, 0)
SIMPLE_WRAPPERS(Agile, fobos_sdr_dev_t, 1)
#undef SIMPLE_WRAPPERS
#define SETTINGS_WRAPPERS(SUFFIX, TYPE, API, CLOCK_TYPE) \
int setFobos##SUFFIX##ClockSourceSafely(TYPE *dev, CLOCK_TYPE n) { return note(dev, API, n); } \
int setFobos##SUFFIX##DirectSamplingSafely(TYPE *dev, unsigned int n) { return note(dev, API, n); } \
int setFobos##SUFFIX##LnaGainSafely(TYPE *dev, unsigned int n) { return note(dev, API, n); } \
int setFobos##SUFFIX##VgaGainSafely(TYPE *dev, unsigned int n) { return note(dev, API, n); } \
int setFobos##SUFFIX##GpoSafely(TYPE *dev, uint8_t n) { return note(dev, API, n); } \
int startFobos##SUFFIX##SyncSafely(TYPE *dev, uint32_t n) { block = n; return note(dev, API); } \
int stopFobos##SUFFIX##SyncSafely(TYPE *dev) { return note(dev, API); } \
int cancelFobos##SUFFIX##AsyncSafely(TYPE *dev) { return note(dev, API); } \
int readFobos##SUFFIX##SyncSafely(TYPE *dev, float *iq, uint32_t *n) { \
    seenIq = iq; *n = result == 0 ? 2 : 0; if (!result) { iq[0] = .25f; iq[1] = -.5f; } return note(dev, API); }
SETTINGS_WRAPPERS(, fobos_dev_t, 0, unsigned int)
SETTINGS_WRAPPERS(Agile, fobos_sdr_dev_t, 1, int)
#undef SETTINGS_WRAPPERS
int getFobosBoardInfoSafely(fobos_dev_t *dev, char *hw, char *, char *, char *, char *) {
    if (!result) std::memcpy(hw, "standard", 9); return note(dev, 0);
}
int getFobosAgileBoardInfoSafely(fobos_sdr_dev_t *dev, char *hw, char *, char *, char *, char *) {
    if (!result) std::memcpy(hw, "agile", 6); return note(dev, 1);
}
int getFobosSampleRatesSafely(fobos_dev_t *dev, double *rates, unsigned int *count) {
    *count = 1; if (rates) rates[0] = 8000000; return note(dev, 0);
}
int getFobosAgileSampleRatesSafely(fobos_sdr_dev_t *dev, double *rates, unsigned int *count) {
    *count = 1; if (rates) rates[0] = 16000000; return note(dev, 1);
}
int setFobosSampleRateSafely(fobos_dev_t *dev, double n, double *actual) {
    if (!result && actual) *actual = n - 1; return note(dev, 0, n);
}
int setFobosFrequencySafely(fobos_dev_t *dev, double n, double *actual) {
    if (!result && actual) *actual = n + 1; return note(dev, 0, n);
}
int setFobosAgileSampleRateSafely(fobos_sdr_dev_t *dev, double n) { return note(dev, 1, n); }
int setFobosAgileFrequencySafely(fobos_sdr_dev_t *dev, double n) { return note(dev, 1, n); }
int readFobosAsyncSafely(fobos_dev_t *dev, void (*callback)(float *, uint32_t, void *),
                        void *ctx, uint32_t buffers, uint32_t samples) {
    bufferCount = buffers; block = samples; float iq[4]{};
    if (!result) callback(iq, 2, ctx); return note(dev, 0);
}
int readFobosAgileAsyncSafely(fobos_sdr_dev_t *dev,
    void (*callback)(float *, uint32_t, fobos_sdr_dev_t *, void *), void *ctx, uint32_t buffers, uint32_t samples) {
    bufferCount = buffers; block = samples; float iq[4]{};
    if (!result) callback(iq, 2, dev, ctx); return note(dev, 1);
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    static_assert(std::is_trivially_copyable<FobosReceiverCore>::value, "Borrowed handle must be allocation free");
    FobosReceiverCore empty;
    float iq[4]{}; uint32_t count = 999;
    check(empty.readSync(iq, &count) == FOBOS_ERR_NOT_OPEN && count == 0 && calls == 0);
    check(empty.startSync(512) == FOBOS_ERR_NOT_OPEN && empty.cancelAsync() == FOBOS_ERR_NOT_OPEN);
    check(empty.close() == 0 && calls == 0);
    check(FobosReceiverCore::deviceCount(FobosApiKind::Standard) == 2);
    check(FobosReceiverCore::deviceCount(FobosApiKind::Agile) == 3);
    for (const auto api : {FobosApiKind::Standard, FobosApiKind::Agile}) {
        const int index = api == FobosApiKind::Agile ? 1 : 0;
        FobosReceiverCore receiver;
        result = -41;
        check(receiver.open(api, 4) == -41 && !receiver.isOpen());
        result = 0;
        check(receiver.open(api, 4) == 0 && receiver.isOpen() && selected == index && argument == 4);
        void *native = receiver.native();
        const int before = calls;
        check(receiver.open(api, 5) == FOBOS_ERR_BAD_PARAM && calls == before && receiver.native() == native);
        { FobosReceiverCore borrowed(native, api); check(borrowed.native() == native); }
        check(calls == before);
        char hw[32]{}; check(receiver.boardInfo(hw, hw, hw, hw, hw) == 0);
        check(std::strcmp(hw, index ? "agile" : "standard") == 0);
        double rates[2]{}; unsigned int rateCount = 0;
        check(receiver.sampleRates(rates, &rateCount) == 0 && rateCount == 1 && rates[0] == (index ? 16000000 : 8000000));
        check(receiver.clockSource(1) == 0 && selected == index && seen == native && argument == 1);
        check(receiver.directSampling(2) == 0 && argument == 2);
        check(receiver.lna(3) == 0 && argument == 3);
        check(receiver.vga(4) == 0 && argument == 4);
        check(receiver.gpo(0xa5) == 0 && argument == 0xa5);
        double actual = 123;
        check(receiver.sampleRate(8000000, &actual) == 0 && actual == (index ? 8000000 : 7999999));
        check(receiver.frequency(100000000, &actual) == 0 && actual == (index ? 100000000 : 100000001));
        result = -42; actual = 123;
        check(receiver.sampleRate(16e6, &actual) == -42 && actual == 123);
        check(receiver.frequency(99e6, &actual) == -42 && actual == 123);
        result = 0;
        check(receiver.startSync(262144) == 0 && block == 262144 && seen == native);
        check(receiver.readSync(iq, &count) == 0 && count == 2 && seenIq == iq && iq[0] == .25f && iq[1] == -.5f);
        const int prior = calls;
        check(receiver.readSync(nullptr, &count) == FOBOS_ERR_BAD_PARAM && count == 0 && calls == prior);
        check(receiver.stopSync() == 0 && seen == native && selected == index);
        struct Context { void *native; int received = 0; } ctx{native};
        check(receiver.readAsync([](float *buffer, uint32_t n, void *dev, void *user) {
            auto *ctx = static_cast<Context *>(user);
            check(buffer && n == 2 && dev == ctx->native); ++ctx->received;
        }, &ctx, 32, 262144) == 0);
        check(ctx.received == 1 && bufferCount == 32 && block == 262144);
        check(receiver.cancelAsync() == 0 && seen == native);
        result = -43; check(receiver.close() == -43 && receiver.isOpen());
        result = 0; check(receiver.close() == 0 && !receiver.isOpen());
        const int closed = calls; check(receiver.close() == 0 && calls == closed);
    }
    std::puts(QJsonDocument(QJsonObject{{"ok", true}, {"checks", checks},
        {"borrowedHandleBytes", static_cast<int>(sizeof(FobosReceiverCore))},
        {"hardwareOpened", false}, {"iqCopies", 0}, {"workerThreadsAdded", 0}}).toJson(QJsonDocument::Compact).constData());
    return 0;
}

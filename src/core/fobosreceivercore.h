#ifndef OBRII_FOBOS_RECEIVER_CORE_H
#define OBRII_FOBOS_RECEIVER_CORE_H

#include "../backends/fobosbackend.h"

// Non-owning native session. The caller retains its existing worker, buffers and
// stop/join/close order; destroying a borrowed view never touches the device.
class FobosReceiverCore {
public:
    using Callback = void (*)(float *, uint32_t, void *, void *);

    FobosReceiverCore() = default;
    FobosReceiverCore(void *native, FobosApiKind api) : native_(native), api_(api) {}

    void *native() const { return native_; }
    FobosApiKind api() const { return api_; }
    bool isOpen() const { return native_ != nullptr; }

    static int deviceCount(FobosApiKind api) {
        return api == FobosApiKind::Agile ? getFobosAgileDeviceCountSafely()
                                         : getFobosStandardDeviceCountSafely();
    }
    int open(FobosApiKind api, uint32_t index) {
        if (native_) return FOBOS_ERR_BAD_PARAM;
        int result;
        if (api == FobosApiKind::Agile) {
            fobos_sdr_dev_t *dev = nullptr;
            result = openFobosAgileDeviceSafely(&dev, index);
            if (result == 0) native_ = dev;
        } else {
            fobos_dev_t *dev = nullptr;
            result = openFobosDeviceSafely(&dev, index);
            if (result == 0) native_ = dev;
        }
        api_ = api;
        return result;
    }
    int close() {
        if (!native_) return FOBOS_ERR_OK;
        const int result = api_ == FobosApiKind::Agile
            ? closeFobosAgileDeviceSafely(agile()) : closeFobosDeviceSafely(standard());
        if (result == 0) native_ = nullptr;
        return result;
    }
    int boardInfo(char *hw, char *fw, char *maker, char *product, char *serial) const {
        if (!native_) return FOBOS_ERR_NOT_OPEN;
        return api_ == FobosApiKind::Agile
            ? getFobosAgileBoardInfoSafely(agile(), hw, fw, maker, product, serial)
            : getFobosBoardInfoSafely(standard(), hw, fw, maker, product, serial);
    }
    int sampleRates(double *values, unsigned int *count) const {
        if (!native_) return FOBOS_ERR_NOT_OPEN;
        return api_ == FobosApiKind::Agile ? getFobosAgileSampleRatesSafely(agile(), values, count)
                                         : getFobosSampleRatesSafely(standard(), values, count);
    }
    int clockSource(int value) const {
        if (!native_) return FOBOS_ERR_NOT_OPEN;
        return api_ == FobosApiKind::Agile ? setFobosAgileClockSourceSafely(agile(), value)
            : setFobosClockSourceSafely(standard(), static_cast<unsigned int>(value));
    }
    int directSampling(unsigned int enabled) const {
        if (!native_) return FOBOS_ERR_NOT_OPEN;
        return api_ == FobosApiKind::Agile ? setFobosAgileDirectSamplingSafely(agile(), enabled)
                                         : setFobosDirectSamplingSafely(standard(), enabled);
    }
    int sampleRate(double requested, double *actual) const {
        if (!native_) return FOBOS_ERR_NOT_OPEN;
        if (api_ == FobosApiKind::Standard)
            return setFobosSampleRateSafely(standard(), requested, actual);
        const int result = setFobosAgileSampleRateSafely(agile(), requested);
        if (result == 0 && actual) *actual = requested;
        return result;
    }
    int frequency(double requested, double *actual) const {
        if (!native_) return FOBOS_ERR_NOT_OPEN;
        if (api_ == FobosApiKind::Standard)
            return setFobosFrequencySafely(standard(), requested, actual);
        const int result = setFobosAgileFrequencySafely(agile(), requested);
        if (result == 0 && actual) *actual = requested;
        return result;
    }
    int lna(unsigned int value) const {
        if (!native_) return FOBOS_ERR_NOT_OPEN;
        return api_ == FobosApiKind::Agile ? setFobosAgileLnaGainSafely(agile(), value)
                                         : setFobosLnaGainSafely(standard(), value);
    }
    int vga(unsigned int value) const {
        if (!native_) return FOBOS_ERR_NOT_OPEN;
        return api_ == FobosApiKind::Agile ? setFobosAgileVgaGainSafely(agile(), value)
                                         : setFobosVgaGainSafely(standard(), value);
    }
    int gpo(uint8_t value) const {
        if (!native_) return FOBOS_ERR_NOT_OPEN;
        return api_ == FobosApiKind::Agile ? setFobosAgileGpoSafely(agile(), value)
                                         : setFobosGpoSafely(standard(), value);
    }
    int startSync(uint32_t blockSamples) const {
        if (!native_) return FOBOS_ERR_NOT_OPEN;
        return api_ == FobosApiKind::Agile ? startFobosAgileSyncSafely(agile(), blockSamples)
                                         : startFobosSyncSafely(standard(), blockSamples);
    }
    int readSync(float *iq, uint32_t *count) const {
        if (count) *count = 0;
        if (!native_) return FOBOS_ERR_NOT_OPEN;
        if (!iq || !count) return FOBOS_ERR_BAD_PARAM;
        return api_ == FobosApiKind::Agile ? readFobosAgileSyncSafely(agile(), iq, count)
                                         : readFobosSyncSafely(standard(), iq, count);
    }
    int stopSync() const {
        if (!native_) return FOBOS_ERR_NOT_OPEN;
        return api_ == FobosApiKind::Agile ? stopFobosAgileSyncSafely(agile())
                                         : stopFobosSyncSafely(standard());
    }
    int cancelAsync() const {
        if (!native_) return FOBOS_ERR_NOT_OPEN;
        return api_ == FobosApiKind::Agile ? cancelFobosAgileAsyncSafely(agile())
                                         : cancelFobosAsyncSafely(standard());
    }
    int readAsync(Callback callback, void *context, uint32_t buffers, uint32_t samples) const {
        if (!native_) return FOBOS_ERR_NOT_OPEN;
        if (!callback) return FOBOS_ERR_BAD_PARAM;
        // Native read_async blocks until cancellation: this bridge lives for all callbacks.
        Bridge bridge{callback, context, native_};
        if (api_ == FobosApiKind::Agile)
            return readFobosAgileAsyncSafely(agile(), [](float *iq, uint32_t n,
                fobos_sdr_dev_t *dev, void *ctx) {
                    const auto &b = *static_cast<Bridge *>(ctx);
                    b.callback(iq, n, dev, b.context);
                }, &bridge, buffers, samples);
        return readFobosAsyncSafely(standard(), [](float *iq, uint32_t n, void *ctx) {
            const auto &b = *static_cast<Bridge *>(ctx);
            b.callback(iq, n, b.native, b.context);
        }, &bridge, buffers, samples);
    }

private:
    struct Bridge { Callback callback; void *context; void *native; };
    fobos_dev_t *standard() const { return static_cast<fobos_dev_t *>(native_); }
    fobos_sdr_dev_t *agile() const { return static_cast<fobos_sdr_dev_t *>(native_); }
    void *native_ = nullptr;
    FobosApiKind api_ = FobosApiKind::Standard;
};

#endif

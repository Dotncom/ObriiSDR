#include "../transmit/hackrftransmitterbackend.h"
#include "../backends/hackrfbackend.h"

#include <QCoreApplication>

#include <algorithm>
#include <array>
#include <cstdio>

// This access shim never supplies a device or calls start/beginStreaming/libhackrf.
struct HackRfFiniteBurstTestAccess {
    static void armMemory(HackRfTransmitterBackend &tx, int sourceRate = 2000000,
                          int deviceRate = 2000000) {
        tx.currentConfiguration.sampleRate = sourceRate;
        tx.currentConfiguration.deviceSampleRate = deviceRate;
        tx.running.store(true);
    }
    static bool stage(HackRfTransmitterBackend &tx, const std::complex<float> *iq,
                      int count, QString *error) {
        return tx.stageFiniteBurst(iq, count, error);
    }
    static int transfer(HackRfTransmitterBackend &tx, unsigned char *buffer, int bytes,
                        int *valid) {
        HackRfTransfer transfer;
        transfer.buffer = buffer;
        transfer.bufferLength = bytes;
        transfer.txContext = &tx;
        const int result = HackRfTransmitterBackend::transferCallback(&transfer);
        *valid = transfer.validLength;
        return result;
    }
    static void flush(HackRfTransmitterBackend &tx, int success) {
        HackRfTransmitterBackend::flushCallback(&tx, success);
    }
    static void blockComplete(HackRfTransmitterBackend &tx, int success) {
        HackRfTransfer transfer;
        transfer.txContext = &tx;
        HackRfTransmitterBackend::blockCompleteCallback(&transfer, success);
    }
    static void live(HackRfTransmitterBackend &tx) {
        tx.currentConfiguration.liveSource = true;
        tx.currentConfiguration.prebufferMilliseconds = 2000;
    }
};

namespace {
int errors = 0;
void check(bool okay, const char *label) {
    std::printf("%s %s\n", okay ? "PASS" : "FAIL", label);
    if (!okay) ++errors;
}
int signedByte(unsigned char value) { return value < 128 ? int(value) : int(value) - 256; }

void finalBufferThenFlush() {
    HackRfTransmitterBackend tx;
    HackRfFiniteBurstTestAccess::armMemory(tx);
    const std::array<std::complex<float>, 4> iq{{{0.25f, 0.0f}, {0.5f, 0.0f},
                                              {0.75f, 0.0f}, {0.875f, 0.0f}}};
    QString error;
    check(!tx.finiteBurstComplete() && !tx.finiteBurstSucceeded(), "new backend has no finite receipt");
    check(HackRfFiniteBurstTestAccess::stage(tx, iq.data(), int(iq.size()), &error), "stage entire finite input in memory");
    std::array<unsigned char, 6> buffer{};
    int valid = -1;
    check(HackRfFiniteBurstTestAccess::transfer(tx, buffer.data(), int(buffer.size()), &valid) == 0 &&
          valid == int(buffer.size()) && signedByte(buffer[0]) == 32 && signedByte(buffer[4]) == 95,
          "non-final buffer is handed off normally");
    check(!tx.finiteBurstComplete(), "buffer generation is not a TX completion");
    buffer.fill(0xff);
    check(HackRfFiniteBurstTestAccess::transfer(tx, buffer.data(), int(buffer.size()), &valid) == 0 &&
          valid == int(buffer.size()) && signedByte(buffer[0]) == 111 &&
          std::all_of(buffer.begin() + 1, buffer.end(), [](unsigned char x) { return x == 0; }),
          "last input sample and padded final buffer are handed off before EOS");
    HackRfFiniteBurstTestAccess::blockComplete(tx, 1);
    check(tx.bufferedSourceSamples() == 1 && !tx.finiteBurstComplete() &&
          tx.sourceUnderrunSamples() == 0, "source exhaustion and block handoff are not flush completion");
    check(!tx.drain(0, &error), "finite drain cannot infer success from an empty source queue");
    check(HackRfFiniteBurstTestAccess::transfer(tx, buffer.data(), int(buffer.size()), &valid) != 0 &&
          valid == 0 && !tx.finiteBurstComplete(), "only the next callback returns EOS, without declaring completion");
    HackRfFiniteBurstTestAccess::flush(tx, 1);
    check(tx.finiteBurstComplete() && tx.finiteBurstSucceeded() && tx.drain(0, &error), "successful flush publishes the finite TX receipt");
    check(!tx.writeIq(iq.data(), 1, &error), "finite stream cannot acquire additional continuous IQ");
    tx.stop();
    check(tx.finiteBurstSucceeded(), "cleanup preserves a successful receipt");
}

void singleSampleAndResampling() {
    HackRfTransmitterBackend tx;
    HackRfFiniteBurstTestAccess::armMemory(tx, 1000000, 2000000);
    const std::complex<float> sample(0.5f, -0.5f);
    QString error;
    check(HackRfFiniteBurstTestAccess::stage(tx, &sample, 1, &error), "single-sample finite input is valid");
    std::array<unsigned char, 8> buffer{};
    int valid = -1;
    check(HackRfFiniteBurstTestAccess::transfer(tx, buffer.data(), int(buffer.size()), &valid) == 0 &&
          valid == 8 && signedByte(buffer[0]) == 64 && signedByte(buffer[1]) == -64 &&
          signedByte(buffer[2]) == 32 && signedByte(buffer[3]) == -32 && buffer[4] == 0,
          "zero lookahead preserves the final sample through interpolation");
    check(HackRfFiniteBurstTestAccess::transfer(tx, buffer.data(), int(buffer.size()), &valid) != 0 &&
          valid == 0, "single-sample burst still hands off its final buffer before EOS");
}

void failedFlushAndTransfers() {
    const std::complex<float> sample(0.5f, 0.0f);
    for (int failure = 0; failure < 3; ++failure) {
        HackRfTransmitterBackend tx;
        HackRfFiniteBurstTestAccess::armMemory(tx);
        QString error;
        check(HackRfFiniteBurstTestAccess::stage(tx, &sample, 1, &error), "failure case stages without hardware");
        std::array<unsigned char, 2> buffer{};
        int valid = -1;
        HackRfFiniteBurstTestAccess::transfer(tx, buffer.data(), int(buffer.size()), &valid);
        check(valid == 2 && !tx.finiteBurstComplete(), "exact-full final buffer is not premature completion");
        if (failure != 2)
            HackRfFiniteBurstTestAccess::transfer(tx, buffer.data(), int(buffer.size()), &valid);
        if (failure == 1) HackRfFiniteBurstTestAccess::blockComplete(tx, 0);
        HackRfFiniteBurstTestAccess::flush(tx, failure == 0 ? 0 : 1);
        check(tx.finiteBurstComplete() && !tx.finiteBurstSucceeded(),
              failure == 0 ? "failed flush never reports success" :
              failure == 1 ? "USB block failure cannot be overwritten by a successful flush" :
                             "flush before EOS fails closed");
        check(!tx.drain(0, &error), "failed finite drain cannot report success");
        HackRfFiniteBurstTestAccess::flush(tx, 1);
        check(!tx.finiteBurstSucceeded(), "late callback cannot resurrect a failed burst");
    }
}

void queueCompaction() {
    HackRfTransmitterBackend tx;
    HackRfFiniteBurstTestAccess::armMemory(tx);
    QVector<std::complex<float>> iq(32770, std::complex<float>(0.25f, 0.0f));
    QString error;
    check(HackRfFiniteBurstTestAccess::stage(tx, iq.constData(), iq.size(), &error), "large finite input stages in memory");
    QVector<unsigned char> buffer(32769 * 2);
    int valid = -1;
    check(HackRfFiniteBurstTestAccess::transfer(tx, buffer.data(), buffer.size(), &valid) == 0 &&
          valid == buffer.size() && !tx.finiteBurstComplete(), "queue compaction keeps the burst active");
    check(HackRfFiniteBurstTestAccess::transfer(tx, buffer.data(), 2, &valid) == 0 &&
          valid == 2 && signedByte(buffer[0]) == 32, "compaction preserves the last input sample");
    check(HackRfFiniteBurstTestAccess::transfer(tx, buffer.data(), 2, &valid) != 0 && valid == 0,
          "compacted finite queue ends on the following callback");
    HackRfFiniteBurstTestAccess::flush(tx, 1);
    check(tx.finiteBurstSucceeded(), "compacted burst completes only on flush");
}

void cancellationAndBounds() {
    HackRfTransmitterBackend tx;
    HackRfFiniteBurstTestAccess::armMemory(tx, 8000, 2000000);
    const std::complex<float> sample(0.5f, 0.0f);
    QString error;
    check(!HackRfFiniteBurstTestAccess::stage(tx, nullptr, 1, &error) &&
          !HackRfFiniteBurstTestAccess::stage(tx, &sample, 0, &error) &&
          !HackRfFiniteBurstTestAccess::stage(tx, &sample, 8000 * 180 + 1, &error),
          "invalid and oversized input is rejected before allocation/copy");
    check(HackRfFiniteBurstTestAccess::stage(tx, &sample, 1, &error), "bounded input stages after validation failures");
    check(!HackRfFiniteBurstTestAccess::stage(tx, &sample, 1, &error), "second finite submission cannot replace an active burst");
    tx.stop();
    HackRfFiniteBurstTestAccess::flush(tx, 1);
    check(tx.finiteBurstComplete() && !tx.finiteBurstSucceeded(), "cancellation remains failed despite a late successful flush");
}

void continuousZeroFill() {
    HackRfTransmitterBackend tx;
    HackRfFiniteBurstTestAccess::armMemory(tx);
    HackRfFiniteBurstTestAccess::live(tx);
    const std::array<std::complex<float>, 2> iq{{{0.5f, 0.0f}, {0.25f, 0.0f}}};
    QString error;
    check(!HackRfFiniteBurstTestAccess::stage(tx, iq.data(), int(iq.size()), &error), "finite path does not change a live session");
    check(tx.writeIq(iq.data(), int(iq.size()), &error) && !tx.streamStarted(),
          "ordinary live IQ still waits for its prebuffer without any device");
    std::array<unsigned char, 8> buffer{};
    int valid = -1;
    check(HackRfFiniteBurstTestAccess::transfer(tx, buffer.data(), int(buffer.size()), &valid) == 0 &&
          valid == int(buffer.size()) && signedByte(buffer[0]) == 64 && tx.sourceUnderrunSamples() == 3,
          "continuous interpolation and underrun accounting remain unchanged");
    buffer.fill(0xff);
    check(HackRfFiniteBurstTestAccess::transfer(tx, buffer.data(), int(buffer.size()), &valid) == 0 &&
          valid == int(buffer.size()) && std::all_of(buffer.begin(), buffer.end(), [](unsigned char x) { return x == 0; }) &&
          tx.sourceUnderrunSamples() == 7 && !tx.finiteBurstComplete(), "continuous source exhaustion keeps zero-filling, not EOS");
    check(tx.setOutputEnabled(false, &error) && tx.bufferedSourceSamples() == 0 &&
          tx.setOutputEnabled(true, &error) && tx.writeIq(iq.data(), int(iq.size()), &error),
          "ordinary PTT can still clear and refill continuous IQ");
}
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    finalBufferThenFlush();
    singleSampleAndResampling();
    failedFlushAndTransfers();
    queueCompaction();
    cancellationAndBounds();
    continuousZeroFill();
    return errors ? 1 : 0;
}

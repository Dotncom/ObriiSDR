#include "cliasyncspectrum.h"
#include "clispectrummeter.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
bool finish(CliAsyncSpectrum &worker, CliSpectrumResult &result) {
    QElapsedTimer timer; timer.start();
    while (timer.elapsed() < 10000) {
        if (worker.poll(result)) return true;
        if (!worker.busy()) return false;
        QThread::msleep(1);
    }
    throw std::runtime_error("bounded FFT completion");
}
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        for (const int size : {512, 1024, 65537, 1048576}) {
            CliAsyncSpectrum worker(size);
            require(worker.valid() && !worker.status().value("workerStarted").toBool(), "construction does not start a worker");
            std::vector<float> iq(size * 2);
            for (int i = 0; i < size; ++i) {
                const double phase = 6.28318530717958647692 * 17 * i / size;
                iq[2*i] = static_cast<float>(0.3 * std::cos(phase)); iq[2*i+1] = static_cast<float>(0.3 * std::sin(phase));
            }
            for (int offset = 0; offset < size;) {
                const int count = (std::min)(117, size - offset);
                worker.submit(iq.data() + offset * 2, count, 48000, 1000000, 0);
                offset += count;
            }
            QElapsedTimer submit; submit.start();
            for (int i = 0; i < 10000; ++i) worker.submit(iq.data(), size, 48000, 1000000, 0);
            const double submitMs = submit.nsecsElapsed() / 1e6;
            require(submitMs < 1000, "busy FFT never queues/copies input or blocks submission");
            const auto stats = worker.status();
            require(stats.value("submittedWindows").toString() == "1" && stats.value("maximumInFlightWindows").toInt() == 1,
                    "single bounded in-flight window");
            require(stats.value("skippedBusySamples").toString().toULongLong() == static_cast<quint64>(size) * 10000,
                    "all skipped spectrum-only input is accounted");
            CliSpectrumResult result;
            require(finish(worker, result) && result.error.isEmpty(), "asynchronous spectrum succeeds");
            if (size == 1048576) std::cout << "1M FFT compute: " << result.computeMs << " ms; 10000 busy submissions: " << submitMs << " ms\n";
            CliSpectrumMeter reference(size);
            std::vector<float> expected;
            reference.analyze(iq.data(), size, 48000, 1000000, &expected);
            require(result.trace.size() == expected.size(), "full FFT detail preserved");
            for (std::size_t i = 0; i < expected.size(); ++i)
                require(std::abs(result.trace[i] - expected[i]) < 1e-5, "same numerical spectrum as synchronous FFT");
            require(result.display.size() == 512, "display reduction is separately bounded");
            for (int i = 0; i < 512; ++i) {
                const auto first = static_cast<qint64>(i) * size / 512;
                const auto last = static_cast<qint64>(i + 1) * size / 512;
                require(result.display[i] == *std::max_element(expected.begin() + first, expected.begin() + last), "peak-preserving display reduction");
            }
            worker.submit(iq.data(), size, 48000, 1000000, 0);
            worker.reset();
            require(!finish(worker, result), "retuning/reset discards old asynchronous results");
            require(worker.status().value("discardedStaleWindows").toString() == "1", "stale result is counted");
            worker.submit(iq.data(), size, 48000, 1100000, 0);
            require(finish(worker, result) && result.centerHz == 1100000, "restart after invalidation");
        }
        require(!CliAsyncSpectrum(511).valid(), "invalid FFT rejected without worker");
        std::cout << "CLI async FFT passed: numerical parity, odd sizes, single-window ownership, nonblocking submit, counters, retune/reset\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}

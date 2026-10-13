#include "dspconnectionprobe.h"
#include "dspstreammanager.h"
#include "dspstreamtransport_p.h"
#include "spectrumcomputesession.h"
#include "iqbuffer.h"
#include <QHostAddress>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <thread>
#include <vector>

namespace {
using namespace DspConnectionProbeProtocol;
using namespace DspStreamTransport;
bool writePacket(QTcpSocket &socket, const QJsonObject &header, const QByteArray &data,
                 const std::atomic<bool> &stop) {
    return send(socket, DspStreamProtocol::encode(header, data), &stop, 3000);
}
double elapsedMs(const QElapsedTimer &timer) { return double(timer.nsecsElapsed()) / 1e6; }
bool validEndpoint(const DspStreamEndpoint &endpoint) {
    QHostAddress address;
    if (!endpoint.port || !address.setAddress(endpoint.address) || address.isMulticast() || address.isNull() ||
        address == QHostAddress::Any || address == QHostAddress::AnyIPv4 || address == QHostAddress::AnyIPv6 ||
        endpoint.token.size() < 32 || endpoint.token.size() > 128) return false;
    for (QChar c : endpoint.token) if (c.unicode() < 33 || c.unicode() > 126) return false;
    return true;
}
}

void DspConnectionProbeProtocol::serve(QTcpSocket &socket, QByteArray &buffer, const std::atomic<bool> &stop) {
    QElapsedTimer lifetime; lifetime.start();
    QJsonObject json; QByteArray payload;
    const auto read = [&]() {
        return lifetime.elapsed() < 20000 && receive(socket, buffer, json, payload, 4000, &stop);
    };
    for (int i = 0; i < PingCount; ++i) {
        if (!read() || !payload.isEmpty() || json.value("type") != "probe_ping" || json.value("id") != i) return;
        if (!writePacket(socket, {{"type", "probe_pong"}, {"id", i}}, {}, stop)) return;
    }
    if (!read() || !payload.isEmpty() || json.value("type") != "probe_upload_begin") return;
    if (!writePacket(socket, {{"type", "probe_upload_ready"}}, {}, stop)) return;
    QElapsedTimer upload; upload.start();
    int bytes = 0, sequence = 0;
    const QByteArray pattern(ChunkBytes, char(0x5a));
    for (;;) {
        if (!read() || upload.elapsed() > 8000) return;
        if (json.value("type") == "probe_upload_end") {
            if (!payload.isEmpty() || bytes <= 0 || json.value("bytes") != bytes) return;
            if (!writePacket(socket, {{"type", "probe_upload_done"}, {"bytes", bytes}}, {}, stop)) return;
            break;
        }
        if (json.value("type") != "probe_upload_data" || json.value("id") != sequence++ ||
            payload != pattern || bytes > MaxBytes - payload.size()) return;
        bytes += payload.size();
    }
    if (!read() || !payload.isEmpty() || json.value("type") != "probe_download") return;
    QElapsedTimer download; download.start();
    bytes = 0; sequence = 0;
    const QByteArray outgoing(ChunkBytes, char(0xa5));
    do {
        if (!writePacket(socket, {{"type", "probe_download_data"}, {"id", sequence++}}, outgoing, stop)) return;
        bytes += outgoing.size();
    } while (!stop && download.elapsed() < DurationMs && bytes < MaxBytes);
    writePacket(socket, {{"type", "probe_download_done"}, {"bytes", bytes}}, {}, stop);
}

struct DspConnectionProbe::Impl {
    std::atomic<bool> active{false}, stopping{false};
    std::thread thread;
    mutable std::mutex mutex;
    DspConnectionProbeResult snapshot;
    void publish(const DspConnectionProbeResult &result) {
        std::lock_guard<std::mutex> lock(mutex); snapshot = result;
    }
    void run(const DspStreamEndpoint &endpoint) {
        DspConnectionProbeResult r;
        const auto stage = [&](DspProbeStage value) { r.stage = value; publish(r); };
        const auto fail = [&](const QString &code, const QString &detail = QString()) {
            r.errorCode = stopping ? QString() : code;
            r.detail = stopping ? QString() : detail;
            stage(stopping ? DspProbeStage::Cancelled : DspProbeStage::Failed);
        };
        try {
            if (!validEndpoint(endpoint)) { fail("invalid_endpoint"); return; }
            QTcpSocket socket;
            socket.setReadBufferSize(PacketLimit);
            stage(DspProbeStage::Connecting);
            QElapsedTimer clock; clock.start();
            socket.connectToHost(QHostAddress(endpoint.address), endpoint.port);
            if (!stopping && socket.state() != QAbstractSocket::ConnectedState) socket.waitForConnected(1500);
            r.connectMs = elapsedMs(clock);
            if (stopping || socket.state() != QAbstractSocket::ConnectedState) { fail("tcp", socket.errorString()); return; }
            socket.setSocketOption(QAbstractSocket::LowDelayOption, 1);
            stage(DspProbeStage::Handshake);
            QByteArray buffer, payload; QJsonObject json;
            const auto write = [&](const QJsonObject &header, const QByteArray &data = QByteArray()) {
                return writePacket(socket, header, data, stopping);
            };
            const auto read = [&]() { return receive(socket, buffer, json, payload, 4000, &stopping); };
            clock.restart();
            if (!write({{"type", "hello"}, {"version", 1}, {"token", endpoint.token}, {"purpose", "connection_probe"}}) || !read()) {
                fail("handshake"); return;
            }
            r.handshakeMs = elapsedMs(clock);
            if (json.value("type") == "error") {
                const QString code = json.value("code").toString();
                fail(code == "authentication_failed" || code == "busy" ? code : QStringLiteral("protocol")); return;
            }
            if (!payload.isEmpty() || json.value("type") != "hello" || json.value("version") != 1) { fail("protocol"); return; }
            if (json.value("connectionProbeVersion") != Version) { fail("unsupported"); return; }
            if (json.value("mainFftVersion") == 1) r.mainFftMaxLength = json.value("mainFftMaxLength").toInt();
            stage(DspProbeStage::Latency);
            std::vector<double> rtts;
            for (int i = 0; i < PingCount; ++i) {
                clock.restart();
                if (!write({{"type", "probe_ping"}, {"id", i}}) || !read()) { fail("timeout"); return; }
                if (!payload.isEmpty() || json.value("type") != "probe_pong" || json.value("id") != i) { fail("protocol"); return; }
                rtts.push_back(elapsedMs(clock));
            }
            r.pingCount = int(rtts.size());
            double mean = 0;
            for (double value : rtts) mean += value / rtts.size();
            for (double value : rtts) r.jitterMs += (value - mean) * (value - mean) / rtts.size();
            r.jitterMs = std::sqrt(r.jitterMs);
            std::sort(rtts.begin(), rtts.end());
            r.rttMinMs = rtts.front(); r.rttMedianMs = (rtts[5] + rtts[6]) * 0.5;
            r.rttP95Ms = rtts[std::size_t(std::ceil(rtts.size() * 0.95)) - 1];
            stage(DspProbeStage::Upload);
            if (!write({{"type", "probe_upload_begin"}}) || !read()) { fail("timeout"); return; }
            if (!payload.isEmpty() || json.value("type") != "probe_upload_ready") { fail("protocol"); return; }
            const QByteArray outgoing(ChunkBytes, char(0x5a));
            int sequence = 0;
            clock.restart();
            do {
                if (!write({{"type", "probe_upload_data"}, {"id", sequence++}}, outgoing)) { fail("timeout"); return; }
                r.uploadBytes += outgoing.size();
            } while (!stopping && clock.elapsed() < DurationMs && r.uploadBytes < MaxBytes);
            if (!write({{"type", "probe_upload_end"}, {"bytes", r.uploadBytes}}) || !read()) { fail("timeout"); return; }
            if (!payload.isEmpty() || json.value("type") != "probe_upload_done" || json.value("bytes") != r.uploadBytes) { fail("protocol"); return; }
            r.uploadMBps = r.uploadBytes / ((std::max)(0.001, elapsedMs(clock)) * 1000.0);
            stage(DspProbeStage::Download);
            clock.restart();
            if (!write({{"type", "probe_download"}})) { fail("timeout"); return; }
            const QByteArray pattern(ChunkBytes, char(0xa5));
            sequence = 0;
            for (;;) {
                if (clock.elapsed() > 8000 || !read()) { fail("timeout"); return; }
                if (json.value("type") == "probe_download_done") {
                    if (!payload.isEmpty() || r.downloadBytes <= 0 || json.value("bytes") != r.downloadBytes) { fail("protocol"); return; }
                    break;
                }
                if (json.value("type") != "probe_download_data" || json.value("id") != sequence++ ||
                    payload != pattern || r.downloadBytes > MaxBytes - payload.size()) { fail("protocol"); return; }
                r.downloadBytes += payload.size();
            }
            r.downloadMBps = r.downloadBytes / ((std::max)(0.001, elapsedMs(clock)) * 1000.0);
            if (stopping) { fail({}); return; }
            stage(DspProbeStage::Complete);
        } catch (const std::exception &) { fail("resources"); }
    }
};
DspConnectionProbe::DspConnectionProbe() : impl(std::make_unique<Impl>()) {}
DspConnectionProbe::~DspConnectionProbe() { cancel(); if (impl->thread.joinable()) impl->thread.join(); }
bool DspConnectionProbe::start(const DspStreamEndpoint &endpoint) {
    if (impl->active.exchange(true)) return false;
    if (impl->thread.joinable()) impl->thread.join();
    impl->stopping = false;
    DspConnectionProbeResult initial; initial.stage = DspProbeStage::Connecting; impl->publish(initial);
    try {
        impl->thread = std::thread([this, endpoint]() { impl->run(endpoint); impl->active = false; });
    } catch (...) {
        initial.stage = DspProbeStage::Failed; initial.errorCode = "resources"; impl->publish(initial); impl->active = false; return false;
    }
    return true;
}
void DspConnectionProbe::cancel() { impl->stopping = true; }
bool DspConnectionProbe::running() const { return impl->active.load(); }
DspConnectionProbeResult DspConnectionProbe::result() const {
    std::lock_guard<std::mutex> lock(impl->mutex); return impl->snapshot;
}

DspConnectionEstimate estimateDspConnection(const DspConnectionProbeResult &r, double rate, int length,
                                           int updateMs, int overlap, int budget) {
    DspConnectionEstimate e;
    if (!std::isfinite(rate) || rate <= 0 || length < 512 || updateMs < 1 || budget < 1) return e;
    const double hop = (std::max)({1.0, std::floor(double(length) * (100 - std::clamp(overlap, 0, 95)) / 100.0),
                                 std::ceil(rate * updateMs / 1000.0)});
    e.requestedFramesPerSecond = 1000.0 / updateMs;
    e.continuousFramesPerSecond = rate / hop;
    e.continuousSpectrumDownloadMBps = length * 4.0 * e.continuousFramesPerSecond / 1e6;
    e.continuousUploadMBps = rate * 8.0 / 1e6;
    e.windowUploadMBps = length * 8.0 * e.requestedFramesPerSecond / 1e6;
    e.spectrumDownloadMBps = length * 4.0 * e.requestedFramesPerSecond / 1e6;
    e.windowsSnapshotFits = std::size_t(length) <= IqBuffer::MaxSnapshotComplexSamples;
    if (r.stage != DspProbeStage::Complete || r.uploadMBps <= 0 || r.downloadMBps <= 0) return e;
    const double up = (std::min)(r.uploadMBps * 0.75, double(budget));
    const double down = r.downloadMBps * 0.75;
    e.memoryFits = length <= r.mainFftMaxLength;
    // IQ chunks use stop-and-wait ACKs; returned spectrum chunks are streamed.
    const double iqChunks = std::ceil(double(length) / SpectrumComputeProtocol::ChunkSamples);
    e.transportMsPerWindow = length * 8.0 / (up * 1000) + length * 4.0 / (down * 1000) +
                             (iqChunks + 2) * r.rttMedianMs;
    e.networkFramesPerSecond = 1000.0 / e.transportMsPerWindow;
    e.windowsFit = e.memoryFits && e.windowsSnapshotFits && e.windowUploadMBps <= up && e.spectrumDownloadMBps <= down &&
                   e.requestedFramesPerSecond <= e.networkFramesPerSecond;
    const double continuousTransportMs = hop * 8.0 / (up * 1000) + length * 4.0 / (down * 1000) +
        (std::ceil(hop / SpectrumComputeProtocol::ChunkSamples) + 1) * r.rttMedianMs;
    e.continuousFits = e.memoryFits && e.continuousUploadMBps <= up && e.continuousSpectrumDownloadMBps <= down &&
                      continuousTransportMs * e.continuousFramesPerSecond <= 1000;
    return e;
}

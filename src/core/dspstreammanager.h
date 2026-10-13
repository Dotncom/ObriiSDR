#pragma once
#include "dspstreamprocessor.h"

struct DspStreamEndpoint {
    bool enabled = false;
    QString address = QStringLiteral("127.0.0.1");
    quint16 port = 7358;
    QString token;
    bool operator==(const DspStreamEndpoint &other) const {
        return enabled == other.enabled && address == other.address && port == other.port && token == other.token;
    }
};

// Thread-confined router; the local path does not serialize or use a socket.
class DspStreamManager final {
public:
    DspStreamManager();
    ~DspStreamManager();
    void configure(const DspStreamEndpoint &endpoint);
    void clear();
    DspStreamResult process(const DspStreamBlock &block, bool external);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

// Explicitly started, bounded compute service; never owns receiver hardware.
class DspComputeServer final {
public:
    DspComputeServer();
    ~DspComputeServer();
    bool start(const QString &address, quint16 port, const QString &token, int mainFftMemoryMiB = 512);
    void requestStop();
    void stop();
    bool running() const;
    quint16 port() const;
    QString status() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

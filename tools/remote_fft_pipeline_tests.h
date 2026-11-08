// Included inside spectrum_compute_selftest.cpp's test namespace.
struct PipelinePeer {
    QTcpServer server;
    QTcpSocket *socket = nullptr;
    DspStreamEndpoint endpoint;
    SpectrumComputeSession session{64};
    QByteArray buffer;
    QString error;
    std::vector<Packet> held;
    bool advertised = true, hold = true, runSeen = false, runWithHeldAcks = false;
    int capability = 4, confirmation = 4, chunks = 0, requestedCredit = 0;

    explicit PipelinePeer(bool extension = true) : advertised(extension) {
        require(server.listen(QHostAddress::LocalHost, 0), "pipeline peer bind failed");
        endpoint.enabled = true;
        endpoint.address = QStringLiteral("127.0.0.1");
        endpoint.port = server.serverPort();
        endpoint.token = QString(64, 'p');
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this] {
            auto *next = server.nextPendingConnection();
            if (socket) { next->abort(); next->deleteLater(); return; }
            socket = next;
            socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
            socket->setReadBufferSize(16 + DspStreamProtocol::MaxJsonBytes + ChunkSamples * 8);
            QObject::connect(socket, &QTcpSocket::readyRead, &server, [this] { receive(); });
            receive();
        });
    }
    void send(const Packet &packet) {
        const auto bytes = DspStreamProtocol::encode(packet.header, packet.payload);
        require(socket->write(bytes) == bytes.size(), "pipeline peer write failed");
    }
    void release(bool all = true, const char *corrupt = nullptr) {
        require(!held.empty(), "no ACK held at barrier");
        const QString fault = QString::fromLatin1(corrupt ? corrupt : "");
        if (fault == "next") {
            quint64 next = 0;
            require(SpectrumComputeProtocol::readCounter(held.front().header.value("next"), next), "invalid test ACK");
            held.front().header["next"] = QString::number(next + 1);
        } else if (fault == "filled") {
            held.front().header["filled"] = held.front().header.value("filled").toInt() - 1;
        } else if (fault == "job") {
            held.front().header["job"] = held.front().header.value("job").toString() + "-stale";
        }
        const std::size_t count = all ? held.size() : 1;
        for (std::size_t i = 0; i < count; ++i) send(held[i]);
        if (fault == "duplicate") send(held.front());
        held.erase(held.begin(), held.begin() + std::ptrdiff_t(count));
    }
    void receive() {
        const int limit = 16 + DspStreamProtocol::MaxJsonBytes + ChunkSamples * 8;
        do {
            buffer.append(socket->read(limit - buffer.size()));
            while (error.isEmpty()) {
                QJsonObject h;
                QByteArray p;
                const int parsed = DspStreamProtocol::extract(buffer, h, p, error);
                if (parsed <= 0) break;
                const auto type = h.value("type").toString();
                if (type == "hello") {
                    QJsonObject hello{{"type", "hello"}, {"version", 1}, {"mainFftVersion", 1},
                                      {"mainFftMaxLength", 1048576}};
                    if (advertised) hello["mainFftPipelineChunks"] = capability;
                    send({hello, {}});
                    continue;
                }
                if (type == "main_fft_begin") requestedCredit = h.value("pipelineChunks").toInt();
                if (type == "main_fft_iq") ++chunks;
                if (type == "main_fft_run") {
                    runSeen = true;
                    runWithHeldAcks = !held.empty();
                    if (!held.empty()) release();
                }
                if (!session.handle(h, p, [this](QJsonObject header, const QByteArray &payload) {
                    if (header.value("type") == "main_fft_ready" && advertised)
                        header["pipelineChunks"] = confirmation;
                    if (header.value("type") == "main_fft_ack" && hold) {
                        if (held.size() == SpectrumComputeProtocol::MaxPipelineChunks) {
                            error = QStringLiteral("client exceeded four pending chunks");
                            return false;
                        }
                        held.push_back({header, payload});
                    } else send({header, payload});
                    return true;
                })) { error = QStringLiteral("session rejected pipeline command"); return; }
            }
        } while (error.isEmpty() && socket->bytesAvailable());
    }
};

void pipelineSessionTests() {
    for (int credit : {1, 2, 4}) {
        Session session;
        auto request = session.beginRequest(radioSettings());
        request["pipelineChunks"] = credit;
        require(session.call(request) && session.replies.front().header.value("pipelineChunks").toInt() == credit,
                "session failed pipeline negotiation");
    }
    for (const QJsonValue &credit : {QJsonValue(0), QJsonValue(5), QJsonValue(1.5),
                                   QJsonValue("4"), QJsonValue(true), QJsonValue(QJsonValue::Null)}) {
        Session session;
        auto request = session.beginRequest(radioSettings());
        request["pipelineChunks"] = credit;
        session.reject(request);
    }
    Session legacy;
    legacy.begin(radioSettings());
    require(!legacy.replies.front().header.contains("pipelineChunks"), "legacy reply changed");
    RemoteFftRetryPolicy retry;
    for (int expected : {1000, 2000, 4000, 8000, 8000, 8000})
        require(retry.failed(true) == expected, "transient retry bound changed");
    require(retry.failed(false) == 30000, "permanent refusal retry bound changed");
    retry.reset();
    require(retry.failed(true) == 1000, "successful/configured retry reset failed");
    std::cout << "Pipeline negotiation and deterministic retry policy passed\n";
}

void pipelineWorkerTests() {
    quint64 epoch = 3001;
    auto options = remoteOptions();
    options.uploadLimitMBps = 2000;
    const auto settings = radioSettings(ChunkSamples * 6);
    const auto iq = makeIq(settings.fftLength);
    for (bool legacy : {false, true}) {
        PipelinePeer peer(!legacy);
        RemoteSpectrumWorker worker;
        const auto published = publishIq(settings, iq, epoch++);
        const auto renew = [&] { requestRemote(worker, settings, peer.endpoint, options); };
        renew();
        const int bound = legacy ? 1 : 4;
        require(waitUntil([&] { return peer.chunks == bound || !peer.error.isEmpty(); }, 3000, renew)
                    && peer.error.isEmpty(), "pipeline did not reach its credit barrier");
        require(!waitUntil([&] { return peer.chunks > bound; }, 100), "client exceeded negotiated credit");
        require(peer.requestedCredit == (legacy ? 0 : 4), "legacy/extended request was not explicit");
        if (!legacy) {
            peer.release(false);
            require(waitUntil([&] { return peer.chunks == 5; }, 2000, renew), "one ACK did not release one credit");
            require(!waitUntil([&] { return peer.chunks > 5; }, 100), "one ACK released extra credit");
            peer.release(false);
        } else {
            peer.hold = false;
            peer.release();
        }
        checkFrame(awaitRemoteFrame(worker, settings, peer.endpoint, options, "pipelined/legacy FFT failed"),
                   settings, iq, published);
        require(peer.error.isEmpty() && peer.runSeen && peer.runWithHeldAcks == !legacy,
                "run/ACK-tail ordering differs from negotiated transport");
        checkQueuedIq(iq);
    }
    for (const char *field : {"next", "filled", "job", "duplicate"}) {
        PipelinePeer peer;
        RemoteSpectrumWorker worker;
        const auto published = publishIq(settings, iq, epoch++);
        const auto renew = [&] { requestRemote(worker, settings, peer.endpoint, options); };
        renew();
        require(waitUntil([&] { return peer.chunks == 4; }, 3000, renew), "bad ACK barrier not reached");
        peer.release(false, field);
        const auto frame = awaitRemoteFrame(worker, settings, peer.endpoint, options, "bad ACK not rejected");
        require(!frame.valid && !frame.error.isEmpty() && !peer.runSeen, "corrupt ACK accepted");
        checkAudioQueue(published);
        checkQueuedIq(iq);
    }
    for (bool badCapability : {false, true}) {
        PipelinePeer peer;
        if (badCapability) peer.capability = 5;
        else peer.confirmation = 1;
        RemoteSpectrumWorker worker;
        publishIq(settings, iq, epoch++);
        requestRemote(worker, settings, peer.endpoint, options);
        const auto frame = awaitRemoteFrame(worker, settings, peer.endpoint, options, "bad negotiation not rejected");
        require(!frame.valid && peer.chunks == 0, "unconfirmed pipeline uploaded IQ");
        checkQueuedIq(iq);
    }
    for (bool retune : {false, true}) {
        PipelinePeer peer;
        RemoteSpectrumWorker worker;
        publishIq(settings, iq, epoch++);
        const auto renew = [&] { requestRemote(worker, settings, peer.endpoint, options); };
        renew();
        require(waitUntil([&] { return peer.chunks == 4; }, 3000, renew), "cancel credit barrier not reached");
        if (retune) {
            LoopbackServer replacement;
            const auto published = publishIq(settings, iq, epoch++);
            requestRemote(worker, settings, replacement.endpoint, options);
            checkFrame(awaitRemoteFrame(worker, settings, replacement.endpoint, options, "in-flight pipeline retune failed"),
                       settings, iq, published);
        } else {
            worker.cancel();
            require(waitUntil([&] { return peer.socket->state() == QAbstractSocket::UnconnectedState; }, 1500),
                    "cancel did not close a full upload window");
        }
        expectNoFrame(worker, "stale pipelined frame escaped cancel/retune");
        checkQueuedIq(iq);
    }
    std::cout << "Pipeline/legacy parity, four-credit barrier, ACK validation, negotiation, cancel and retune passed\n";
}

void fallbackRetryTests() {
    QTcpServer busy;
    require(busy.listen(QHostAddress::LocalHost, 0), "busy peer bind failed");
    int attempts = 0;
    QObject::connect(&busy, &QTcpServer::newConnection, &busy, [&] {
        auto *socket = busy.nextPendingConnection();
        ++attempts;
        socket->write(DspStreamProtocol::encode({{"type", "error"}, {"code", "busy"}}));
        socket->disconnectFromHost();
        QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    });
    DspStreamEndpoint endpoint;
    endpoint.enabled = true;
    endpoint.address = QStringLiteral("127.0.0.1");
    endpoint.port = busy.serverPort();
    endpoint.token = QString(64, 'r');
    RemoteSpectrumWorker worker;
    auto options = remoteOptions();
    options.fallbackToLocal = true;
    const auto settings = radioSettings();
    const auto iq = makeIq(settings.fftLength);
    const auto published = publishIq(settings, iq, 4001);
    const auto renew = [&] { requestRemote(worker, settings, endpoint, options); };
    renew();
    checkFrame(awaitFrame(worker, "retry fallback failed", renew), settings, iq, published);
    require(waitUntil([&] { return attempts == 2 && worker.status().contains("2000 ms"); }, 3000, renew),
            "retry did not back off after second failure");
    expectNoFrame(worker, "retry recomputed unchanged fallback IQ");
    checkQueuedIq(iq);
    const auto fresh = makeIq(settings.fftLength, 92);
    const auto next = publishIq(settings, fresh, 4001, false);
    checkFrame(awaitFrame(worker, "warm fallback did not accept fresh IQ", renew), settings, fresh, next);
    require(worker.status().contains("LOCAL FALLBACK"), "retry fallback lost its label");
    checkQueuedIq(fresh);
    worker.cancel();
    expectNoFrame(worker, "cancelled fallback published a frame");
    std::cout << "Persistent fallback, duplicate suppression and retry backoff integration passed\n";
}

void pipelineFailureTests() {
    auto options = remoteOptions(SpectrumComputeMode::Continuous);
    options.uploadLimitMBps = 2000;
    const auto settings = radioSettings(ChunkSamples * 6);
    const auto iq = makeIq(settings.fftLength);
    quint64 epoch = 4501;
    for (int action : {0, 1, 2, 3}) {
        PipelinePeer peer;
        RemoteSpectrumWorker worker;
        const auto published = publishIq(settings, iq, epoch++);
        const auto renew = [&] { requestRemote(worker, settings, peer.endpoint, options); };
        renew();
        require(waitUntil([&] { return peer.chunks == 4; }, 3000, renew), "failure credit barrier not reached");
        if (action == 0 || action == 1) {
            if (action == 0) peer.socket->abort();
            const auto frame = awaitFrame(worker, "disconnect/ACK timeout not reported", renew);
            require(!frame.valid && !frame.error.isEmpty(), "failed upload returned FFT data");
            require(peer.chunks == 4, "failed upload exceeded credit bound");
            checkAudioQueue(published);
        } else if (action == 2) {
            require(waitUntil([&] { return peer.socket->state() == QAbstractSocket::UnconnectedState; }, 3000),
                    "expired lease left a full upload window connected");
            expectNoFrame(worker, "lease-expired upload published a frame");
        } else {
            // Same request generation, but a new source epoch invalidates all ACKs.
            const auto fresh = publishIq(settings, iq, epoch++);
            require(waitUntil([&] { return peer.socket->state() == QAbstractSocket::UnconnectedState; }, 1500),
                    "epoch change did not close the old upload window");
            SpectrumFftFrame frame;
            if (worker.takeLatest(frame)) require(!frame.valid, "old epoch FFT escaped its barrier");
            checkAudioQueue(fresh);
        }
        checkQueuedIq(iq);
    }
    std::cout << "Full-window disconnect, ACK timeout, lease expiry and source epoch barriers passed\n";
}

void fallbackRecoveryTests() {
    LoopbackServer loopback;
    loopback.server.stop();
    RemoteSpectrumWorker worker;
    auto options = remoteOptions();
    options.fallbackToLocal = true;
    const auto settings = radioSettings();
    const auto iq = makeIq(settings.fftLength);
    auto published = publishIq(settings, iq, 4701);
    const auto renew = [&] { requestRemote(worker, settings, loopback.endpoint, options); };
    renew();
    checkFrame(awaitFrame(worker, "recovery initial fallback failed", renew), settings, iq, published);
    require(worker.status().contains("LOCAL FALLBACK"), "initial failure did not enter fallback");
    checkQueuedIq(iq);
    published = IqBuffer::stats();
    require(loopback.server.start(loopback.endpoint.address, loopback.endpoint.port, loopback.endpoint.token, 64),
            "recovery server restart failed");
    require(waitUntil([&] { return loopback.server.port() != 0; }), "recovery listener did not start");
    checkFrame(awaitFrame(worker, "remote retry failed to recover", renew), settings, iq, published);
    loopback.server.requestStop();
    require(waitUntil([&] { return !loopback.server.running(); }), "native server stop did not finish");
    const auto fresh = makeIq(settings.fftLength, 113);
    published = publishIq(settings, fresh, 4701, false);
    checkFrame(awaitFrame(worker, "fallback after server stop failed", renew), settings, fresh, published);
    require(waitUntil([&] { return worker.status().contains("1000 ms"); }, 1000, renew),
            "successful remote result did not reset retry backoff");
    checkQueuedIq(fresh);
    worker.cancel();
    expectNoFrame(worker, "recovered worker published after cancel");
    std::cout << "Fallback -> native remote recovery -> server stop fallback passed\n";
}

void managerMetadataCoexistenceTest() {
    // Use the processor's codec unchanged; the manager must return its decoded
    // output domain even when it differs from the uploaded IQ clock/count.
    std::promise<quint16> port;
    auto ready = port.get_future();
    auto peer = std::async(std::launch::async, [&] {
        QTcpServer server;
        if (!server.listen(QHostAddress::LocalHost, 0)) { port.set_value(0); return false; }
        port.set_value(server.serverPort());
        if (!server.waitForNewConnection(5000)) return false;
        auto *socket = server.nextPendingConnection();
        QByteArray buffer, payload;
        QJsonObject header;
        if (!DspStreamTransport::receive(*socket, buffer, header, payload, 3000) || header.value("type") != "hello" ||
            !DspStreamTransport::send(*socket, DspStreamProtocol::encode({{"type", "hello"}, {"version", 1}}))) return false;
        quint64 origin = 0;
        for (int i = 0; i < 2; ++i) {
            if (!DspStreamTransport::receive(*socket, buffer, header, payload, 3000)) return false;
            DspStreamBlock block;
            QString error;
            if (!DspStreamProtocol::decodeRequest(header, payload, block, error)) return false;
            if (!i) origin = block.firstSample;
            DspStreamResult result;
            result.sampleRate = block.sampleRate / 2;
            result.originFirstSample = origin;
            result.firstSample = i ? 2016 : 0;
            result.sampleCount = i ? 2048 : 2016;
            result.rateNumerator = 1;
            result.rateDenominator = 2;
            result.inputReserve = 64;
            result.gap = !i;
            result.iq = std::make_shared<const std::vector<float>>(result.sampleCount * 2, 0.25f);
            if (!DspStreamTransport::send(*socket, DspStreamProtocol::response(block, result))) return false;
        }
        return true;
    });
    DspStreamEndpoint endpoint;
    endpoint.enabled = true;
    endpoint.address = QStringLiteral("127.0.0.1");
    endpoint.port = ready.get();
    endpoint.token = QString(64, 'm');
    require(endpoint.port != 0, "metadata peer bind failed");
    DspStreamManager manager;
    manager.configure(endpoint);
    auto block = ddcTone();
    block.ffts.clear();
    const quint64 origin = (quint64(1) << 60) + 3;
    block.firstSample = origin;
    for (int i = 0; i < 2; ++i) {
        block.gap = !i;
        const auto result = manager.process(block, true);
        require(result.error.isEmpty() && result.sampleRate == 48000 && result.originFirstSample == origin &&
                    result.firstSample == (i ? 2016U : 0U) && result.sampleCount == (i ? 2048U : 2016U) &&
                    result.rateNumerator == 1 && result.rateDenominator == 2 && result.inputReserve == 64 &&
                    result.gap == !i && result.iq && result.iq->size() == result.sampleCount * 2,
                "manager overwrote rate-changing processor metadata");
        block.firstSample += 4096;
    }
    require(peer.get(), "metadata peer failed");
    std::cout << "Manager preserves rational output rate/count/origin/reserve/gap on consecutive replies passed\n";
}

#include "clidspgraph.h"

#include "dspbranchaudiooutput.h"
#include "iqbuffer.h"

#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>

#include <algorithm>
#include <cmath>

namespace {
QJsonObject errorReply(const QString &message) {
    return {{QStringLiteral("ok"), false}, {QStringLiteral("error"), message}};
}
RadioSettings sourceSettings(double centerHz, double sampleRateHz) {
    RadioSettings settings;
    settings.inputMode = INPUT_RF;
    settings.centerFrequency = centerHz;
    settings.sampleRate = sampleRateHz;
    settings.listeningFrequency = centerHz;
    settings.simplifiedAudioChannelizer = false;
    return settings;
}
QString parseEndpoint(const QJsonObject &request, bool listener, DspStreamEndpoint &endpoint) {
    QHostAddress address;
    const auto port = request.value(QStringLiteral("port"));
    const auto token = request.value(QStringLiteral("token"));
    if (!request.value(QStringLiteral("address")).isString() ||
        !address.setAddress(request.value(QStringLiteral("address")).toString()) ||
        address.isNull() || address.isMulticast() || address == QHostAddress::Broadcast ||
        (!listener && (address == QHostAddress::Any || address == QHostAddress::AnyIPv4 ||
                       address == QHostAddress::AnyIPv6)))
        return QStringLiteral("An explicit numeric unicast IP address is required (wildcard binds are allowed only for compute)");
    if (!port.isDouble() || !std::isfinite(port.toDouble()) ||
        std::floor(port.toDouble()) != port.toDouble() ||
        port.toDouble() < (listener ? 0 : 1) || port.toDouble() > 65535)
        return QStringLiteral("An explicit integer TCP port is required (1..65535; compute may use 0)");
    if (!token.isString() || token.toString().size() < 32 || token.toString().size() > 128)
        return QStringLiteral("An explicit session token of 32..128 printable non-space ASCII characters is required");
    for (QChar c : token.toString())
        if (c.unicode() < 33 || c.unicode() > 126)
            return QStringLiteral("Session token must contain only printable non-space ASCII characters");
    endpoint.enabled = true;
    endpoint.address = address.toString();
    endpoint.port = static_cast<quint16>(port.toInt());
    endpoint.token = token.toString();
    return {};
}
}

CliDspGraph::~CliDspGraph() { stop(); }

QJsonObject CliDspGraph::status() const {
    quint64 audioDrops = 0;
    std::size_t audioBytes = 0;
    {
        std::lock_guard<std::mutex> lock(audioMutex);
        audioDrops = droppedAudioFrames;
        audioBytes = queuedAudioBytes;
    }
    QJsonArray branches;
    for (const DspBranchConfig &branch : plan.branches) {
        QJsonObject item{{QStringLiteral("id"), branch.id},
                         {QStringLiteral("source"), branch.source.toJson()},
                         {QStringLiteral("enabled"), branch.enabled},
                         {QStringLiteral("externalProcessing"), branch.external},
                         {QStringLiteral("frequencyHz"), branch.frequencyHz},
                         {QStringLiteral("bandwidthHz"), branch.bandwidthHz},
                         {QStringLiteral("decimation"), branch.decimation},
                         {QStringLiteral("pfb"), branch.pfb},
                         {QStringLiteral("fftCount"), branch.ffts.size()},
                         {QStringLiteral("audioEnabled"), branch.audioEnabled}};
        if (recordingDirectories.contains(branch.id))
            item.insert(QStringLiteral("recordingDirectory"), recordingDirectories.value(branch.id));
        const auto port = ports.find(branch.audioOutputId);
        if (port != ports.end()) {
            item.insert(QStringLiteral("pcmHost"), QStringLiteral("127.0.0.1"));
            item.insert(QStringLiteral("pcmPort"), static_cast<int>(port->second.server->serverPort()));
            item.insert(QStringLiteral("pcmFormat"), QStringLiteral("s16le-mono-48000"));
            item.insert(QStringLiteral("pcmClients"), static_cast<int>(port->second.clients.size()));
            item.insert(QStringLiteral("pcmFrames"), static_cast<double>(port->second.frames));
            item.insert(QStringLiteral("pcmDrops"), static_cast<double>(port->second.drops));
        }
        const auto result = frames.find(branch.id);
        if (result != frames.end()) {
            item.insert(QStringLiteral("sequence"), static_cast<double>(result->second.sequence));
            item.insert(QStringLiteral("processingMs"), result->second.processingMs);
            item.insert(QStringLiteral("gaps"), static_cast<double>(result->second.gaps));
            item.insert(QStringLiteral("error"), result->second.error);
            item.insert(QStringLiteral("audioStatus"), result->second.audioStatus);
            item.insert(QStringLiteral("recordingStatus"), result->second.recordingStatus);
        }
        branches.append(item);
    }
    const auto execution = engine ? engine->executionStats() : DspBranchExecutionStats{};
    const QJsonObject remote{{QStringLiteral("enabled"), remoteEndpoint.enabled},
                             {QStringLiteral("address"), remoteEndpoint.address},
                             {QStringLiteral("port"), int(remoteEndpoint.port)},
                             {QStringLiteral("tokenConfigured"), !remoteEndpoint.token.isEmpty()}};
    const QJsonObject compute{{QStringLiteral("running"), computeServer && computeServer->running()},
                              {QStringLiteral("address"), computeAddress},
                              {QStringLiteral("requestedPort"), int(computeRequestedPort)},
                              {QStringLiteral("port"), computeServer ? int(computeServer->port()) : 0},
                              {QStringLiteral("status"), computeServer ? computeServer->status() : QStringLiteral("Stopped")}};
    return {{QStringLiteral("loaded"), !path.isEmpty()},
            {QStringLiteral("running"), active},
            {QStringLiteral("path"), path},
            {QStringLiteral("modified"), modified},
            {QStringLiteral("branches"), branches},
            {QStringLiteral("remote"), remote},
            {QStringLiteral("compute"), compute},
            {QStringLiteral("remoteExecution"), QJsonObject{
                {QStringLiteral("workerStarted"), execution.remoteWorkerStarted},
                {QStringLiteral("pendingBlocks"), static_cast<double>(execution.pendingRemoteBlocks)},
                {QStringLiteral("pendingBytes"), static_cast<double>(execution.pendingRemoteBytes)},
                {QStringLiteral("droppedBlocks"), static_cast<double>(execution.droppedRemoteBlocks)}}},
            {QStringLiteral("droppedAudioFrames"), static_cast<double>(audioDrops)},
            {QStringLiteral("queuedAudioBytes"), static_cast<double>(audioBytes)}};
}

QJsonObject CliDspGraph::remoteCommand(const QJsonObject &request) {
    const QString mode = request.value(QStringLiteral("remoteOperation")).toString();
    if (mode == QStringLiteral("status"))
        return {{QStringLiteral("ok"), true}, {QStringLiteral("graph"), status()}};
    DspStreamEndpoint endpoint;
    if (mode == QStringLiteral("configure")) {
        const QString error = parseEndpoint(request, false, endpoint);
        if (!error.isEmpty()) return errorReply(error);
    } else if (mode != QStringLiteral("disable")) {
        return errorReply(QStringLiteral("Unknown graph remote operation"));
    }
    if (!(remoteEndpoint == endpoint)) {
        remoteEndpoint = endpoint;
        if (engine) engine->setExternalEndpoint(remoteEndpoint);
        for (const auto &branch : plan.branches) if (branch.external) {
            frames.erase(branch.id);
            for (const auto &fft : branch.ffts) frames.erase(fft.id);
        }
    }
    return {{QStringLiteral("ok"), true}, {QStringLiteral("graph"), status()}};
}

QJsonObject CliDspGraph::computeCommand(const QJsonObject &request) {
    const QString mode = request.value(QStringLiteral("computeOperation")).toString();
    if (mode == QStringLiteral("stop")) {
        if (computeServer) computeServer->requestStop();
    } else if (mode == QStringLiteral("start")) {
        DspStreamEndpoint endpoint;
        const QString error = parseEndpoint(request, true, endpoint);
        if (!error.isEmpty()) return errorReply(error);
        if (computeServer && computeServer->running())
            return errorReply(QStringLiteral("Compute service is already running or stopping"));
        computeServer = std::make_unique<DspComputeServer>();
        if (!computeServer->start(endpoint.address, endpoint.port, endpoint.token))
            return errorReply(QStringLiteral("Cannot start compute service"));
        computeAddress = endpoint.address;
        computeRequestedPort = endpoint.port;
    } else if (mode != QStringLiteral("status")) {
        return errorReply(QStringLiteral("Unknown graph compute operation"));
    }
    // Start/stop are asynchronous; status/port, not the command acknowledgement, report readiness.
    return {{QStringLiteral("ok"), true}, {QStringLiteral("graph"), status()}};
}

QJsonObject CliDspGraph::trace(const QString &id) const {
    const auto found = frames.find(id);
    if (found == frames.end() || found->second.levels.empty())
        return errorReply(QStringLiteral("No branch FFT frame for this ID"));
    const DspBranchFrame &frame = found->second;
    QJsonArray levels;
    for (float level : frame.levels) levels.append(level);
    return {{QStringLiteral("ok"), true}, {QStringLiteral("id"), id},
            {QStringLiteral("centerHz"), frame.centerHz},
            {QStringLiteral("sampleRateHz"), frame.sampleRateHz},
            {QStringLiteral("binWidthHz"), frame.binWidthHz},
            {QStringLiteral("rbwHz"), frame.rbwHz},
            {QStringLiteral("levelsDbfs"), levels}};
}

void CliDspGraph::enqueueAudio(const DspBranchPcmFrame &frame) {
    std::lock_guard<std::mutex> lock(audioMutex);
    if (frame.pcm.isEmpty() && !frame.reset) return;
    if (frame.reset) {
        for (auto it = audioQueue.begin(); it != audioQueue.end();) {
            if (it->sinkId == frame.sinkId) {
                queuedAudioBytes -= static_cast<std::size_t>(it->pcm.size());
                it = audioQueue.erase(it);
            } else ++it;
        }
        audioQueue.push_back({frame.sinkId, {}, true});
    }
    if (frame.pcm.isEmpty()) return;
    if (queuedAudioBytes + static_cast<std::size_t>(frame.pcm.size()) > 2 * 1024 * 1024) {
        ++droppedAudioFrames;
        return;
    }
    queuedAudioBytes += static_cast<std::size_t>(frame.pcm.size());
    audioQueue.push_back({frame.sinkId, frame.pcm, false});
}

void CliDspGraph::drainAudio() {
    std::deque<AudioItem> items;
    {
        std::lock_guard<std::mutex> lock(audioMutex);
        items.swap(audioQueue);
        queuedAudioBytes = 0;
    }
    for (const AudioItem &item : items) {
        auto found = ports.find(item.sinkId);
        if (found == ports.end()) continue;
        PcmPort &port = found->second;
        if (item.reset) {
            const auto clients = port.clients;
            for (QTcpSocket *client : clients) client->abort();
            continue;
        }
        ++port.frames;
        for (QTcpSocket *client : port.clients) {
            if (client->state() != QAbstractSocket::ConnectedState) continue;
            if (client->bytesToWrite() + item.pcm.size() > 256 * 1024 ||
                client->write(item.pcm) != item.pcm.size()) ++port.drops;
        }
    }
}

void CliDspGraph::poll() {
    if (!active || !engine) return;
    drainAudio();
    for (DspBranchFrame &frame : engine->takeFrames())
        frames[frame.id] = std::move(frame);
}

void CliDspGraph::stop() {
    active = false;
    if (engine) {
        engine->setAudioConsumer({});
        engine.reset();
    }
    ports.clear();
    frames.clear();
    recordingDirectories.clear();
    {
        std::lock_guard<std::mutex> lock(audioMutex);
        audioQueue.clear();
        queuedAudioBytes = 0;
    }
    IqBuffer::clear();
}

void CliDspGraph::updateSource(double center, double rate) {
    if (!active || !engine || (std::abs(center - centerHz) < 0.5 &&
                               std::abs(rate - sampleRateHz) < 0.5)) return;
    centerHz = center;
    sampleRateHz = rate;
    IqBuffer::clear();
    IqBuffer::setSampleRateEstimate(rate);
    engine->configure(plan, sourceSettings(center, rate), true);
    frames.clear();
}

void CliDspGraph::publishIq(const float *iq, std::size_t samples) {
    if (!active || !iq || samples == 0) return;
    IqBuffer::publish(iq, samples * 2, false, true);
}

QString CliDspGraph::validate(const QJsonObject &candidate, const DspGraphPlan &compiled,
                              double center, double rate) const {
    QHash<QString, QString> blockTypes;
    QHash<QString, QString> upstream;
    for (const QJsonValue &value : candidate.value(QStringLiteral("blocks")).toArray()) {
        const QJsonObject block = value.toObject();
        blockTypes.insert(block.value(QStringLiteral("id")).toString(),
                          block.value(QStringLiteral("type")).toString());
    }
    for (const QJsonValue &value : candidate.value(QStringLiteral("connections")).toArray()) {
        const QJsonObject edge = value.toObject();
        upstream.insert(edge.value(QStringLiteral("target")).toString(),
                        edge.value(QStringLiteral("source")).toString());
    }
    bool enabledBranch = false;
    for (const DspBranchConfig &branch : compiled.branches) {
        if (!branch.enabled) continue;
        enabledBranch = true;
        if (branch.external && !remoteEndpoint.enabled)
            return QStringLiteral("Configure an explicit CLI remote DSP endpoint before starting external branches");
        double sourceCenter = center;
        double sourceRate = rate;
        if (!branch.source.isPrimary() &&
            (!auxiliarySourceLookup ||
             !auxiliarySourceLookup(branch.source, &sourceCenter, &sourceRate)))
            return QStringLiteral("CLI graph branch %1 has no configured Multi-SDR IQ source").arg(branch.id);
        QString input = upstream.value(branch.id);
        if (blockTypes.value(input) == QStringLiteral("pfb_channelizer")) input = upstream.value(input);
        const QString sourceType = blockTypes.value(input);
        if (sourceType != QStringLiteral("receiver_source") && sourceType != QStringLiteral("iq_source"))
            return QStringLiteral("CLI graph branch is not connected to local receiver IQ");
        const double frequency = branch.frequencyHz > 0.0 ? branch.frequencyHz : sourceCenter;
        if (!std::isfinite(sourceCenter) || !std::isfinite(sourceRate) || sourceRate <= 0.0 ||
            std::abs(frequency - sourceCenter) + branch.bandwidthHz * 0.5 > sourceRate * 0.5 ||
            branch.bandwidthHz * 0.5 >= branch.outputRateHz * 0.45)
            return QStringLiteral("CLI graph branch %1 has an invalid IQ span or output rate").arg(branch.id);
    }
    if (!enabledBranch)
        return QStringLiteral("CLI graph requires an enabled DDC branch; main FFT and descriptive routes are not executed here");
    return {};
}

QJsonObject CliDspGraph::command(const QJsonObject &request, double center, double rate,
                                 bool receiverRunning) {
    const QString operation = request.value(QStringLiteral("operation")).toString();
    if (operation == QStringLiteral("remote")) return remoteCommand(request);
    if (operation == QStringLiteral("compute")) return computeCommand(request);
    if (operation == QStringLiteral("status"))
        return {{QStringLiteral("ok"), true}, {QStringLiteral("graph"), status()}};
    if (operation == QStringLiteral("blocks")) {
        if (path.isEmpty()) return errorReply(QStringLiteral("Load a DSP graph JSON first"));
        return {{QStringLiteral("ok"), true}, {QStringLiteral("blocks"), definition.value(QStringLiteral("blocks"))},
                {QStringLiteral("connections"), definition.value(QStringLiteral("connections"))}};
    }
    if (operation == QStringLiteral("save")) {
        if (path.isEmpty()) return errorReply(QStringLiteral("Load a DSP graph JSON first"));
        const QString target = request.value(QStringLiteral("path")).toString();
        QFile file(target);
        if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
            return errorReply(QStringLiteral("Cannot create new DSP graph JSON: ") + file.errorString());
        const QByteArray bytes = QJsonDocument(definition).toJson(QJsonDocument::Indented);
        if (file.write(bytes) != bytes.size() || !file.flush()) {
            const QString message = file.errorString();
            file.close();
            file.remove();
            return errorReply(QStringLiteral("Cannot save DSP graph JSON: ") + message);
        }
        file.close();
        path = target;
        modified = false;
        return {{QStringLiteral("ok"), true}, {QStringLiteral("graph"), status()}};
    }
    if (operation == QStringLiteral("trace")) return trace(request.value(QStringLiteral("id")).toString());
    if (operation == QStringLiteral("record")) {
        if (!active || !engine) return errorReply(QStringLiteral("Start the DSP graph before recording"));
        const QString mode = request.value(QStringLiteral("recordOperation")).toString();
        if (mode == QStringLiteral("status")) return {{QStringLiteral("ok"), true}, {QStringLiteral("graph"), status()}};
        const QString id = request.value(QStringLiteral("id")).toString();
        const auto branch = std::find_if(plan.branches.cbegin(), plan.branches.cend(),
            [&](const DspBranchConfig &candidate) { return candidate.id == id && candidate.enabled; });
        if (branch == plan.branches.cend()) return errorReply(QStringLiteral("No enabled DDC branch with this ID"));
        if (mode == QStringLiteral("stop")) {
            if (!recordingDirectories.contains(id)) return errorReply(QStringLiteral("This branch is not recording"));
            engine->setRecording(id, {});
            recordingDirectories.remove(id);
        } else if (mode == QStringLiteral("start")) {
            if (recordingDirectories.contains(id)) return errorReply(QStringLiteral("This branch is already recording"));
            const QString directory = request.value(QStringLiteral("directory")).toString();
            if (!QFileInfo(directory).isDir() || !QFileInfo(directory).isAbsolute())
                return errorReply(QStringLiteral("Recording requires an existing absolute directory"));
            recordingDirectories.insert(id, directory);
            engine->setRecording(id, directory);
        } else return errorReply(QStringLiteral("Unknown graph record operation"));
        frames.clear();
        return {{QStringLiteral("ok"), true}, {QStringLiteral("graph"), status()}};
    }
    if (operation == QStringLiteral("set")) {
        if (path.isEmpty()) return errorReply(QStringLiteral("Load a DSP graph JSON first"));
        const QString id = request.value(QStringLiteral("id")).toString();
        const QString field = request.value(QStringLiteral("field")).toString();
        const QJsonValue value = request.value(QStringLiteral("value"));
        QJsonArray blocks = definition.value(QStringLiteral("blocks")).toArray();
        int index = -1;
        for (int i = 0; i < blocks.size(); ++i)
            if (blocks[i].toObject().value(QStringLiteral("id")).toString() == id) { index = i; break; }
        if (index < 0) return errorReply(QStringLiteral("Unknown graph block ID"));
        QJsonObject block = blocks[index].toObject();
        const QString type = block.value(QStringLiteral("type")).toString();
        const QHash<QString, QStringList> fields{
            {QStringLiteral("ddc_channelizer"), {QStringLiteral("enabled"), QStringLiteral("frequencyHz"),
                QStringLiteral("bandwidthHz"), QStringLiteral("outputRateHz"), QStringLiteral("decimation"),
                QStringLiteral("firTaps"), QStringLiteral("transitionHz"), QStringLiteral("externalProcessing")}},
            {QStringLiteral("pfb_channelizer"), {QStringLiteral("channels"), QStringLiteral("tapsPerPhase")}},
            {QStringLiteral("branch_fft"), {QStringLiteral("fftLength"), QStringLiteral("window"), QStringLiteral("refreshMs")}},
            {QStringLiteral("branch_demodulator"), {QStringLiteral("modulation"), QStringLiteral("bandwidthHz"),
                QStringLiteral("lowPassHz"), QStringLiteral("squelchDb")}},
            {QStringLiteral("branch_audio_output"), {QStringLiteral("enabled"), QStringLiteral("volume")}}
        };
        if (!fields.value(type).contains(field))
            return errorReply(QStringLiteral("Field is not editable for this graph block"));
        if ((field == QStringLiteral("enabled") || field == QStringLiteral("externalProcessing"))
                ? !value.isBool() : !value.isDouble())
            return errorReply(QStringLiteral("Graph setting has the wrong value type"));
        if (value.isDouble()) {
            const double number = value.toDouble();
            const bool integer = field == QStringLiteral("decimation") || field == QStringLiteral("firTaps") ||
                field == QStringLiteral("channels") || field == QStringLiteral("tapsPerPhase") ||
                field == QStringLiteral("fftLength") || field == QStringLiteral("window") ||
                field == QStringLiteral("refreshMs") || field == QStringLiteral("modulation");
            bool valid = std::isfinite(number) && (!integer || std::floor(number) == number);
            if (field == QStringLiteral("frequencyHz")) valid &= number >= 0.0 && number <= 8e9;
            else if (field == QStringLiteral("bandwidthHz")) valid &= number >= 100.0 &&
                number <= (type == QStringLiteral("branch_demodulator") ? 250000.0 : 1500000.0);
            else if (field == QStringLiteral("outputRateHz")) valid &= number >= 1000.0 && number <= 2e6;
            else if (field == QStringLiteral("decimation")) valid &= number >= 0.0 && number <= 4096.0;
            else if (field == QStringLiteral("firTaps")) valid &=
                (number == 16 || number == 32 || number == 64 || number == 128 || number == 256);
            else if (field == QStringLiteral("transitionHz")) valid &= number >= 0.0 && number <= 500000.0;
            else if (field == QStringLiteral("channels")) valid &=
                (number == 8 || number == 16 || number == 32 || number == 64);
            else if (field == QStringLiteral("tapsPerPhase")) valid &= number >= 8 && number <= 64 &&
                static_cast<int>(number) % 2 == 0;
            else if (field == QStringLiteral("fftLength")) valid &= number >= 512 && number <= 65536 &&
                (static_cast<int>(number) & (static_cast<int>(number) - 1)) == 0;
            else if (field == QStringLiteral("window")) valid &= number >= 0 && number <= 4;
            else if (field == QStringLiteral("refreshMs")) valid &= number >= 20 && number <= 2000;
            else if (field == QStringLiteral("modulation")) valid &= number >= 0 && number <= 7;
            else if (field == QStringLiteral("lowPassHz")) valid &= number >= 100 && number <= 20000;
            else if (field == QStringLiteral("squelchDb")) valid &= number >= -200 && number <= 0;
            else if (field == QStringLiteral("volume")) valid &= number >= 0 && number <= 2;
            if (!valid) return errorReply(QStringLiteral("Graph setting is outside its supported range"));
        }
        if (active && (field == QStringLiteral("enabled") || field == QStringLiteral("externalProcessing")))
            return errorReply(QStringLiteral("Stop the graph before changing enabled sinks, branches or processing routes"));
        QJsonObject settings = block.value(QStringLiteral("settings")).toObject();
        settings.insert(field, value);
        block.insert(QStringLiteral("settings"), settings);
        blocks[index] = block;
        QJsonObject candidate = definition;
        candidate.insert(QStringLiteral("blocks"), blocks);
        const DspGraphPlan compiled = compileDspGraph(candidate);
        if (!compiled.errors.isEmpty()) return errorReply(compiled.errors.join(QStringLiteral("; ")));
        if (active) {
            const QString error = validate(candidate, compiled, center, rate);
            if (!error.isEmpty()) return errorReply(error);
        }
        const bool changed = compiled.executionKey != plan.executionKey;
        definition = candidate;
        plan = compiled;
        modified = true;
        if (active && engine) {
            if (changed) IqBuffer::clear();
            engine->configure(plan, sourceSettings(center, rate), true);
            frames.clear();
        }
        return {{QStringLiteral("ok"), true}, {QStringLiteral("graph"), status()}};
    }
    if (operation == QStringLiteral("stop")) {
        stop();
        return {{QStringLiteral("ok"), true}, {QStringLiteral("graph"), status()}};
    }
    if (operation == QStringLiteral("load")) {
        if (active) return errorReply(QStringLiteral("Stop the graph before loading a new one"));
        const QString candidatePath = request.value(QStringLiteral("path")).toString();
        QFile file(candidatePath);
        if (!file.open(QIODevice::ReadOnly) || file.size() > 2 * 1024 * 1024)
            return errorReply(QStringLiteral("Cannot read DSP graph JSON or it exceeds 2 MiB"));
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject())
            return errorReply(QStringLiteral("DSP graph must be a JSON object"));
        const DspGraphPlan compiled = compileDspGraph(document.object());
        if (!compiled.errors.isEmpty()) return errorReply(compiled.errors.join(QStringLiteral("; ")));
        path = candidatePath;
        modified = false;
        definition = document.object();
        plan = compiled;
        return {{QStringLiteral("ok"), true}, {QStringLiteral("graph"), status()}};
    }
    if (operation == QStringLiteral("restoreDefinition")) {
        if (active) return errorReply(QStringLiteral("Stop the graph before restoring it"));
        const QJsonObject candidate = request.value(QStringLiteral("definition")).toObject();
        const DspGraphPlan compiled = compileDspGraph(candidate);
        if (!compiled.errors.isEmpty()) return errorReply(compiled.errors.join(QStringLiteral("; ")));
        path = QStringLiteral("<experiment preset>");
        modified = false;
        definition = candidate;
        plan = compiled;
        return {{QStringLiteral("ok"), true}, {QStringLiteral("graph"), status()}};
    }
    if (operation != QStringLiteral("start")) return errorReply(QStringLiteral("Unknown graph operation"));
    if (active) return {{QStringLiteral("ok"), true}, {QStringLiteral("graph"), status()}};
    if (path.isEmpty()) return errorReply(QStringLiteral("Load a DSP graph JSON first"));
    if (!std::isfinite(center) || !std::isfinite(rate) || rate <= 0.0)
        return errorReply(QStringLiteral("A valid local IQ receiver configuration is required"));
    const QString validationError = validate(definition, plan, center, rate);
    if (!validationError.isEmpty()) return errorReply(validationError);
    for (const DspBranchConfig &branch : plan.branches) {
        if (!branch.enabled || !branch.audioEnabled || branch.audioOutputId.isEmpty()) continue;
        auto &port = ports[branch.audioOutputId];
        port.server = std::make_unique<QTcpServer>();
        if (!port.server->listen(QHostAddress::LocalHost, 0)) {
            const QString message = port.server->errorString();
            ports.clear();
            return errorReply(message);
        }
        const QString sinkId = branch.audioOutputId;
        QObject::connect(port.server.get(), &QTcpServer::newConnection, port.server.get(),
                         [this, sinkId] {
            auto found = ports.find(sinkId);
            if (found == ports.end()) return;
            PcmPort &target = found->second;
            while (target.server->hasPendingConnections()) {
                QTcpSocket *client = target.server->nextPendingConnection();
                target.clients.push_back(client);
                QObject::connect(client, &QTcpSocket::disconnected, target.server.get(),
                                 [this, sinkId, client] {
                    auto current = ports.find(sinkId);
                    if (current != ports.end()) {
                        auto &clients = current->second.clients;
                        clients.erase(std::remove(clients.begin(), clients.end(), client), clients.end());
                    }
                    client->deleteLater();
                });
            }
        });
    }
    centerHz = center;
    sampleRateHz = rate;
    IqBuffer::clear();
    IqBuffer::setSampleRateEstimate(rate);
    engine = std::make_unique<DspBranchEngine>();
    engine->setExternalEndpoint(remoteEndpoint);
    engine->setAudioConsumer([this](const DspBranchPcmFrame &frame) { enqueueAudio(frame); });
    engine->configure(plan, sourceSettings(center, rate), true);
    active = true;
    return {{QStringLiteral("ok"), true}, {QStringLiteral("sourceRunning"), receiverRunning},
            {QStringLiteral("graph"), status()}};
}

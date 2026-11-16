#include "analyzerinstrumentscpi.h"

#include <QDeadlineTimer>
#include <QNetworkProxy>
#include <QRegularExpression>
#include <QTcpSocket>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

namespace {
constexpr int MaximumReplyBytes = 4096;
constexpr int MaximumSweepMs = 15 * 60 * 1000;
constexpr int MaximumTimeoutMs = 5000;

QPair<QString, quint16> endpoint(const QString &resource) {
    static const QRegularExpression pattern(
        QStringLiteral("\\ATCPIP[0-9]*::([A-Za-z0-9_.-]{1,253})::([0-9]{1,5})::SOCKET\\z"),
        QRegularExpression::CaseInsensitiveOption);
    const auto match = pattern.match(resource.trimmed());
    return {match.captured(1), match.captured(2).toUShort()};
}

bool asciiCommand(const QString &command) {
    return command.size() <= 256 && std::all_of(command.cbegin(), command.cend(), [](QChar c) {
        return c.unicode() >= ' ' && c.unicode() <= '~' && c != QLatin1Char(';');
    });
}

bool queryCommand(const QString &command) {
    return asciiCommand(command) && command.endsWith(QLatin1Char('?')) && command.count(QLatin1Char('?')) == 1;
}

bool setter(const QString &command) {
    return asciiCommand(command) && command.count(QStringLiteral("%1")) == 1 &&
           !command.contains(QLatin1Char('?'));
}

class ScpiSweep {
public:
    ScpiSweep(const AnalyzerInstrumentPlan &plan, const std::atomic_bool &cancelled)
        : plan(plan), cancelled(cancelled), sweepDeadline(MaximumSweepMs) {
        socket.setProxy(QNetworkProxy::NoProxy);
        socket.setReadBufferSize(MaximumReplyBytes);
    }

    QString run(const AnalyzerInstrumentBackend::PointCallback &report) {
        QString error = sweep(report);
        if (identified && !disableOutput(error.isEmpty())) {
            if (!error.isEmpty()) error += QLatin1Char('\n');
            error += QStringLiteral("SCPI output OFF could not be acknowledged.");
        }
        return error;
    }

private:
    bool active() const { return !cancelled.load() && !sweepDeadline.hasExpired(); }

    bool connectDevice(bool cleanup = false) {
        const auto address = endpoint(plan.scpi.resource);
        socket.connectToHost(address.first, address.second);
        const QDeadlineTimer deadline(plan.scpi.timeoutMs);
        while (socket.state() != QAbstractSocket::ConnectedState) {
            if (socket.state() == QAbstractSocket::UnconnectedState || deadline.hasExpired() ||
                (!cleanup && !active()))
                return false;
            socket.waitForConnected(int(deadline.remainingTime()));
        }
        return true;
    }

    QByteArray query(const QStringList &commands, bool cleanup = false) {
        QByteArray wire;
        for (const auto &command : commands) {
            if (!command.isEmpty()) wire += command.toLatin1() + '\n';
        }
        const QDeadlineTimer deadline(plan.scpi.timeoutMs);
        if ((!cleanup && !active()) || socket.state() != QAbstractSocket::ConnectedState ||
            socket.write(wire) != wire.size()) return {};
        while (socket.bytesToWrite() > 0) {
            if (deadline.hasExpired() || (!cleanup && !active()) ||
                socket.state() != QAbstractSocket::ConnectedState) return {};
            socket.waitForBytesWritten(int(std::min<qint64>(50, deadline.remainingTime())));
        }
        while (!socket.canReadLine()) {
            if (deadline.hasExpired() || (!cleanup && !active()) ||
                socket.state() != QAbstractSocket::ConnectedState ||
                socket.bytesAvailable() >= MaximumReplyBytes) return {};
            socket.waitForReadyRead(int(std::min<qint64>(50, deadline.remainingTime())));
        }
        return socket.readLine(MaximumReplyBytes).trimmed();
    }

    bool noInstrumentError(bool cleanup = false) {
        const auto reply = query({QStringLiteral("SYST:ERR?")}, cleanup);
        bool ok = false;
        const int code = reply.left(reply.indexOf(',') < 0 ? reply.size() : reply.indexOf(',')).toInt(&ok);
        return ok && code == 0;
    }

    bool wait(double delayMs) const {
        const QDeadlineTimer deadline(qint64(std::ceil(delayMs)), Qt::PreciseTimer);
        while (!deadline.hasExpired() && active())
            QThread::msleep(static_cast<unsigned long>(std::min<qint64>(20, deadline.remainingTime())));
        return active();
    }

    bool tune(double frequency, bool first) {
        const auto &p = plan.scpi;
        QStringList commands;
        if (first) {
            commands << p.outputCommand.arg(QStringLiteral("OFF"))
                     << p.powerCommand.arg(plan.sourceLevelDbm, 0, 'g', 12);
        }
        commands << p.frequencyCommand.arg(frequency, 0, 'g', 14) << QStringLiteral("*OPC?");
        return query(commands) == "1" && noInstrumentError();
    }

    QString failed(const char *operation) const {
        if (cancelled.load()) return QStringLiteral("SCPI sweep stopped by the operator.");
        if (sweepDeadline.hasExpired()) return QStringLiteral("SCPI sweep reached its time limit.");
        return QStringLiteral("SCPI failed at %1.").arg(QString::fromLatin1(operation));
    }

    QString sweep(const AnalyzerInstrumentBackend::PointCallback &report) {
        const auto &p = plan.scpi;
        const auto frequencies = AnalyzerInstrumentPlanner::frequencyGrid(plan);
        if (!connectDevice() || query({p.identityQuery}).isEmpty()) return failed("connection or identity query");
        identified = true;
        const int count = plan.bidirectionalSweep ? 2 * frequencies.size() - 1 : frequencies.size();
        for (int index = 0; index < count; ++index) {
            const double frequency = frequencies.at(index < frequencies.size() ? index : count - index - 1);
            if (!tune(frequency, index == 0)) return failed("frequency or source power setup");
            if (index == 0) {
                if (query({p.outputCommand.arg(QStringLiteral("ON")), QStringLiteral("*OPC?")}) != "1" ||
                    !noInstrumentError()) return failed("output enable");
            }
            if (!wait(plan.settleMs)) return failed("settling");
            double powerMw = 0.0;
            for (int i = 0; i < plan.averages; ++i) {
                if (!wait(plan.dwellMs)) return failed("dwell");
                if (!p.triggerCommand.isEmpty() &&
                    query({p.triggerCommand, QStringLiteral("*OPC?")}) != "1") return failed("trigger");
                bool numeric = false;
                const double dbm = query({p.readCommand}).toDouble(&numeric);
                if (!numeric || !std::isfinite(dbm) || dbm < -250.0 || dbm > 100.0)
                    return failed("numeric dBm readback");
                powerMw += std::pow(10.0, dbm / 10.0);
            }
            AnalyzerMeasurementPoint point;
            point.frequencyHz = frequency;
            point.stimulusDbm = plan.sourceLevelDbm;
            point.forwardDb = 10.0 * std::log10(powerMw / plan.averages);
            point.valid = true;
            point.forwardOnly = true;
            report(point);
        }
        return {};
    }

    bool disableOutput(bool synchronized) {
        const auto off = plan.scpi.outputCommand.arg(QStringLiteral("OFF"));
        const QStringList commands{off, QStringLiteral("*OPC?")};
        if (synchronized && query(commands, true) == "1" && noInstrumentError(true)) return true;
        // A timed-out read may still have a reply in flight. Send OFF on the
        // current stream, then use a fresh session for an unambiguous reply.
        if (socket.state() == QAbstractSocket::ConnectedState) {
            socket.write(off.toLatin1() + '\n');
            socket.disconnectFromHost();
            if (socket.state() != QAbstractSocket::UnconnectedState)
                socket.waitForDisconnected(std::min(200, plan.scpi.timeoutMs));
        }
        socket.abort();
        return connectDevice(true) && query(commands, true) == "1" && noInstrumentError(true);
    }

    const AnalyzerInstrumentPlan &plan;
    const std::atomic_bool &cancelled;
    QDeadlineTimer sweepDeadline;
    QTcpSocket socket;
    bool identified = false;
};
}

AnalyzerInstrumentScpi::~AnalyzerInstrumentScpi() {
    cancel();
    if (worker) {
        worker->wait();
        delete worker;
    }
}

QString AnalyzerInstrumentScpi::backendId() const { return QStringLiteral("scpi-tcp-tracking"); }

AnalyzerHardwareCapabilities AnalyzerInstrumentScpi::capabilities() const {
    AnalyzerHardwareCapabilities caps;
    caps.receiverAvailable = caps.externalGenerator = true;
    caps.receiverChannels = 1;
    return caps;
}

QString AnalyzerInstrumentScpi::validationError(const AnalyzerInstrumentPlan &plan) {
    if (plan.mode != AnalyzerInstrumentMode::TrackingGenerator || !plan.useExternalGenerator || !plan.scpi.enabled)
        return QStringLiteral("Select Tracking generator and enable the external SCPI profile.");
    const auto validation = AnalyzerInstrumentPlanner::validate(plan);
    if (!validation.executable) return validation.summary();
    if (plan.sourceLevelDbm > plan.capabilities.maximumSafeOutputDbm)
        return QStringLiteral("Requested source power exceeds the declared safe output limit.");
    const auto &p = plan.scpi;
    if (endpoint(p.resource).second == 0 || !queryCommand(p.identityQuery) || !queryCommand(p.readCommand) ||
        !setter(p.frequencyCommand) || !setter(p.powerCommand) || !setter(p.outputCommand) ||
        !asciiCommand(p.triggerCommand) || p.triggerCommand.contains(QLatin1Char('?')))
        return QStringLiteral("Set a TCPIP::host::port::SOCKET resource, SCPI setters and a dBm read query.");
    const qint64 count = plan.bidirectionalSweep ? 2LL * plan.points - 1 : plan.points;
    if (p.timeoutMs < 100 || p.timeoutMs > MaximumTimeoutMs || count > 2000 ||
        count * plan.averages > 20000 || count * (plan.settleMs + plan.dwellMs * plan.averages) > MaximumSweepMs)
        return QStringLiteral("SCPI limits are 100–5000 ms per query, 2000 points, 20000 readings and 15 minutes.");
    return {};
}

bool AnalyzerInstrumentScpi::start(const AnalyzerInstrumentPlan &plan, PointCallback report,
                                   CompletionCallback complete) {
    if (worker || !report || !complete || !validationError(plan).isEmpty()) return false;
    cancelled.store(false);
    pointReady = std::move(report);
    const auto error = std::make_shared<QString>();
    worker = QThread::create([this, plan, error] {
        *error = ScpiSweep(plan, cancelled).run([this](const AnalyzerMeasurementPoint &point) {
            QMetaObject::invokeMethod(this, [this, point] {
                if (pointReady) pointReady(point);
            }, Qt::QueuedConnection);
        });
    });
    QObject::connect(worker, &QThread::finished, this, [this, complete = std::move(complete), error] {
        worker->wait();
        worker->deleteLater();
        worker = nullptr;
        pointReady = {};
        complete(*error);
    });
    worker->start();
    return true;
}

void AnalyzerInstrumentScpi::cancel() { cancelled.store(true); }

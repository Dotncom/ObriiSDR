#include "clinetworkbridge.h"
#include "clispectrummeter.h"

#include <QApplication>
#include <QCheckBox>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <vector>

namespace {

QString socketName(const QString &id) { return QStringLiteral("obrii-cli-view-") + id; }

QString formatHz(double hz) {
    if (std::abs(hz) >= 1e9) return QString::number(hz / 1e9, 'f', 3) + QStringLiteral(" GHz");
    if (std::abs(hz) >= 1e6) return QString::number(hz / 1e6, 'f', 3) + QStringLiteral(" MHz");
    if (std::abs(hz) >= 1e3) return QString::number(hz / 1e3, 'f', 1) + QStringLiteral(" kHz");
    return QString::number(hz, 'f', 0) + QStringLiteral(" Hz");
}

QColor heatColor(float value, double minimum, double maximum) {
    const double t = std::clamp((static_cast<double>(value) - minimum) / (maximum - minimum), 0.0, 1.0);
    if (t < 0.25) return QColor::fromRgbF(0.035 + t * 0.20, 0.07 + t * 0.53, 0.10 + t * 2.0);
    if (t < 0.5) return QColor::fromRgbF(0.085 + (t - 0.25) * 0.80, 0.2025 + (t - 0.25) * 2.10,
                                         0.60 + (t - 0.25) * 1.0);
    if (t < 0.75) return QColor::fromRgbF(0.285 + (t - 0.5) * 2.5, 0.7275 + (t - 0.5) * 0.90,
                                          0.85 - (t - 0.5) * 2.0);
    return QColor::fromRgbF(0.91 + (t - 0.75) * 0.36, 0.9525 - (t - 0.75) * 2.4,
                            0.35 - (t - 0.75) * 1.2);
}

class PlotCanvas final : public QWidget {
public:
    explicit PlotCanvas(QString viewKind, QWidget *parent = nullptr) : QWidget(parent), kind(std::move(viewKind)) {
        setMinimumSize(kind == QStringLiteral("scale") ? QSize(480, 110) : QSize(480, 260));
        setMouseTracking(true);
        setAutoFillBackground(false);
    }

    QString kind;
    double minHz = 0.0, maxHz = 0.0;
    double minDb = -120.0, maxDb = -20.0;
    double sourceMinHz = 0.0, sourceMaxHz = 0.0;
    bool paused = false;
    quint64 frames = 0;
    std::function<void()> changed;

    void setViewport(double minimum, double maximum) {
        minHz = minimum; maxHz = maximum;
        waterfall.fill(QColor("#101718"));
        update();
        if (changed) changed();
    }
    void setLevels(double minimum, double maximum) {
        minDb = minimum; maxDb = maximum;
        waterfall.fill(QColor("#101718"));
        update();
    }
    void setSourceRange(double minimum, double maximum) {
        if (std::isfinite(minimum) && std::isfinite(maximum) && maximum > minimum) {
            sourceMinHz = minimum; sourceMaxHz = maximum; update();
        }
    }
    double leftHz() const { return minHz < maxHz ? minHz : sourceMinHz; }
    double rightHz() const { return minHz < maxHz ? maxHz : sourceMaxHz; }

    void ingest(const QJsonObject &frame) {
        if (paused) return;
        const QJsonArray frequencyArray = frame.value(QStringLiteral("frequencies")).toArray();
        const QJsonArray levelArray = frame.value(QStringLiteral("magnitudes")).toArray();
        const int count = (std::min)(frequencyArray.size(), levelArray.size());
        if (count < 2) return;
        frequencies.resize(static_cast<std::size_t>(count));
        levels.resize(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) {
            frequencies[static_cast<std::size_t>(i)] = frequencyArray[i].toDouble();
            levels[static_cast<std::size_t>(i)] = levelArray[i].toDouble(-160.0);
        }
        setSourceRange(frequencies.front(), frequencies.back());
        ++frames;
        if (kind == QStringLiteral("level")) {
            float peak = -160.0f;
            for (int i = 0; i < count; ++i)
                if (frequencies[static_cast<std::size_t>(i)] >= leftHz() &&
                    frequencies[static_cast<std::size_t>(i)] <= rightHz())
                    peak = (std::max)(peak, levels[static_cast<std::size_t>(i)]);
            history.push_back(peak);
            if (history.size() > 600) history.pop_front();
        }
        if (kind == QStringLiteral("waterfall") || kind == QStringLiteral("both")) pushWaterfall();
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.fillRect(rect(), QColor("#101718"));
        p.setRenderHint(QPainter::Antialiasing);
        const QRect plot(56, 14, (std::max)(1, width() - 68), (std::max)(1, height() - 48));
        if (kind == QStringLiteral("scale")) {
            drawAxis(p, QRect(plot.left(), 24, plot.width(), 54));
            return;
        }
        if (kind == QStringLiteral("both")) {
            const int upper = plot.height() * 45 / 100;
            drawGraph(p, QRect(plot.left(), plot.top(), plot.width(), upper), false);
            drawWaterfall(p, QRect(plot.left(), plot.top() + upper + 5,
                                   plot.width(), plot.height() - upper - 5));
        } else if (kind == QStringLiteral("waterfall")) drawWaterfall(p, plot);
        else drawGraph(p, plot, kind == QStringLiteral("level"));
        drawAxis(p, QRect(plot.left(), plot.bottom() + 2, plot.width(), 25));
        if (paused) {
            p.setPen(QColor("#f0c771"));
            p.drawText(QRect(plot.right() - 82, plot.top() + 8, 76, 20), Qt::AlignRight, QStringLiteral("PAUSED"));
        }
    }

    void wheelEvent(QWheelEvent *event) override {
        const double left = leftHz(), right = rightHz();
        if (!(right > left)) return;
        const double factor = event->angleDelta().y() > 0 ? 0.8 : 1.25;
        const double pivot = std::clamp((event->position().x() - 56.0) / (width() - 68.0), 0.0, 1.0);
        const double frequency = left + pivot * (right - left);
        setViewport(frequency - (frequency - left) * factor,
                    frequency + (right - frequency) * factor);
        event->accept();
    }

    void mousePressEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton) dragStart = event->pos();
    }
    void mouseMoveEvent(QMouseEvent *event) override {
        if (!(event->buttons() & Qt::LeftButton) || dragStart.isNull()) return;
        const double span = rightHz() - leftHz();
        if (span <= 0.0) return;
        const double delta = (dragStart.x() - event->pos().x()) * span / (std::max)(1, width() - 68);
        setViewport(leftHz() + delta, rightHz() + delta);
        dragStart = event->pos();
    }
    void mouseReleaseEvent(QMouseEvent *) override { dragStart = {}; }

private:
    void drawAxis(QPainter &p, const QRect &area) const {
        p.setPen(QColor("#718889"));
        p.drawLine(area.left(), area.top(), area.right(), area.top());
        if (!(rightHz() > leftHz())) return;
        for (int i = 0; i <= 8; ++i) {
            const int x = area.left() + i * area.width() / 8;
            p.drawLine(x, area.top(), x, area.top() + (i % 2 ? 5 : 9));
            if (i % 2 == 0) {
                const double hz = leftHz() + (rightHz() - leftHz()) * i / 8.0;
                constexpr int labelWidth = 148;
                const int labelX = i == 0 ? x : i == 8 ? x - labelWidth : x - labelWidth / 2;
                const int alignment = i == 0 ? Qt::AlignLeft : i == 8 ? Qt::AlignRight : Qt::AlignHCenter;
                p.drawText(QRect(labelX, area.top() + 11, labelWidth, 16), alignment, formatHz(hz));
            }
        }
    }
    void drawGrid(QPainter &p, const QRect &area) const {
        p.setPen(QColor("#263739"));
        for (int i = 0; i <= 8; ++i) p.drawLine(area.left() + i * area.width() / 8, area.top(),
                                                  area.left() + i * area.width() / 8, area.bottom());
        for (int i = 0; i <= 5; ++i) p.drawLine(area.left(), area.top() + i * area.height() / 5,
                                                  area.right(), area.top() + i * area.height() / 5);
        p.setPen(QColor("#9dafaf"));
        for (int i = 0; i <= 5; ++i)
            p.drawText(QRect(3, area.top() + i * area.height() / 5 - 8, 49, 17), Qt::AlignRight,
                       QString::number(maxDb - (maxDb - minDb) * i / 5.0, 'f', 0));
    }
    void drawGraph(QPainter &p, const QRect &area, bool levelGraph) const {
        p.fillRect(area, QColor("#141f21"));
        drawGrid(p, area);
        QPainterPath path;
        int points = 0;
        if (levelGraph) {
            for (std::size_t i = 0; i < history.size(); ++i) {
                const double x = area.left() + i * area.width() / (std::max)(std::size_t(1), history.size() - 1);
                const double y = area.bottom() - std::clamp((history[i] - minDb) / (maxDb - minDb), 0.0, 1.0) * area.height();
                if (!points++) path.moveTo(x, y); else path.lineTo(x, y);
            }
        } else {
            for (std::size_t i = 0; i < frequencies.size(); ++i) {
                if (frequencies[i] < leftHz() || frequencies[i] > rightHz()) continue;
                const double x = area.left() + (frequencies[i] - leftHz()) / (rightHz() - leftHz()) * area.width();
                const double y = area.bottom() - std::clamp((levels[i] - minDb) / (maxDb - minDb), 0.0, 1.0) * area.height();
                if (!points++) path.moveTo(x, y); else path.lineTo(x, y);
            }
        }
        p.setPen(QPen(QColor("#61d9c7"), 1.4));
        if (points > 1) p.drawPath(path);
    }
    void drawWaterfall(QPainter &p, const QRect &area) const {
        p.fillRect(area, QColor("#101718"));
        if (!waterfall.isNull()) p.drawImage(area, waterfall);
        p.setPen(QColor("#496368"));
        p.drawRect(area.adjusted(0, 0, -1, -1));
    }
    void pushWaterfall() {
        constexpr int imageWidth = 1024, imageHeight = 256;
        if (waterfall.isNull()) {
            waterfall = QImage(imageWidth, imageHeight, QImage::Format_RGB32);
            waterfall.fill(QColor("#101718"));
        }
        const int stride = waterfall.bytesPerLine();
        std::memmove(waterfall.bits() + stride, waterfall.bits(), static_cast<std::size_t>(stride) * (imageHeight - 1));
        auto *row = reinterpret_cast<QRgb *>(waterfall.bits());
        const double left = leftHz(), right = rightHz();
        if (!(right > left) || frequencies.size() < 2) return;
        for (int x = 0; x < imageWidth; ++x) {
            const double hz = left + (right - left) * x / (imageWidth - 1.0);
            const auto it = std::lower_bound(frequencies.begin(), frequencies.end(), hz);
            float db = static_cast<float>(minDb);
            if (it != frequencies.end() && it != frequencies.begin()) {
                const std::size_t i = static_cast<std::size_t>(it - frequencies.begin());
                db = (std::max)(levels[i - 1], levels[i]);
            }
            row[x] = heatColor(db, minDb, maxDb).rgb();
        }
    }
    std::vector<float> frequencies, levels;
    std::deque<float> history;
    QImage waterfall;
    QPoint dragStart;
};

class ViewWindow final : public QWidget {
public:
    ViewWindow(QString viewId, QString viewKind, QString sourceHost, quint16 sourcePort)
        : id(std::move(viewId)), host(std::move(sourceHost)), port(sourcePort),
          canvas(std::move(viewKind), this), bridge(this), local(this) {
        setWindowTitle(QStringLiteral("Obrii CLI - %1 [%2]").arg(canvas.kind, id));
        resize(canvas.kind == QStringLiteral("scale") ? 760 : 900,
               canvas.kind == QStringLiteral("scale") ? 170 : 540);
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(7, 7, 7, 7);
        auto *header = new QHBoxLayout();
        sourceLabel = new QLabel(this);
        header->addWidget(sourceLabel, 1);
        pauseCheck = new QCheckBox(QStringLiteral("Pause"), this);
        header->addWidget(pauseCheck);
        layout->addLayout(header);
        layout->addWidget(&canvas, 1);
        setStyleSheet(QStringLiteral("QWidget { background:#101718; color:#d5e1df; } "
                                     "QCheckBox { spacing:5px; }"));
        connect(pauseCheck, &QCheckBox::toggled, this, [this](bool checked) {
            canvas.paused = checked; canvas.update();
        });
        canvas.changed = [this] { updateSourceLabel(); };
        updateSourceLabel();
        bridge.onClientSpectrum = [this](const QJsonObject &frame) {
            canvas.ingest(frame); updateSourceLabel();
        };
        bridge.onClientState = [this](const QJsonObject &state) {
            const QJsonObject settings = state.value(QStringLiteral("settings")).toObject();
            const double center = settings.value(QStringLiteral("centerFrequency")).toDouble();
            const double rate = settings.value(QStringLiteral("sampleRate")).toDouble();
            canvas.setSourceRange(center - rate / 2, center + rate / 2);
            updateSourceLabel();
        };
        bridge.onClientIq = [this](const QJsonObject &frame, const QByteArray &bytes) {
            if (frame.value(QStringLiteral("sampleFormat")).toString() != QStringLiteral("iq_s8_interleaved") ||
                bytes.size() < 8192 || bytes.size() % 2 != 0 || iqTimer.elapsed() < 70) return;
            if (!meter) meter = std::make_unique<CliSpectrumMeter>(4096);
            if (!meter->valid()) return;
            iqSamples.resize(static_cast<std::size_t>(bytes.size()));
            const auto *raw = reinterpret_cast<const signed char *>(bytes.constData());
            for (std::size_t i = 0; i < iqSamples.size(); ++i)
                iqSamples[i] = static_cast<float>(raw[i]) / 127.0f;
            std::vector<float> ordered;
            const double rate = frame.value(QStringLiteral("sampleRate")).toDouble();
            const double center = frame.value(QStringLiteral("centerFrequency")).toDouble();
            meter->analyze(iqSamples.data(), static_cast<int>(iqSamples.size() / 2), rate, center, &ordered);
            if (ordered.empty()) return;
            QJsonArray frequencies, magnitudes;
            for (int i = 0; i < static_cast<int>(ordered.size()); ++i) {
                frequencies.append(center - rate / 2 + rate * i / ordered.size());
                magnitudes.append(ordered[static_cast<std::size_t>(i)]);
            }
            canvas.ingest({{QStringLiteral("frequencies"), frequencies},
                           {QStringLiteral("magnitudes"), magnitudes}});
            iqTimer.restart();
        };
        iqTimer.start();
        bridge.connectClient(host, port);
        connect(&reconnect, &QTimer::timeout, this, [this] {
            if (!bridge.ready()) bridge.connectClient(host, port);
            updateSourceLabel();
        });
        reconnect.start(3000);
    }

    bool startControl(QString *error) {
        if (local.listen(socketName(id))) return true;
        *error = local.errorString();
        return false;
    }

    void enableControl() {
        connect(&local, &QLocalServer::newConnection, this, [this] {
            while (local.hasPendingConnections()) {
                QLocalSocket *socket = local.nextPendingConnection();
                connect(socket, &QLocalSocket::disconnected, socket, &QLocalSocket::deleteLater);
                connect(socket, &QLocalSocket::readyRead, socket, [this, socket] {
                    if (!socket->canReadLine()) return;
                    const QJsonObject request = QJsonDocument::fromJson(socket->readLine(8192)).object();
                    const QString action = request.value(QStringLiteral("action")).toString();
                    QJsonObject reply{{QStringLiteral("ok"), true}};
                    if (action == QStringLiteral("status")) reply = status();
                    else if (action == QStringLiteral("set")) {
                        const QJsonObject patch = request.value(QStringLiteral("settings")).toObject();
                        const double minimum = patch.value(QStringLiteral("minHz")).toDouble(canvas.leftHz());
                        const double maximum = patch.value(QStringLiteral("maxHz")).toDouble(canvas.rightHz());
                        const double bottom = patch.value(QStringLiteral("minDb")).toDouble(canvas.minDb);
                        const double top = patch.value(QStringLiteral("maxDb")).toDouble(canvas.maxDb);
                        const int requestedPort = patch.value(QStringLiteral("port")).toInt(port);
                        const QString requestedHost = patch.value(QStringLiteral("host")).toString(host);
                        if ((patch.contains(QStringLiteral("minHz")) || patch.contains(QStringLiteral("maxHz"))) &&
                            (!std::isfinite(minimum) || !std::isfinite(maximum) || minimum <= 0 || maximum <= minimum))
                            reply = {{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("Invalid frequency range")}};
                        else if (!std::isfinite(bottom) || !std::isfinite(top) || bottom >= top ||
                                 requestedPort < 1 || requestedPort > 65535 || requestedHost.isEmpty())
                            reply = {{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("Invalid view settings")}};
                        else {
                            if (minimum != canvas.minHz || maximum != canvas.maxHz) canvas.setViewport(minimum, maximum);
                            if (bottom != canvas.minDb || top != canvas.maxDb) canvas.setLevels(bottom, top);
                            if (patch.contains(QStringLiteral("paused"))) {
                                canvas.paused = patch.value(QStringLiteral("paused")).toBool();
                                pauseCheck->setChecked(canvas.paused);
                            }
                            if (host != requestedHost || port != requestedPort) {
                                host = requestedHost; port = static_cast<quint16>(requestedPort);
                                bridge.connectClient(host, port);
                            }
                            updateSourceLabel();
                            reply = status();
                        }
                    } else if (action == QStringLiteral("snapshot")) {
                        const QString path = request.value(QStringLiteral("path")).toString();
                        if (path.isEmpty() || !path.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive) ||
                            QFile::exists(path) || !grab().save(path, "PNG"))
                            reply = {{QStringLiteral("ok"), false},
                                     {QStringLiteral("error"), QStringLiteral("Cannot write new PNG snapshot")}};
                        else reply.insert(QStringLiteral("path"), path);
                    } else if (action == QStringLiteral("close")) {
                        QTimer::singleShot(0, this, &QWidget::close);
                    } else reply = {{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("Unknown view action")}};
                    socket->write(QJsonDocument(reply).toJson(QJsonDocument::Compact) + '\n');
                    socket->flush();
                    socket->disconnectFromServer();
                });
            }
        });
    }

private:
    QJsonObject status() const {
        return {{QStringLiteral("ok"), true}, {QStringLiteral("id"), id},
                {QStringLiteral("kind"), canvas.kind}, {QStringLiteral("host"), host},
                {QStringLiteral("port"), port}, {QStringLiteral("connected"), bridge.ready()},
                {QStringLiteral("frames"), static_cast<double>(canvas.frames)},
                {QStringLiteral("minHz"), canvas.leftHz()}, {QStringLiteral("maxHz"), canvas.rightHz()},
                {QStringLiteral("minDb"), canvas.minDb}, {QStringLiteral("maxDb"), canvas.maxDb},
                {QStringLiteral("paused"), canvas.paused}};
    }
    void updateSourceLabel() {
        sourceLabel->setText(QStringLiteral("%1:%2  |  %3 - %4")
            .arg(host).arg(port).arg(formatHz(canvas.leftHz())).arg(formatHz(canvas.rightHz())));
    }
    QString id, host;
    quint16 port;
    PlotCanvas canvas;
    CliNetworkBridge bridge;
    QLocalServer local;
    QTimer reconnect;
    QLabel *sourceLabel = nullptr;
    QCheckBox *pauseCheck = nullptr;
    QElapsedTimer iqTimer;
    std::unique_ptr<CliSpectrumMeter> meter;
    std::vector<float> iqSamples;
};

} // namespace

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QCommandLineParser parser;
    parser.addOption({QStringLiteral("id"), QStringLiteral("Window ID"), QStringLiteral("id")});
    parser.addOption({QStringLiteral("kind"), QStringLiteral("spectrum, waterfall, both, level, scale"), QStringLiteral("kind")});
    parser.addOption({QStringLiteral("host"), QStringLiteral("Source server host"), QStringLiteral("host"), QStringLiteral("127.0.0.1")});
    parser.addOption({QStringLiteral("port"), QStringLiteral("Source server port"), QStringLiteral("port"), QStringLiteral("21090")});
    parser.process(app);
    const QString id = parser.value(QStringLiteral("id"));
    const QString kind = parser.value(QStringLiteral("kind"));
    bool okay = false;
    const int port = parser.value(QStringLiteral("port")).toInt(&okay);
    if (id.isEmpty() || !QStringList{QStringLiteral("spectrum"), QStringLiteral("waterfall"),
         QStringLiteral("both"), QStringLiteral("level"), QStringLiteral("scale")}.contains(kind) ||
        !okay || port < 1 || port > 65535) return 2;
    ViewWindow window(id, kind, parser.value(QStringLiteral("host")), static_cast<quint16>(port));
    QString error;
    if (!window.startControl(&error)) return 3;
    window.enableControl();
    window.show();
    return app.exec();
}

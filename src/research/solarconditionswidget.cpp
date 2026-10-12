#include "solarconditionswidget.h"
#include <QDesktopServices>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QSslSocket>
#include <cmath>
#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#endif

namespace {
QDateTime utc(QString text) {
    QDateTime time = QDateTime::fromString(text, Qt::ISODate);
    if (time.isValid() && !text.endsWith('Z') && !text.contains('+')) time.setTimeSpec(Qt::UTC);
    return time.toUTC();
}
QByteArray fetch(const QString &path, const std::atomic_bool &cancel, QString *error) {
    constexpr int limit = 1024 * 1024;
    if (cancel.load()) return {};
#ifdef Q_OS_WIN
    // Qt 5 distributions without OpenSSL still have the system TLS implementation.
    if (!QSslSocket::supportsSsl()) {
        HINTERNET session = WinHttpOpen(L"ObriiSDR-Solar/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) { *error = QStringLiteral("WinHTTP session error"); return {}; }
        WinHttpSetTimeouts(session, 1500, 1500, 1500, 1500);
        HINTERNET connection = WinHttpConnect(session, L"services.swpc.noaa.gov", INTERNET_DEFAULT_HTTPS_PORT, 0);
        HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET",
            reinterpret_cast<LPCWSTR>(path.utf16()), nullptr, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
        QByteArray data;
        bool okay = request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                       WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(request, nullptr);
        DWORD code = 0, size = sizeof(code);
        okay = okay && WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                         WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX) && code == 200;
        while (okay && !cancel.load()) {
            char chunk[8192]; DWORD count = 0;
            if (!WinHttpReadData(request, chunk, sizeof(chunk), &count)) {okay = false; break;}
            if (!count) break;
            if (data.size() + int(count) > limit) {okay = false; break;}
            data.append(chunk, int(count));
        }
        if (request) WinHttpCloseHandle(request);
        if (connection) WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        if (!okay || cancel.load()) { *error = QStringLiteral("NOAA HTTPS request failed or cancelled"); return {}; }
        return data;
    }
#endif
    QNetworkAccessManager manager;
    QNetworkRequest request(QUrl(QStringLiteral("https://services.swpc.noaa.gov") + path));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setRawHeader("User-Agent", "ObriiSDR-Solar/1.0");
    QNetworkReply *reply = manager.get(request);
    QEventLoop loop;
    QTimer timeout, cancellation;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, reply, &QNetworkReply::abort);
    QObject::connect(&cancellation, &QTimer::timeout, reply, [&]() { if (cancel.load()) reply->abort(); });
    QObject::connect(reply, &QNetworkReply::downloadProgress, reply, [&](qint64 count, qint64 total) {
        if (count > limit || total > limit) reply->abort();
    });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timeout.start(12000); cancellation.start(100);
    loop.exec();
    const QByteArray data = reply->readAll();
    if (reply->error() != QNetworkReply::NoError || data.size() > limit || cancel.load()) {
        *error = reply->errorString(); return {};
    }
    return data;
}
}
bool solar::parseFlux(const QByteArray &json, Observation *out) {
    if (!out) return false;
    const auto document = QJsonDocument::fromJson(json);
    if (!document.isArray()) return false;
    bool found = false;
    for (const auto &value : document.array()) {
        const auto object = value.toObject();
        const auto time = utc(object.value("time_tag").toString());
        const double flux = object.value("flux").toDouble(-1);
        if (!time.isValid() || object.value("frequency").toInt() != 2800) continue;
        if (flux > 0 && flux < 1000) found = true;
        if (flux > 0 && flux < 1000 && (!out->fluxUtc.isValid() || time > out->fluxUtc)) {
            out->fluxUtc = time; out->flux = flux; found = true;
        }
        const double mean = object.value("ninety_day_mean").toDouble(-1);
        if (mean > 0 && mean < 1000 && (!out->flux90Utc.isValid() || time > out->flux90Utc)) {
            out->flux90Utc = time; out->flux90 = mean;
        }
    }
    return found;
}
bool solar::parseKp(const QByteArray &json, Observation *out) {
    if (!out) return false;
    const auto document = QJsonDocument::fromJson(json);
    if (!document.isArray()) return false;
    bool found = false;
    for (const auto &value : document.array()) {
        const auto object = value.toObject();
        const auto time = utc(object.value("time_tag").toString());
        const double kp = object.value("Kp").toDouble(-1);
        if (time.isValid() && kp >= 0 && kp <= 9) found = true;
        if (time.isValid() && kp >= 0 && kp <= 9 && (!out->kpUtc.isValid() || time > out->kpUtc)) {
            out->kpUtc = time; out->kp = kp;
            const double ap = object.value("a_running").toDouble(-1);
            out->ap = ap >= 0 && ap <= 400 ? ap : std::numeric_limits<double>::quiet_NaN();
            found = true;
        }
    }
    return found;
}
SolarConditionsWidget::SolarConditionsWidget(bool ukrainian, QWidget *parent)
    : QWidget(parent), uk(ukrainian) {
    auto *layout = new QVBoxLayout(this);
    values = new QLabel(this); values->setWordWrap(true); values->setTextFormat(Qt::RichText);
    status = new QLabel(this); status->setWordWrap(true); status->setTextFormat(Qt::PlainText);
    refreshButton = new QPushButton(uk ? QString::fromUtf8(u8"Оновити NOAA") : "Refresh NOAA", this);
    layout->addWidget(refreshButton); layout->addWidget(values); layout->addWidget(status);
    auto *notes = new QLabel(uk ? QString::fromUtf8(u8"F10.7 — сонячний потік, sfu; середнє за 90 днів. Kp — планетарний індекс геомагнітної активності (0–9). Це орієнтири для КХ, а не розрахунок MUF чи прогноз конкретного радіозв'язку. Дані завантажуються лише цією кнопкою; координати не надсилаються.") :
        "F10.7: solar flux in sfu, with a 90-day mean. Kp: planetary geomagnetic activity (0-9). HF context only, not a MUF or point-to-point ionospheric forecast. Download is manual; no coordinates are sent.", this);
    notes->setWordWrap(true); layout->addWidget(notes);
    auto *source = new QPushButton(uk ? QString::fromUtf8(u8"Про індекси NOAA") : "NOAA index reference", this);
    layout->addWidget(source); layout->addStretch();
    status->setText(uk ? QString::fromUtf8(u8"Дані ще не завантажені") : "Not downloaded yet");
    connect(refreshButton, &QPushButton::clicked, this, &SolarConditionsWidget::refresh);
    connect(source, &QPushButton::clicked, this, []() {
        QDesktopServices::openUrl(QUrl("https://www.swpc.noaa.gov/products/planetary-k-index"));
    });
}
SolarConditionsWidget::~SolarConditionsWidget() {
    cancel.store(true);
    if (worker) {worker->wait(); delete worker;}
}
void SolarConditionsWidget::refresh() {
    if (worker) return;
    cancel.store(false); refreshButton->setEnabled(false);
    status->setText(uk ? QString::fromUtf8(u8"Завантаження NOAA...") : "Downloading NOAA...");
    worker = QThread::create([this]() {
        solar::Observation observation = last;
        QString errorFlux, errorKp;
        const auto flux = fetch("/json/f107_cm_flux.json", cancel, &errorFlux);
        const auto kp = cancel.load() ? QByteArray() : fetch("/products/noaa-planetary-k-index.json", cancel, &errorKp);
        const bool fluxOkay = solar::parseFlux(flux, &observation);
        const bool kpOkay = solar::parseKp(kp, &observation);
        QMetaObject::invokeMethod(this, [this, observation, fluxOkay, kpOkay, errorFlux, errorKp]() {
            last = observation;
            auto number = [](double x) { return std::isfinite(x) ? QString::number(x, 'f', 1) : QStringLiteral("--"); };
            auto date = [](const QDateTime &d) { return d.isValid() ? d.toString("yyyy-MM-dd HH:mm 'UTC'") : QStringLiteral("--"); };
            values->setText(QStringLiteral("<h3>F10.7: %1 sfu</h3>%2<br>90 d: %3 sfu<br>%4<hr><h3>Kp: %5</h3>%6")
                .arg(number(last.flux), date(last.fluxUtc), number(last.flux90), date(last.flux90Utc),
                     number(last.kp), date(last.kpUtc)));
            const auto now = QDateTime::currentDateTimeUtc();
            const bool stale = !last.fluxUtc.isValid() || !last.kpUtc.isValid() ||
                               last.fluxUtc.secsTo(now) > 36*3600 || last.kpUtc.secsTo(now) > 6*3600;
            QString message = (fluxOkay && kpOkay) ? (uk ? QString::fromUtf8(u8"Оновлено: ") : "Fetched: ") + date(now) :
                (uk ? QString::fromUtf8(u8"Не всі дані оновлено; попередні значення збережені. ") : "Partial/failed refresh; previous observations retained. ") + errorFlux + " " + errorKp;
            if (stale) message += uk ? QString::fromUtf8(u8"\nУвага: дані застарілі або неповні.") : "\nWarning: stale or incomplete observations.";
            status->setText(message); refreshButton->setEnabled(true);
        }, Qt::QueuedConnection);
    });
    connect(worker, &QThread::finished, this, [this]() { auto *done = worker; worker = nullptr; done->deleteLater(); });
    worker->start(QThread::LowPriority);
}

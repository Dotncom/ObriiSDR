#include "publicdatafetch.h"
#include <QEventLoop>
#include <QElapsedTimer>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslSocket>
#include <QTimer>
#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#endif

namespace {
QString httpFailure(int status) {
    QString reason;
    if (status == 401 || status == 403) reason = QStringLiteral("; check API key and service access");
    else if (status == 429) reason = QStringLiteral("; request quota/rate limit exceeded");
    return QStringLiteral("HTTP %1%2").arg(status).arg(reason);
}
}

QByteArray fetchPublicData(const QUrl &url, const std::atomic_bool &cancel, QString *error, int *httpStatus) {
    constexpr int limit = 2 * 1024 * 1024;
    if (error) error->clear();
    if (httpStatus) *httpStatus = 0;
    if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty() ||
        (url.scheme() != QStringLiteral("https") && url.scheme() != QStringLiteral("http"))) {
        if (error) *error = QStringLiteral("Invalid public HTTP(S) source URL."); return {};
    }
    if (cancel) { if (error) *error = QStringLiteral("Request cancelled"); return {}; }
#ifdef Q_OS_WIN
    if (!QSslSocket::supportsSsl()) {
        HINTERNET session = WinHttpOpen(L"ObriiSDR-PublicData/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) { if (error) *error = QStringLiteral("WinHTTP session error"); return {}; }
        WinHttpSetTimeouts(session, 2000, 2000, 2000, 2000);
        QByteArray result;
        QUrl current = url;
        bool okay = false;
        bool tooLarge = false;
        DWORD lastStatus = 0, networkError = 0;
        QElapsedTimer deadline; deadline.start();
        for (int redirect = 0; redirect <= 3 && !cancel; ++redirect) {
            const QString host = current.host();
            const bool secure = current.scheme() == QStringLiteral("https");
            QString path = current.path(QUrl::FullyEncoded);
            if (path.isEmpty()) path = QStringLiteral("/");
            if (current.hasQuery()) path += '?' + current.query(QUrl::FullyEncoded);
            HINTERNET connection = WinHttpConnect(session, reinterpret_cast<LPCWSTR>(host.utf16()),
                static_cast<INTERNET_PORT>(current.port(secure ? 443 : 80)), 0);
            HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", reinterpret_cast<LPCWSTR>(path.utf16()),
                nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0) : nullptr;
            DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
            if (request) WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
            okay = request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(request, nullptr);
            DWORD code = 0, size = sizeof(code);
            okay = okay && WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX);
            if (!okay) networkError = GetLastError();
            lastStatus = code;
            if (httpStatus) *httpStatus = static_cast<int>(code);
            QUrl target;
            if (okay && (code == 301 || code == 302 || code == 303 || code == 307 || code == 308)) {
                wchar_t location[4096]; size = sizeof(location);
                if (WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, location, &size, WINHTTP_NO_HEADER_INDEX))
                    target = current.resolved(QUrl(QString::fromWCharArray(location)));
                okay = false;
            } else if (okay && code == 200) {
                while (!cancel && deadline.elapsed() < 12000) {
                    char chunk[8192]; DWORD count = 0;
                    if (!WinHttpReadData(request, chunk, sizeof(chunk), &count)) { networkError = GetLastError(); okay = false; break; }
                    if (!count) break;
                    if (result.size() + static_cast<int>(count) > limit) { tooLarge = true; okay = false; break; }
                    result.append(chunk, static_cast<int>(count));
                }
                if (deadline.elapsed() >= 12000) okay = false;
            } else okay = false;
            if (request) WinHttpCloseHandle(request);
            if (connection) WinHttpCloseHandle(connection);
            if (okay) break;
            if (!target.isValid() || !target.userInfo().isEmpty() || target.host().isEmpty() ||
                (target.scheme() != "https" && target.scheme() != "http") || (secure && target.scheme() != "https")) break;
            current = target;
        }
        WinHttpCloseHandle(session);
        if (!okay || cancel) {
            if (error) {
                if (cancel) *error = QStringLiteral("Request cancelled");
                else if (tooLarge) *error = QStringLiteral("Response exceeds the 2 MiB limit");
                else if (deadline.elapsed() >= 12000) *error = QStringLiteral("Request timed out");
                else if (lastStatus && lastStatus != 200) *error = httpFailure(static_cast<int>(lastStatus));
                else *error = QStringLiteral("Windows HTTPS/network error %1").arg(networkError);
            }
            return {};
        }
        return result;
    }
#endif
    QNetworkAccessManager manager;
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setMaximumRedirectsAllowed(3);
    request.setRawHeader("User-Agent", "ObriiSDR-PublicData/1.0");
    auto *reply = manager.get(request);
    QEventLoop loop; QTimer timeout, cancellation;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, reply, &QNetworkReply::abort);
    QObject::connect(&cancellation, &QTimer::timeout, reply, [&] { if (cancel) reply->abort(); });
    QObject::connect(reply, &QNetworkReply::downloadProgress, reply, [reply, limit](qint64 count, qint64 total) { if (count > limit || total > limit) reply->abort(); });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timeout.start(12000); cancellation.start(100); loop.exec();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (httpStatus) *httpStatus = status;
    const QByteArray result = reply->readAll();
    if (reply->error() != QNetworkReply::NoError || result.size() > limit || cancel) {
        if (error) {
            if (cancel) *error = QStringLiteral("Request cancelled");
            else if (!timeout.isActive()) *error = QStringLiteral("Request timed out");
            else if (status && status != 200) *error = httpFailure(status);
            else if (url.scheme() == "https" && !QSslSocket::supportsSsl()) *error = QStringLiteral("TLS unavailable; install a compatible OpenSSL runtime");
            else *error = QStringLiteral("Qt network error %1 (or response limit exceeded)").arg(int(reply->error()));
        }
        return {};
    }
    return result;
}

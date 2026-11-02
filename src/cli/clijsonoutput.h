#ifndef OBRII_CLI_JSON_OUTPUT_H
#define OBRII_CLI_JSON_OUTPUT_H

#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

// Direct callbacks enqueue here, never in Qt's unbounded event queue. A blocked
// stdout consumer cannot prevent producer/watchdog/backend cancellation.
class CliJsonOutput {
public:
    explicit CliJsonOutput(QString module) : module(std::move(module)), writer([this] { run(); }) {}
    ~CliJsonOutput() { finish(); }
    bool send(QJsonObject object) {
        object.insert(QStringLiteral("module"), module);
        object.insert(QStringLiteral("utc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        const QByteArray line = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
        std::lock_guard<std::mutex> lock(mutex);
        if (closing || failed.load()) return false;
        if (queue.size() >= 256 || queuedBytes + line.size() > 256 * 1024) {
            failed.store(true);
            abort.store(true);
        } else {
            queue.push_back(line);
            queuedBytes += line.size();
        }
        ready.notify_one();
        return !failed.load();
    }
    bool okay() const { return !failed.load(); }
    bool finish() {
        if (!writer.joinable()) return okay();
        {
            std::lock_guard<std::mutex> lock(mutex);
            closing = true;
        }
        ready.notify_one();
        QElapsedTimer timer;
        timer.start();
        while (!done.load()) {
            if (timer.elapsed() >= 2000) {
                failed.store(true);
                abort.store(true);
            }
#ifdef Q_OS_WIN
            // Repeat to cover cancellation racing with entry into WriteFile.
            if (abort.load()) CancelSynchronousIo(writer.native_handle());
#endif
            QThread::msleep(10);
        }
        writer.join();
        return okay();
    }
private:
    bool writeLine(const QByteArray &line) {
        qint64 offset = 0;
        while (offset < line.size() && !abort.load()) {
#ifdef Q_OS_WIN
            DWORD written = 0;
            if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line.constData() + offset,
                           static_cast<DWORD>(line.size() - offset), &written, nullptr) || !written)
                return false;
            offset += written;
#else
            pollfd descriptor{STDOUT_FILENO, POLLOUT, 0};
            const int result = ::poll(&descriptor, 1, 50);
            if (result < 0 && errno == EINTR) continue;
            if (result < 0 || (result > 0 && (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL))))
                return false;
            if (!result) continue;
            const ssize_t written = ::write(STDOUT_FILENO, line.constData() + offset,
                                            static_cast<std::size_t>(line.size() - offset));
            if (written < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            if (written <= 0) return false;
            offset += written;
#endif
        }
        return offset == line.size();
    }
    void run() {
#ifndef Q_OS_WIN
        const int flags = fcntl(STDOUT_FILENO, F_GETFL);
        if (flags < 0 || fcntl(STDOUT_FILENO, F_SETFL, flags | O_NONBLOCK) < 0) {
            failed.store(true);
            done.store(true);
            return;
        }
#endif
        for (;;) {
            QByteArray line;
            {
                std::unique_lock<std::mutex> lock(mutex);
                ready.wait(lock, [this] { return abort.load() || closing || !queue.empty(); });
                if (abort.load() || (closing && queue.empty())) break;
                line = std::move(queue.front());
                queuedBytes -= line.size();
                queue.pop_front();
            }
            if (!writeLine(line)) { failed.store(true); break; }
        }
#ifndef Q_OS_WIN
        fcntl(STDOUT_FILENO, F_SETFL, flags);
#endif
        done.store(true);
    }
    const QString module;
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<QByteArray> queue;
    qint64 queuedBytes = 0;
    bool closing = false;
    std::atomic_bool failed{false}, abort{false}, done{false};
    std::thread writer;
};

#endif

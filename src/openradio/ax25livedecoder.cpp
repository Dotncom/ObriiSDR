#include "ax25livedecoder.h"

#include "ax25afsk1200.h"

#include <QDateTime>
#include <QMutexLocker>
#include <QtEndian>

#include <algorithm>

Ax25LiveDecoder::Ax25LiveDecoder(QObject *parent) : QThread(parent) {}

Ax25LiveDecoder::~Ax25LiveDecoder() { stop(); }

void Ax25LiveDecoder::submitPcm16(const QByteArray &pcm, int sampleRate) {
    if (pcm.size() < 2 || sampleRate < 12000 || sampleRate > 192000 ||
        sampleRate % 1200 != 0) return;
    QMutexLocker lock(&mutex);
    if (stopping) return;
    if (rate != sampleRate) {
        rate = sampleRate;
        bufferedPcm.clear();
        totalSamples = processedSamples = 0;
        recentlyDecoded.clear();
    }
    const int alignedBytes = pcm.size() & ~1;
    bufferedPcm.append(pcm.constData(), alignedBytes);
    totalSamples += alignedBytes / 2;
    const int maximumBytes = rate * 3 * 2;
    if (bufferedPcm.size() > maximumBytes)
        bufferedPcm.remove(0, (bufferedPcm.size() - maximumBytes) & ~1);
    if (bufferedPcm.size() >= rate && totalSamples - processedSamples >= rate / 10)
        ready.wakeOne();
}

void Ax25LiveDecoder::stop() {
    {
        QMutexLocker lock(&mutex);
        stopping = true;
        ready.wakeAll();
    }
    if (isRunning()) wait();
}

void Ax25LiveDecoder::run() {
    for (;;) {
        QByteArray pcm;
        int sampleRate = 0;
        {
            QMutexLocker lock(&mutex);
            while (!stopping && (bufferedPcm.size() < rate ||
                                  totalSamples - processedSamples < rate / 10))
                ready.wait(&mutex);
            if (stopping) return;
            sampleRate = rate;
            pcm = bufferedPcm.right((std::min)(bufferedPcm.size(), rate * 2 * 2));
            processedSamples = totalSamples;
        }
        QVector<float> audio(pcm.size() / 2);
        for (int i = 0; i < audio.size(); ++i)
            audio[i] = static_cast<qint16>(qFromLittleEndian<quint16>(
                reinterpret_cast<const uchar *>(pcm.constData() + i * 2))) / 32768.0f;
        const QList<QByteArray> frames = Ax25Afsk1200::demodulate(audio, sampleRate);
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        for (const QByteArray &frame : frames) {
            if (recentlyDecoded.value(frame, 0) > now - 3000) continue;
            recentlyDecoded.insert(frame, now);
            if (frame.size() > 2) emit frameDecoded(frame.left(frame.size() - 2));
        }
        for (auto it = recentlyDecoded.begin(); it != recentlyDecoded.end(); ) {
            if (it.value() < now - 3000) it = recentlyDecoded.erase(it);
            else ++it;
        }
    }
}

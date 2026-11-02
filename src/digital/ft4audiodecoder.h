#pragma once
#include <QByteArray>
#include <QStringList>
#include <QHash>
#include <vector>

// Thread-confined, bounded rolling FT4 receiver. Input: mono signed PCM16 LE.
class Ft4AudioDecoder {
public:
    void reset();
    QStringList process(const QByteArray &pcm, int sampleRate);
private:
    QStringList analyze();
    std::vector<float> audio;
    QHash<QString, qint64> recent;
    qint64 samples = 0;
    int sinceAnalysis = 0, inputRate = 0;
    double phase = 0.0, sum = 0.0;
    int count = 0;
};

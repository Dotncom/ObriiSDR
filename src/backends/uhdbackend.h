#ifndef UHD_BACKEND_H
#define UHD_BACKEND_H

#include <QString>
#include <QStringList>
#include <cstddef>

QStringList enumerateUhdDeviceArgs(QString *errorMessage = nullptr);

class UhdRxSession {
public:
    ~UhdRxSession();
    bool open(int deviceIndex, int channel, double sampleRateHz, double centerHz,
              double gainDb, const QString &antenna, const QString &fpgaImagePath,
              QString *errorMessage);
    bool retune(double centerHz, QString *errorMessage);
    int receive(float *interleavedIq, std::size_t capacity, QString *errorMessage);
    void close();

private:
    void *device_ = nullptr;
    void *streamer_ = nullptr;
    void *metadata_ = nullptr;
    int channel_ = 0;
};

#endif // UHD_BACKEND_H

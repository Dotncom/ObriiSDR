#pragma once
#include <QJsonObject>
#include <QByteArray>
#include <QList>
#include <QVector>
#include <complex>
#include <memory>

namespace Mesh {
// Byte/IQ conversion only. Native protocol addressing/routing stays above PHY.
class WireModem {
public:
    explicit WireModem(const QJsonObject &configuration={});
    ~WireModem();
    WireModem(WireModem &&) noexcept;
    WireModem &operator=(WireModem &&) noexcept;
    static bool normalize(const QJsonObject &,QJsonObject *,QString *error);
    int sampleRate() const;
    int maximumWireBytes() const;
    quint64 estimatedSamples(int wireBytes) const;
    QVector<std::complex<float>> modulate(const QByteArray &wire,QString *error);
    QList<QByteArray> receive(const float *interleaved,int count,bool discontinuity=false);
    void reset();
    QJsonObject status() const;
private:
    struct Private;
    std::unique_ptr<Private> d;
};
}

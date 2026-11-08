#pragma once

#include "kisscodec.h"
#include "transmit/virtualtransmitterbackend.h"

#include <QElapsedTimer>
#include <QQueue>
#include <QTimer>

class KissVirtualTx final : public QObject {
    Q_OBJECT
public:
    explicit KissVirtualTx(QObject *parent = nullptr);
    ~KissVirtualTx() override;
    void enable(const QString &channel, double centerHz);
    void disable();
    bool enqueue(const KissFrame &frame, QString *error);

signals:
    void sent(int frameBytes);
    void failed(const QString &error);

private:
    void pump();
    void abortBurst(const QString &error);

    VirtualTransmitterBackend transmitter;
    TxConfiguration configuration;
    QTimer timer;
    QElapsedTimer burstTime;
    QQueue<QByteArray> pending;
    QVector<std::complex<float>> burst;
    int offset = 0;
    int frameBytes = 0;
    bool enabled = false;
};

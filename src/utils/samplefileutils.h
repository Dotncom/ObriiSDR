#ifndef SAMPLEFILEUTILS_H
#define SAMPLEFILEUTILS_H

#include <QByteArray>
#include <QtGlobal>
#include <vector>

void appendLe32(QByteArray &buffer, quint32 value);
void appendLe16(QByteArray &buffer, quint16 value);
quint32 readLe32(const char *data);
qint16 readLeI16(const char *data);
void appendIqS16LeToFloat(const QByteArray &iqData, int sampleCount, std::vector<float> &output);
void appendIqS8ToFloat(const QByteArray &iqData, int sampleCount, std::vector<float> &output);
QByteArray streamingWavHeader();
qint16 pcm16FromFloat(float sample);
QByteArray fixedPcm16StereoWavHeader(int sampleRate, quint32 dataBytes);

#endif // SAMPLEFILEUTILS_H

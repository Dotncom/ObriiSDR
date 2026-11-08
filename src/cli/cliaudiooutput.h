#ifndef CLIAUDIOOUTPUT_H
#define CLIAUDIOOUTPUT_H

#include "audioprocessor.h"
#include "iqchannelizer.h"

#include <QFile>
#include <QString>

#include <functional>
#include <vector>

class CliAudioOutput {
public:
    bool open(const QString &path, bool toStdout, int inputMode, int modulation, double bandwidthHz,
              double sourceRateHz, double centerHz, double listenHz, bool followCenter,
              QString *error, std::function<void(const QByteArray &)> frameCallback = {});
    bool process(const float *iq, std::size_t complexSamples, QString *error,
                 bool decodePcm = true);
    bool close(QString *error);
    void setCenter(double centerHz);
    void setListening(double listenHz);
    void setModulation(int modulation);
    void setBandwidth(double bandwidthHz);
    void setFilterChain(const QString &json);
    void setSampleRate(double sourceRateHz);
    void setChannelIqCallback(std::function<void(const float *, int, double)> callback);
    double listeningHz() const { return sourceSettings.listeningFrequency; }
    bool followsReceiverCenter() const { return followsCenter; }
    bool enabled() const { return outputFile.isOpen() || standardOutput || static_cast<bool>(onFrame); }

private:
    bool writeWaveHeader(QString *error);
    QFile outputFile;
    bool standardOutput = false;
    bool followsCenter = true;
    bool discontinuity = true;
    quint64 audioBytes = 0;
    RadioSettings sourceSettings;
    IqChannelizer channelizer;
    AudioProcessor demodulator;
    std::vector<float> channelIq;
    std::vector<short> pcm;
    std::function<void(const QByteArray &)> onFrame;
    std::function<void(const float *, int, double)> onChannelIq;
};

#endif

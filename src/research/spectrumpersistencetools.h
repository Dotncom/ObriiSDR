#ifndef SPECTRUMPERSISTENCETOOLS_H
#define SPECTRUMPERSISTENCETOOLS_H

#include <QWidget>
#include <QString>

#include <functional>
#include <memory>
#include <vector>

class SignalDensityWidget : public QWidget {
public:
    using Translator = std::function<QString(const QString &, const QString &)>;

    explicit SignalDensityWidget(Translator translator,
                                 QWidget *parent = nullptr,
                                 QString settingsGroup = QStringLiteral("researchDensity"));
    ~SignalDensityWidget() override;

    void appendSpectrumFrame(const std::vector<float> &frequencies,
                             const std::vector<float> &levels,
                             int amplitudeUnit,
                             double cursorFrequencyHz);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

class SpectrumMaskWidget : public QWidget {
public:
    using Translator = std::function<QString(const QString &, const QString &)>;
    using TriggerHandler = std::function<void()>;

    explicit SpectrumMaskWidget(Translator translator,
                                TriggerHandler triggerHandler,
                                QWidget *parent = nullptr);
    ~SpectrumMaskWidget() override;

    void appendSpectrumFrame(const std::vector<float> &frequencies,
                             const std::vector<float> &levels,
                             int amplitudeUnit);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

#endif // SPECTRUMPERSISTENCETOOLS_H

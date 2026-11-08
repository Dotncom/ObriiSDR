#ifndef ZOOMDENSITYDIALOG_H
#define ZOOMDENSITYDIALOG_H

#include <QDialog>

#include <functional>
#include <vector>

class QLabel;
class SignalDensityWidget;

class ZoomDensityDialog : public QDialog {
public:
    using Translator = std::function<QString(const QString &, const QString &)>;

    explicit ZoomDensityDialog(Translator translator, QWidget *parent = nullptr);

    void setRange(double lowHz, double highHz);
    void appendSpectrumFrame(const std::vector<float> &frequencies,
                             const std::vector<float> &levels,
                             int amplitudeUnit,
                             double cursorFrequencyHz);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    static QString formatFrequency(double frequencyHz);

    Translator translator;
    QLabel *rangeLabel = nullptr;
    SignalDensityWidget *density = nullptr;
    double lowHz = 0.0;
    double highHz = 0.0;
    std::vector<float> croppedFrequencies;
    std::vector<float> croppedLevels;
};

#endif // ZOOMDENSITYDIALOG_H

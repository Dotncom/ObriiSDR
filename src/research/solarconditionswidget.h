#ifndef SOLARCONDITIONSWIDGET_H
#define SOLARCONDITIONSWIDGET_H

#include <QDateTime>
#include <QWidget>
#include <atomic>
#include <limits>
class QLabel;
class QPushButton;
class QThread;
namespace solar {
struct Observation {
    double flux = std::numeric_limits<double>::quiet_NaN();
    double flux90 = std::numeric_limits<double>::quiet_NaN();
    double kp = std::numeric_limits<double>::quiet_NaN();
    double ap = std::numeric_limits<double>::quiet_NaN();
    QDateTime fluxUtc, flux90Utc, kpUtc;
};
bool parseFlux(const QByteArray &json, Observation *out);
bool parseKp(const QByteArray &json, Observation *out);
}
class SolarConditionsWidget : public QWidget {
    Q_OBJECT
public:
    explicit SolarConditionsWidget(bool ukrainian, QWidget *parent = nullptr);
    ~SolarConditionsWidget() override;
private:
    void refresh();
    bool uk;
    std::atomic_bool cancel{false};
    QThread *worker = nullptr;
    solar::Observation last;
    QLabel *values = nullptr, *status = nullptr;
    QPushButton *refreshButton = nullptr;
};
#endif

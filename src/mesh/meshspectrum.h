#pragma once
#include <QJsonObject>
#include <QVector>
#include <QWidget>

namespace Mesh {
// Display adapter only: FFT bins come from the node's existing CLI worker.
class SpectrumView : public QWidget {
    Q_OBJECT
public:
    explicit SpectrumView(QWidget *parent=nullptr);
    bool setTrace(const QJsonObject &trace);
    void clear(const QString &message);
    void setRange(double minimum,double maximum);
    void setListener(double frequencyHz,double bandwidthHz);
    int binCount() const { return levels.size(); }
protected:
    void paintEvent(QPaintEvent *) override;
private:
    QVector<double> levels;
    double firstHz=0,binHz=1,minimumDb=-140,maximumDb=0;
    double listenerHz=0,bandwidthHz=0;
    QString message;
};
}

#ifndef HF_PREDICTION_DIALOG_H
#define HF_PREDICTION_DIALOG_H
#include "hfprediction.h"
#include <QDialog>
#include <functional>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTabWidget;

class HfPredictionDialog final : public QDialog {
    Q_OBJECT
public:
    using Translation = std::function<QString(const QString &key, const QString &fallback)>;
    explicit HfPredictionDialog(QWidget *parent = nullptr);
    HfPredictionDialog(QWidget *parent, Translation translation);
    ~HfPredictionDialog() override;
    bool setLink(hf::Point tx, hf::Point rx, double powerW, double txGainDbi, double rxGainDbi);
    bool setRequest(const hf::Request &request);
    hf::Request request() const;
    void setRuntime(const hf::Runtime &runtime);
    hf::Runtime runtime() const;
signals:
    void resultReady(const hf::Result &result);
protected:
    void closeEvent(QCloseEvent *event) override;
private:
    QString uiText(const char *key, const char *fallback) const;
    void invalidate();
    void calculate();
    void render();
    void exportCsv();
    void setBusy(bool busy);
    void applyRequest(const hf::Request &request);
    Translation translate;
    hf::PredictionJob job;
    hf::Result result;
    bool loading = false;
    int timeoutMs = 120000;
    QVector<int> selectedHours;
    QDoubleSpinBox *txLat, *txLon, *rxLat, *rxLon, *power, *txGain, *rxGain, *bandwidth, *snr;
    QSpinBox *year, *month, *ssn, *percent;
    QComboBox *noise, *metric;
    QCheckBox *longPath;
    QLineEdit *frequencies, *helper, *data;
    QTabWidget *inputs;
    QTableWidget *table;
    QLabel *status;
    QPushButton *run, *stop, *exportButton;
};
#endif

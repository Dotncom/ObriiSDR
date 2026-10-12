#ifndef GNSSSDRPVTDIALOG_H
#define GNSSSDRPVTDIALOG_H
#include "gnsssdrstream.h"
#include <QDialog>
#include <QPointer>
#include <functional>
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLineEdit;
class QLabel;
class QPushButton;
class QPlainTextEdit;
class QProcess;
class GnssSdrPvtDialog : public QDialog {
    Q_OBJECT
public:
    using Translator = std::function<QString(const QString &, const QString &)>;
    GnssSdrPvtDialog(const Translator &translate, QWidget *parent = nullptr);
    ~GnssSdrPvtDialog() override;
    void setReceiverContext(bool running, bool allowed, double sampleRateHz, double centerHz);
    void stop();
signals:
    void mapSourceStarted();
    void positionSentence(const QString &line);
protected:
    void closeEvent(QCloseEvent *event) override;
private:
    void start();
    void appendLog(const QString &message);
    Translator text;
    QComboBox *source, *format;
    QLineEdit *path;
    QDoubleSpinBox *sampleRate, *center;
    QList<QCheckBox *> systems;
    QCheckBox *map;
    QLabel *status;
    QPushButton *startButton, *stopButton;
    QPlainTextEdit *log;
    QProcess *process;
    QPointer<GnssSdrStream> stream;
    bool receiverRunning = false, receiverAllowed = false;
    double receiverRate = 0, receiverCenter = 0;
    GnssSdr::Input activeInput;
    int generation = 0;
    bool stoppingProcess = false;
};
#endif

#ifndef LRPTDECODEDIALOG_H
#define LRPTDECODEDIALOG_H
#include <QDialog>
#include <QPointer>
#include <functional>
class QComboBox;
class QLineEdit;
class QDoubleSpinBox;
class QCheckBox;
class QPushButton;
class QLabel;
class QPlainTextEdit;
class QProcess;
class QThread;
class LrptDecodeDialog : public QDialog {
    Q_OBJECT
public:
    using Translator = std::function<QString(const QString &, const QString &)>;
    LrptDecodeDialog(const Translator &translate, QWidget *parent = nullptr);
    ~LrptDecodeDialog() override;
    void stop();
protected:
    void closeEvent(QCloseEvent *event) override;
private:
    void start();
    void loadResults();
    void preview();
    Translator text;
    QLineEdit *input, *output;
    QComboBox *pipeline, *format, *images;
    QDoubleSpinBox *sampleRate;
    QCheckBox *dcBlock, *iqSwap;
    QPushButton *startButton, *stopButton;
    QLabel *image, *status;
    QPlainTextEdit *log;
    QProcess *process;
    QPointer<QThread> preparation;
    QString resultDirectory;
    int generation = 0;
};
#endif

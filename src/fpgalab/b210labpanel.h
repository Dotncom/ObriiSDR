#pragma once

#include <QByteArray>
#include <QFile>
#include <QProcess>
#include <QWidget>
#include <functional>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTemporaryFile;

class B210LabPanel final : public QWidget {
public:
    explicit B210LabPanel(std::function<QByteArray(QString *)> pipeline,
                         std::function<void(const QString &)> openProject,
                         std::function<bool()> prepareBuild, QWidget *parent);
    void setOpenPath(const QString &folder, const QString &image);
    bool busy() const { return process_.state() != QProcess::NotRunning; }

private:
    enum class Job { Create, Build, Test };
    void startBuild(bool create);
    void startTest();
    void start(Job job, const QString &program, const QStringList &arguments);
    void finish(int exitCode, QProcess::ExitStatus status);
    void sendImage();
    void setBusy(bool active);
    void log(const QString &line);

    std::function<QByteArray(QString *)> pipeline_;
    std::function<void(const QString &)> openProject_;
    std::function<bool()> prepareBuild_;
    QProcess process_;
    QFile image_;
    QTemporaryFile *rtlFile_;
    QByteArray checkScript_;
    QByteArray result_;
    Job job_ = Job::Create;
    QString outputPath_;
    QString variantName_;
    QString projectPath_;
    QLineEdit *recipe_;
    QLineEdit *vendor_;
    QLineEdit *environment_;
    QComboBox *variant_;
    QCheckBox *passthrough_;
    QLineEdit *host_;
    QLineEdit *cli_;
    QLineEdit *baseline_;
    QLineEdit *candidate_;
    QLineEdit *library_;
    QSpinBox *seconds_;
    QDoubleSpinBox *center_;
    QCheckBox *gnss_;
    QCheckBox *pps_;
    QPlainTextEdit *log_;
    QPushButton *create_;
    QPushButton *build_;
    QPushButton *test_;
};

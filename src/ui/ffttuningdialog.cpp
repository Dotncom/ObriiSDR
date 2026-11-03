#include "ffttuningdialog.h"
#include "../core/fftplanprofile.h"
#include "../core/ffttuningguard.h"
#include "../core/computebudgetmanager.h"
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QFormLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QLabel>
#include <QProcess>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <QUuid>
#include <algorithm>

void showFftTuningDialog(QWidget *parent,int length,bool uk,const std::function<bool()> &receiverBusy) {
    const auto text=[uk](const char *en,const char *ua){return QString::fromUtf8(uk?ua:en);};
    QDialog dialog(parent);dialog.setWindowTitle(text("FFT tuning","Автопідбір FFT"));dialog.setWindowModality(Qt::ApplicationModal);dialog.resize(620,470);
    QVBoxLayout layout(&dialog);QFormLayout form;layout.addLayout(&form);
    QComboBox sizes;sizes.setObjectName("fftTuneSize");for(int n=512;n<=67108864;n*=2)sizes.addItem(QString::number(n),n);
    if(sizes.findData(length)<0)sizes.addItem(QString::number(length),length);sizes.setCurrentIndex(sizes.findData(length));
    const int threadLimit=qMax(1,(std::min)({16,QThread::idealThreadCount(),ComputeBudgetManager::instance().budget().totalWorkerThreads}));
    QSpinBox threads;threads.setRange(1,threadLimit);
    threads.setValue(threadLimit);
    QSpinBox planning;planning.setRange(100,5000);planning.setSingleStep(100);planning.setValue(500);planning.setSuffix(" ms");
    form.addRow(text("FFT size","Розмір FFT"),&sizes);form.addRow(text("Maximum threads","Максимум потоків"),&threads);form.addRow(text("Planning budget / candidate","Час планування / кандидат"),&planning);
    QLabel status;status.setWordWrap(true);layout.addWidget(&status);
    QTableWidget table(0,4);table.setHorizontalHeaderLabels({text("Threads","Потоки"),text("Plan","План"),"ms",text("Valid","Коректний")});table.horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);table.setEditTriggers(QAbstractItemView::NoEditTriggers);layout.addWidget(&table);
    QDialogButtonBox buttons;auto start=buttons.addButton(text("Tune","Підібрати"),QDialogButtonBox::ActionRole);auto clear=buttons.addButton(text("Clear profile","Видалити профіль"),QDialogButtonBox::ActionRole);auto close=buttons.addButton(QDialogButtonBox::Close);layout.addWidget(&buttons);
    start->setObjectName("fftTuneStart");clear->setObjectName("fftTuneClear");
    close->setText(text("Close","Закрити"));
    QProcess process;QByteArray buffer;bool active=false,complete=false,cancelRequested=false;QString pendingPath;int tuningLength=0;
    const auto cached=[&]{const auto p=loadFftPlanProfile(sizes.currentData().toInt(),threadLimit);status.setText(p.threads?text("Saved: %1 threads, %2 ms","Збережено: %1 потоків, %2 мс").arg(p.threads).arg(p.medianMs,0,'f',2):text("No compatible saved profile","Сумісного збереженого профілю немає"));};
    bool previouslyBusy=false;
    const auto refresh=[&]{const bool busy=receiverBusy()||(!active&&fftTuningActive());start->setEnabled(!active&&!busy);clear->setEnabled(!active&&!busy);sizes.setEnabled(!active);threads.setEnabled(!active);planning.setEnabled(!active);
        if(!active&&busy)status.setText(text("Stop all receivers and wait for the FFT worker.","Зупиніть усі приймачі та дочекайтеся FFT."));
        else if(!active&&previouslyBusy)cached();previouslyBusy=busy;
    };
    QObject::connect(&sizes,QOverload<int>::of(&QComboBox::currentIndexChanged),&dialog,[&](int){cached();refresh();});
    QObject::connect(clear,&QPushButton::clicked,&dialog,[&]{if(!receiverBusy()&&removeFftPlanProfile(sizes.currentData().toInt()))cached();});
    QObject::connect(start,&QPushButton::clicked,&dialog,[&]{
        if(active||receiverBusy())return;bool expected=false;if(!fftTuningGuard().compare_exchange_strong(expected,true))return;
        active=true;complete=false;cancelRequested=false;buffer.clear();table.setRowCount(0);status.setText(text("Benchmarking...","Тестування..."));close->setText(text("Cancel","Скасувати"));refresh();
        tuningLength=sizes.currentData().toInt();pendingPath=fftPlanProfilePath(tuningLength)+"."+QUuid::createUuid().toString(QUuid::WithoutBraces)+".pending";
        const QString exe=QDir(QApplication::applicationDirPath()).filePath(
#ifdef _WIN32
            "ObriiCli.exe"
#else
            "ObriiCli"
#endif
        );
        process.start(exe,{"fft-tune","--fft",sizes.currentData().toString(),"--max-threads",QString::number(threads.value()),"--planning-ms",QString::number(planning.value()),"--pending-profile",pendingPath});
    });
    const auto readOutput=[&]{
        buffer+=process.readAllStandardOutput();if(buffer.size()>512*1024){process.kill();return;}
        while(buffer.contains('\n')){const int end=buffer.indexOf('\n');const auto doc=QJsonDocument::fromJson(buffer.left(end));buffer.remove(0,end+1);const auto o=doc.object();
            if(o.value("event")=="candidate"){const int row=table.rowCount();table.insertRow(row);const QStringList values{QString::number(o.value("threads").toInt()),o.value("strategy").toString(),QString::number(o.value("medianMs").toDouble(),'f',2),o.value("valid").toBool()?"+":"-"};for(int c=0;c<4;++c)table.setItem(row,c,new QTableWidgetItem(values[c]));}
            if(o.value("event")=="complete"&&o.value("ok").toBool()&&o.value("profile").toString()==pendingPath)complete=true;
            if(o.contains("error"))status.setText(o.value("error").toString());
        }
    };
    QObject::connect(&process,&QProcess::readyReadStandardOutput,&dialog,readOutput);
    QObject::connect(&process,QOverload<int,QProcess::ExitStatus>::of(&QProcess::finished),&dialog,[&](int code,QProcess::ExitStatus){
        readOutput();const bool published=code==0&&complete&&!cancelRequested&&publishFftPlanProfile(pendingPath,tuningLength);
        QFile::remove(pendingPath);active=false;fftTuningGuard().store(false);close->setText(text("Close","Закрити"));
        if(published)cached();else status.setText(text("Tuning stopped; existing profile retained.","Підбір зупинено; попередній профіль збережено."));refresh();
    });
    QObject::connect(&process,&QProcess::errorOccurred,&dialog,[&](QProcess::ProcessError error){if(error==QProcess::FailedToStart){active=false;fftTuningGuard().store(false);status.setText(process.errorString());refresh();}});
    QObject::connect(close,&QPushButton::clicked,&dialog,[&]{if(active){cancelRequested=true;process.kill();}else dialog.reject();});
    QTimer timer;timer.setInterval(250);QObject::connect(&timer,&QTimer::timeout,&dialog,refresh);timer.start();cached();refresh();dialog.exec();
    cancelRequested=true;if(process.state()!=QProcess::NotRunning){process.kill();process.waitForFinished(3000);}if(active)fftTuningGuard().store(false);if(!pendingPath.isEmpty())QFile::remove(pendingPath);
}

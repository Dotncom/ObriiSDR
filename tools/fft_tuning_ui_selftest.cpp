#include "ffttuningdialog.h"
#include "ffttuningguard.h"
#include <QApplication>
#include <QDialog>
#include <QPushButton>
#include <QTimer>
#include <cstdio>

int main(int argc,char **argv){
    QApplication app(argc,argv);bool busy=true,blocked=false,enabled=false;int ticks=0;
    QTimer timer;timer.setInterval(150);
    QObject::connect(&timer,&QTimer::timeout,[&]{
        for(auto widget:QApplication::topLevelWidgets())if(auto dialog=qobject_cast<QDialog *>(widget)){
            const auto start=dialog->findChild<QPushButton *>("fftTuneStart");if(!start)continue;
            if(++ticks==1){blocked=!start->isEnabled();busy=false;}
            if(ticks>=4){enabled=start->isEnabled();if(argc>1)dialog->grab().save(QString::fromLocal8Bit(argv[1]));dialog->reject();}
        }
    });timer.start();
    showFftTuningDialog(nullptr,8388608,true,[&]{return busy;});
    if(!blocked||!enabled){std::puts("FFT tuner UI stopped-RX gate FAIL");return 1;}
    timer.stop();fftTuningGuard().store(true);bool secondBlocked=false;
    QTimer::singleShot(500,[&]{for(auto widget:QApplication::topLevelWidgets())if(auto dialog=qobject_cast<QDialog *>(widget)){
        auto start=dialog->findChild<QPushButton *>("fftTuneStart");if(start){secondBlocked=!start->isEnabled();dialog->reject();}
    }});
    showFftTuningDialog(nullptr,1024,true,[]{return false;});
    const bool ownerPreserved=fftTuningActive();fftTuningGuard().store(false);
    if(!secondBlocked||!ownerPreserved){std::puts("FFT tuner guard ownership FAIL");return 1;}
    std::puts("FFT tuner UI stopped-RX gate and responsive polling PASS; no RX or tuner launched");return 0;
}

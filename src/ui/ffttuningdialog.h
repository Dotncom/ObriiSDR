#pragma once
#include <functional>
class QWidget;
void showFftTuningDialog(QWidget *parent, int length, bool ukrainian,
                         const std::function<bool()> &receiverBusy);

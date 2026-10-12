#ifndef HDF5SPECTRUMEXPORTUI_H
#define HDF5SPECTRUMEXPORTUI_H
#include <QString>
#include <functional>
class QWidget;
void showSpectrumHdf5Export(QWidget *parent, const std::function<QString(const QString &, const QString &)> &translate);
#endif

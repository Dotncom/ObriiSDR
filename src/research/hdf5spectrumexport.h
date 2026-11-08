#ifndef HDF5SPECTRUMEXPORT_H
#define HDF5SPECTRUMEXPORT_H
#include <QString>
#include <atomic>
#include <functional>

bool exportSpectrumHdf5(const QString &inputPath, const QString &outputPath,
                        QString *error, const std::atomic_bool *cancel = nullptr,
                        const std::function<void(int)> &progress = {});
bool hdf5SpectrumExportBuilt();
#endif

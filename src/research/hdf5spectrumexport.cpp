#include "hdf5spectrumexport.h"
#include "spectrumframerecorder.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLibrary>
#include <QTemporaryFile>
#include <algorithm>
#include <mutex>
#ifdef OBRII_HAS_HDF5_HEADERS
#include <hdf5.h>
#endif

bool hdf5SpectrumExportBuilt() {
#ifdef OBRII_HAS_HDF5_HEADERS
    return true;
#else
    return false;
#endif
}

#ifdef OBRII_HAS_HDF5_HEADERS
namespace {
struct HdfApi {
    QLibrary library;
#define HDF_FN(name) decltype(&::name) name = nullptr
    HDF_FN(H5open); HDF_FN(H5Fcreate); HDF_FN(H5Fclose); HDF_FN(H5Fflush);
    HDF_FN(H5Screate_simple); HDF_FN(H5Sclose); HDF_FN(H5Sselect_hyperslab);
    HDF_FN(H5Dcreate2); HDF_FN(H5Dwrite); HDF_FN(H5Dclose); HDF_FN(H5Dset_extent); HDF_FN(H5Dget_space);
    HDF_FN(H5Pcreate); HDF_FN(H5Pclose); HDF_FN(H5Pset_chunk);
    HDF_FN(H5Gcreate2); HDF_FN(H5Gclose); HDF_FN(H5Eget_auto2); HDF_FN(H5Eset_auto2);
#undef HDF_FN
    hid_t floatType = -1, doubleType = -1, byteType = -1, datasetClass = -1;
    bool load(QString *error) {
        QStringList candidates;
        const QString overridePath = qEnvironmentVariable("OBRII_HDF5_LIBRARY");
        if (!overridePath.isEmpty()) candidates << overridePath;
        candidates << QDir(QCoreApplication::applicationDirPath()).filePath("hdf5/hdf5")
                   << "hdf5_serial" << "hdf5";
        for (const auto &path : candidates) {
            library.setFileName(path);
            if (library.load()) break;
        }
        if (!library.isLoaded()) { if (error) *error = QStringLiteral("HDF5 runtime not found: %1").arg(library.errorString()); return false; }
#define HDF_LOAD(name) name = reinterpret_cast<decltype(name)>(library.resolve(#name)); if (!name) { if (error) *error = QStringLiteral("Missing HDF5 API: " #name); return false; }
        HDF_LOAD(H5open); HDF_LOAD(H5Fcreate); HDF_LOAD(H5Fclose); HDF_LOAD(H5Fflush);
        HDF_LOAD(H5Screate_simple); HDF_LOAD(H5Sclose); HDF_LOAD(H5Sselect_hyperslab);
        HDF_LOAD(H5Dcreate2); HDF_LOAD(H5Dwrite); HDF_LOAD(H5Dclose); HDF_LOAD(H5Dset_extent); HDF_LOAD(H5Dget_space);
        HDF_LOAD(H5Pcreate); HDF_LOAD(H5Pclose); HDF_LOAD(H5Pset_chunk);
        HDF_LOAD(H5Gcreate2); HDF_LOAD(H5Gclose); HDF_LOAD(H5Eget_auto2); HDF_LOAD(H5Eset_auto2);
#undef HDF_LOAD
        if (H5open() < 0) return false;
        const auto predefined = [this](const char *name) -> hid_t {
            const auto address = library.resolve(name);
            return address ? *reinterpret_cast<const hid_t *>(address) : -1;
        };
        floatType = predefined("H5T_NATIVE_FLOAT_g"); doubleType = predefined("H5T_NATIVE_DOUBLE_g");
        byteType = predefined("H5T_NATIVE_UCHAR_g"); datasetClass = predefined("H5P_CLS_DATASET_CREATE_ID_g");
        if (floatType < 0 || doubleType < 0 || byteType < 0 || datasetClass < 0) {
            if (error) *error = QStringLiteral("Unsupported HDF5 runtime type API"); return false;
        }
        return true;
    }
    hid_t createRows(hid_t group, const char *name, hid_t type, hsize_t width) {
        const hsize_t dimensions[2] = {0, width}, maximum[2] = {H5S_UNLIMITED, width}, chunk[2] = {1, width};
        const hid_t space = H5Screate_simple(2, dimensions, maximum);
        const hid_t properties = H5Pcreate(datasetClass);
        hid_t dataset = -1;
        if (space >= 0 && properties >= 0 && H5Pset_chunk(properties, 2, chunk) >= 0)
            dataset = H5Dcreate2(group, name, type, space, H5P_DEFAULT, properties, H5P_DEFAULT);
        if (properties >= 0) H5Pclose(properties);
        if (space >= 0) H5Sclose(space);
        return dataset;
    }
    bool appendRow(hid_t dataset, hid_t type, hsize_t row, hsize_t width, const void *data) {
        const hsize_t dimensions[2] = {row+1, width}, offset[2] = {row, 0}, count[2] = {1, width};
        if (H5Dset_extent(dataset, dimensions) < 0) return false;
        const hid_t fileSpace = H5Dget_space(dataset), memorySpace = H5Screate_simple(2, count, nullptr);
        const bool ok = fileSpace >= 0 && memorySpace >= 0 &&
            H5Sselect_hyperslab(fileSpace, H5S_SELECT_SET, offset, nullptr, count, nullptr) >= 0 &&
            H5Dwrite(dataset, type, memorySpace, fileSpace, H5P_DEFAULT, data) >= 0;
        if (memorySpace >= 0) H5Sclose(memorySpace);
        if (fileSpace >= 0) H5Sclose(fileSpace);
        return ok;
    }
};
}
#endif

bool exportSpectrumHdf5(const QString &inputPath, const QString &outputPath,
                        QString *error, const std::atomic_bool *cancel,
                        const std::function<void(int)> &progress) {
    if (error) error->clear();
#ifndef OBRII_HAS_HDF5_HEADERS
    Q_UNUSED(inputPath); Q_UNUSED(outputPath); Q_UNUSED(cancel); Q_UNUSED(progress);
    if (error) *error = QStringLiteral("HDF5 export is not built. Install libhdf5-dev and rebuild.");
    return false;
#else
    static std::mutex mutex;
    std::unique_lock<std::mutex> lock(mutex, std::try_to_lock);
    if (!lock.owns_lock()) { if (error) *error = QStringLiteral("Another HDF5 export is running"); return false; }
    if (QFileInfo(inputPath).absoluteFilePath() == QFileInfo(outputPath).absoluteFilePath() || QFileInfo::exists(outputPath)) {
        if (error) *error = QStringLiteral("Choose a new output file; existing files are not overwritten"); return false;
    }
    SpectrumFrameReader reader;
    if (!reader.open(inputPath, error)) return false;
    HdfApi h;
    if (!h.load(error)) return false;
    H5E_auto2_t previousHandler = nullptr; void *previousData = nullptr;
    h.H5Eget_auto2(H5E_DEFAULT, &previousHandler, &previousData);
    h.H5Eset_auto2(H5E_DEFAULT, nullptr, nullptr);
    QTemporaryFile temporary(QFileInfo(outputPath).absoluteDir().filePath(".obrii-hdf5-XXXXXX.part"));
    if (!temporary.open()) {
        if (error) *error = temporary.errorString();
        h.H5Eset_auto2(H5E_DEFAULT, previousHandler, previousData); return false;
    }
    const QString temporaryPath = temporary.fileName(); temporary.close();
    // The public H5F_ACC_TRUNC macro calls global HDF5 functions; this loader uses resolved APIs only.
    const hid_t file = h.H5Fcreate(temporaryPath.toUtf8().constData(), 0x0002u, H5P_DEFAULT, H5P_DEFAULT);
    bool ok = file >= 0;
    hid_t data = -1, parameters = -1;
    hsize_t width = 0, row = 0;
    int segment = 0, frames = 0;
    if (ok) {
        QJsonObject metadata = reader.metadata();
        metadata["hdf5Schema"] = "obrii-spectrum-v1";
        metadata["frameColumns"] = "utc_ms,elapsed_ms,center_hz,min_hz,max_hz,fft_length";
        metadata["spectrumOrder"] = "fftshift: N/2 rotation, matching Obrii spectrum replay";
        const QByteArray json = QJsonDocument(metadata).toJson(QJsonDocument::Compact);
        const hsize_t length = hsize_t(json.size());
        const hid_t space = h.H5Screate_simple(1, &length, nullptr);
        const hid_t ds = space >= 0 ? h.H5Dcreate2(file, "metadata_json_utf8", h.byteType, space, 0, 0, 0) : -1;
        ok = ds >= 0 && h.H5Dwrite(ds, h.byteType, H5S_ALL, H5S_ALL, 0, json.constData()) >= 0;
        if (ds >= 0) h.H5Dclose(ds); if (space >= 0) h.H5Sclose(space);
    }
    while (ok && !reader.atEnd()) {
        if (cancel && cancel->load()) { if (error) *error = QStringLiteral("Export cancelled"); ok = false; break; }
        SpectrumFrameRecord frame;
        if (!reader.readNext(frame, error)) { ok = false; break; }
        std::rotate(frame.magnitudes.begin(), frame.magnitudes.begin()+frame.magnitudes.size()/2, frame.magnitudes.end());
        if (width != frame.magnitudes.size()) {
            if (data >= 0) h.H5Dclose(data); if (parameters >= 0) h.H5Dclose(parameters);
            data = parameters = -1; width = frame.magnitudes.size(); row = 0;
            const QByteArray groupName = QStringLiteral("segment_%1").arg(segment++, 6, 10, QLatin1Char('0')).toUtf8();
            const hid_t group = h.H5Gcreate2(file, groupName.constData(), 0, 0, 0);
            if (group >= 0) {
                data = h.createRows(group, "spectrum_db", h.floatType, width);
                parameters = h.createRows(group, "frame_metadata", h.doubleType, 6);
                h.H5Gclose(group);
            }
            if (data < 0 || parameters < 0) { ok = false; break; }
        }
        const double values[6] = {double(frame.utcMs), double(frame.elapsedMs), frame.centerFrequency,
            frame.minFrequency, frame.maxFrequency, double(frame.fftLength)};
        ok = h.appendRow(data, h.floatType, row, width, frame.magnitudes.data()) &&
            h.appendRow(parameters, h.doubleType, row, 6, values);
        ++row; ++frames;
        if (progress) progress(int(100.0*reader.position()/std::max<qint64>(1, reader.size())));
    }
    if (data >= 0) h.H5Dclose(data); if (parameters >= 0) h.H5Dclose(parameters);
    if (file >= 0) {
        ok = h.H5Fflush(file, H5F_SCOPE_GLOBAL) >= 0 && ok;
        ok = h.H5Fclose(file) >= 0 && ok;
    }
    h.H5Eset_auto2(H5E_DEFAULT, previousHandler, previousData);
    if (!ok || frames == 0) {
        if (error && error->isEmpty()) *error = frames == 0 ? QStringLiteral("No spectrum frames") : QStringLiteral("HDF5 write failed");
        return false;
    }
    if (!temporary.rename(outputPath)) { if (error) *error = QStringLiteral("Cannot publish HDF5 output: %1").arg(temporary.errorString()); return false; }
    temporary.setAutoRemove(false);
    return true;
#endif
}

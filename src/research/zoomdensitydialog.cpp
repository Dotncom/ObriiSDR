#include "zoomdensitydialog.h"

#include "appsettingsutils.h"
#include "spectrumpersistencetools.h"

#include <QCloseEvent>
#include <QLabel>
#include <QSettings>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <utility>

ZoomDensityDialog::ZoomDensityDialog(Translator translator, QWidget *parent)
    : QDialog(parent), translator(std::move(translator)) {
    const auto text = [this](const QString &key, const QString &fallback) {
        return this->translator ? this->translator(key, fallback) : fallback;
    };
    setWindowTitle(text(QStringLiteral("zoom_density_title"),
                        QStringLiteral("Selected signal density")));
    setAttribute(Qt::WA_DeleteOnClose, false);
    resize(1050, 720);
    QVBoxLayout *layout = new QVBoxLayout(this);
    rangeLabel = new QLabel(this);
    density = new SignalDensityWidget(this->translator,
                                      this,
                                      QStringLiteral("zoomDensity"));
    layout->addWidget(rangeLabel);
    layout->addWidget(density, 1);

    QSettings settings(persistentSettingsFilePath(), QSettings::IniFormat);
    const QByteArray geometry = settings.value(QStringLiteral("zoomDensity/geometry")).toByteArray();
    if (!geometry.isEmpty()) restoreGeometry(geometry);
}

void ZoomDensityDialog::setRange(double selectedLowHz, double selectedHighHz) {
    lowHz = (std::min)(selectedLowHz, selectedHighHz);
    highHz = (std::max)(selectedLowHz, selectedHighHz);
    rangeLabel->setText(QString::fromUtf8(u8"Виділена смуга: %1 – %2  |  ширина %3")
                            .arg(formatFrequency(lowHz),
                                 formatFrequency(highHz),
                                 formatFrequency(highHz - lowHz)));
}

void ZoomDensityDialog::appendSpectrumFrame(const std::vector<float> &frequencies,
                                            const std::vector<float> &levels,
                                            int amplitudeUnit,
                                            double cursorFrequencyHz) {
    if (!density || !isVisible() || highHz <= lowHz) return;
    const std::size_t count = (std::min)(frequencies.size(), levels.size());
    std::vector<std::pair<float, float>> selected;
    selected.reserve(count / 8 + 8);
    for (std::size_t i = 0; i < count; ++i) {
        const float frequency = frequencies[i];
        const float level = levels[i];
        if (std::isfinite(frequency) && std::isfinite(level) &&
            frequency >= lowHz && frequency <= highHz) {
            selected.emplace_back(frequency, level);
        }
    }
    if (selected.size() < 2) return;
    std::sort(selected.begin(), selected.end(), [](const auto &left, const auto &right) {
        return left.first < right.first;
    });
    croppedFrequencies.resize(selected.size());
    croppedLevels.resize(selected.size());
    for (std::size_t i = 0; i < selected.size(); ++i) {
        croppedFrequencies[i] = selected[i].first;
        croppedLevels[i] = selected[i].second;
    }
    density->appendSpectrumFrame(croppedFrequencies,
                                 croppedLevels,
                                 amplitudeUnit,
                                 cursorFrequencyHz);
}

void ZoomDensityDialog::closeEvent(QCloseEvent *event) {
    QSettings settings(persistentSettingsFilePath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("zoomDensity/geometry"), saveGeometry());
    settings.sync();
    QDialog::closeEvent(event);
}

QString ZoomDensityDialog::formatFrequency(double hz) {
    const double absolute = std::abs(hz);
    if (absolute >= 1.0e9) return QStringLiteral("%1 GHz").arg(hz / 1.0e9, 0, 'f', 6);
    if (absolute >= 1.0e6) return QStringLiteral("%1 MHz").arg(hz / 1.0e6, 0, 'f', 6);
    if (absolute >= 1.0e3) return QStringLiteral("%1 kHz").arg(hz / 1.0e3, 0, 'f', 3);
    return QStringLiteral("%1 Hz").arg(hz, 0, 'f', 2);
}

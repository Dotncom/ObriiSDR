#include "spectrumpersistencetools.h"

#include "appsettingsutils.h"
#include "waterfall3dview.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QGridLayout>
#include <QImage>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTextStream>
#include <QStackedWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <utility>

namespace {
constexpr int kLevelBins = 256;
constexpr int kPlotLeft = 76;
constexpr int kPlotRight = 18;
constexpr int kPlotTop = 14;
constexpr int kPlotBottom = 38;

QString frequencyText(double hz) {
    if (!std::isfinite(hz)) return QStringLiteral("--");
    if (std::abs(hz) >= 1.0e9) return QStringLiteral("%1 GHz").arg(hz / 1.0e9, 0, 'f', 4);
    if (std::abs(hz) >= 1.0e6) return QStringLiteral("%1 MHz").arg(hz / 1.0e6, 0, 'f', 4);
    if (std::abs(hz) >= 1.0e3) return QStringLiteral("%1 kHz").arg(hz / 1.0e3, 0, 'f', 2);
    return QStringLiteral("%1 Hz").arg(hz, 0, 'f', 0);
}

QString amplitudeUnitName(int unit) {
    switch ((std::clamp)(unit, 0, 3)) {
    case 1: return QStringLiteral("dBm");
    case 2: return QStringLiteral("dBuV");
    case 3: return QStringLiteral("uV");
    default: return QStringLiteral("dBFS");
    }
}

QString levelText(double db, int unit) {
    switch ((std::clamp)(unit, 0, 3)) {
    case 2:
        return QStringLiteral("%1").arg(db + 106.9897, 0, 'f', 1);
    case 3:
        return QStringLiteral("%1").arg(std::pow(10.0, (db + 106.9897) / 20.0), 0, 'g', 4);
    default:
        return QStringLiteral("%1").arg(db, 0, 'f', 1);
    }
}

QColor densityColor(double t) {
    t = (std::clamp)(t, 0.0, 1.0);
    if (t < 0.16) {
        const double u = t / 0.16;
        return QColor(static_cast<int>(8.0 * (1.0 - u)),
                      static_cast<int>(18.0 + 14.0 * u),
                      static_cast<int>(48.0 + 72.0 * u));
    }
    if (t < 0.38) {
        const double u = (t - 0.16) / 0.22;
        return QColor(0, static_cast<int>(32.0 + 190.0 * u), static_cast<int>(120.0 + 120.0 * u));
    }
    if (t < 0.64) {
        const double u = (t - 0.38) / 0.26;
        return QColor(static_cast<int>(245.0 * u), static_cast<int>(222.0 + 25.0 * u), static_cast<int>(240.0 * (1.0 - u)));
    }
    if (t < 0.84) {
        const double u = (t - 0.64) / 0.20;
        return QColor(245, static_cast<int>(247.0 * (1.0 - u) + 50.0 * u), 0);
    }
    const double u = (t - 0.84) / 0.16;
    return QColor(245 + static_cast<int>(10.0 * u),
                  50 + static_cast<int>(205.0 * u),
                  static_cast<int>(255.0 * u));
}

std::vector<float> resampleLevels(const std::vector<float> &levels, int width) {
    std::vector<float> result(static_cast<std::size_t>((std::max)(1, width)), -200.0f);
    if (levels.empty()) return result;
    if (result.size() == 1 || levels.size() == 1) {
        result[0] = levels[0];
        return result;
    }
    for (std::size_t x = 0; x < result.size(); ++x) {
        const double source = static_cast<double>(x) *
                              static_cast<double>(levels.size() - 1) /
                              static_cast<double>(result.size() - 1);
        const std::size_t first = static_cast<std::size_t>(source);
        const std::size_t second = (std::min)(first + 1, levels.size() - 1);
        const double mix = source - static_cast<double>(first);
        result[x] = static_cast<float>(levels[first] +
                                       (levels[second] - levels[first]) * mix);
    }
    return result;
}

class DensityCanvas : public QWidget {
public:
    explicit DensityCanvas(QWidget *parent = nullptr) : QWidget(parent) {
        setMinimumSize(620, 360);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }

    void setImage(const QImage &value,
                  double firstHz,
                  double lastHz,
                  double minimumDb,
                  double maximumDb,
                  int unit,
                  double cursorHz) {
        image = value;
        minFrequencyHz = firstHz;
        maxFrequencyHz = lastHz;
        minLevelDb = minimumDb;
        maxLevelDb = maximumDb;
        amplitudeUnit = unit;
        cursorFrequencyHz = cursorHz;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(12, 14, 18));
        painter.setRenderHint(QPainter::TextAntialiasing, true);
        const QRect plot(kPlotLeft,
                         kPlotTop,
                         (std::max)(1, width() - kPlotLeft - kPlotRight),
                         (std::max)(1, height() - kPlotTop - kPlotBottom));
        painter.setPen(QColor(55, 63, 74));
        for (int i = 0; i <= 5; ++i) {
            const int y = plot.top() + (plot.height() * i) / 5;
            painter.drawLine(plot.left(), y, plot.right(), y);
            const double db = maxLevelDb - (maxLevelDb - minLevelDb) * i / 5.0;
            painter.setPen(QColor(190, 198, 210));
            painter.drawText(QRect(2, y - 10, kPlotLeft - 8, 20),
                             Qt::AlignRight | Qt::AlignVCenter,
                             levelText(db, amplitudeUnit));
            painter.setPen(QColor(55, 63, 74));
        }
        if (!image.isNull()) {
            painter.drawImage(plot, image);
        }
        if (std::isfinite(cursorFrequencyHz) &&
            maxFrequencyHz > minFrequencyHz &&
            cursorFrequencyHz >= minFrequencyHz &&
            cursorFrequencyHz <= maxFrequencyHz) {
            const int cursorX = plot.left() + static_cast<int>(
                std::lround((cursorFrequencyHz - minFrequencyHz) /
                            (maxFrequencyHz - minFrequencyHz) * plot.width()));
            painter.setPen(QPen(QColor(255, 225, 70, 220), 1.0, Qt::DashLine));
            painter.drawLine(cursorX, plot.top(), cursorX, plot.bottom());
        }
        painter.setPen(QColor(105, 115, 128));
        painter.drawRect(plot.adjusted(0, 0, -1, -1));
        painter.setPen(QColor(210, 216, 225));
        for (int i = 0; i <= 4; ++i) {
            const int x = plot.left() + (plot.width() * i) / 4;
            const double hz = minFrequencyHz +
                              (maxFrequencyHz - minFrequencyHz) * i / 4.0;
            painter.drawText(QRect(x - 65, plot.bottom() + 6, 130, 22),
                             Qt::AlignHCenter | Qt::AlignTop,
                             frequencyText(hz));
        }
        painter.save();
        painter.translate(15, plot.center().y());
        painter.rotate(-90.0);
        painter.drawText(QRect(-100, -10, 200, 20),
                         Qt::AlignCenter,
                         amplitudeUnitName(amplitudeUnit));
        painter.restore();
    }

private:
    QImage image;
    double minFrequencyHz = 0.0;
    double maxFrequencyHz = 0.0;
    double minLevelDb = -140.0;
    double maxLevelDb = -20.0;
    int amplitudeUnit = 0;
    double cursorFrequencyHz = std::numeric_limits<double>::quiet_NaN();
};
}

struct SignalDensityWidget::Impl {
    struct Contribution {
        std::vector<std::uint16_t> rows;
        std::vector<float> weights;
    };

    Translator translator;
    SignalDensityWidget *owner = nullptr;
    DensityCanvas *canvas = nullptr;
    Waterfall3DView *view3D = nullptr;
    QStackedWidget *viewStack = nullptr;
    QLabel *status = nullptr;
    QSpinBox *history = nullptr;
    QDoubleSpinBox *decay = nullptr;
    QDoubleSpinBox *threshold = nullptr;
    QComboBox *mode = nullptr;
    QComboBox *resolution = nullptr;
    QComboBox *viewMode = nullptr;
    QComboBox *axisX = nullptr;
    QComboBox *axisY = nullptr;
    QComboBox *surfaceStyle = nullptr;
    QComboBox *surfaceSmoothing = nullptr;
    QComboBox *surfaceLighting = nullptr;
    QComboBox *colorMode = nullptr;
    QComboBox *frontProfileStyle = nullptr;
    QSpinBox *interval = nullptr;
    QDoubleSpinBox *minimum = nullptr;
    QDoubleSpinBox *maximum = nullptr;
    QCheckBox *paused = nullptr;
    std::vector<float> histogram;
    std::vector<float> latestSpectrumLevels;
    std::deque<Contribution> contributions;
    QImage rendered;
    QElapsedTimer updateClock;
    double firstFrequencyHz = 0.0;
    double lastFrequencyHz = 0.0;
    int frames = 0;
    int amplitudeUnit = 0;
    int axisXDimension = 0;
    int axisYDimension = 1;
    double cursorFrequencyHz = std::numeric_limits<double>::quiet_NaN();
    QString settingsGroup = QStringLiteral("researchDensity");

    QString text(const QString &key, const QString &fallback) const {
        return translator ? translator(key, fallback) : fallback;
    }

    int widthBins() const {
        return resolution ? resolution->currentData().toInt() : 512;
    }

    void clear() {
        histogram.assign(static_cast<std::size_t>(widthBins() * kLevelBins), 0.0f);
        contributions.clear();
        latestSpectrumLevels.clear();
        rendered = QImage(widthBins(), kLevelBins, QImage::Format_RGB32);
        rendered.fill(Qt::black);
        frames = 0;
        canvas->setImage(rendered,
                         firstFrequencyHz,
                         lastFrequencyHz,
                         minimum->value(),
                         maximum->value(),
                         amplitudeUnit,
                         cursorFrequencyHz);
        if (view3D) view3D->clearHistory();
        status->setText(text(QStringLiteral("research_density_waiting"),
                             QStringLiteral("Waiting for spectrum frames")));
    }

    void saveSettings() const {
        QSettings settings(persistentSettingsFilePath(), QSettings::IniFormat);
        settings.beginGroup(settingsGroup);
        settings.setValue(QStringLiteral("history"), history->value());
        settings.setValue(QStringLiteral("decay"), decay->value());
        settings.setValue(QStringLiteral("threshold"), threshold->value());
        settings.setValue(QStringLiteral("mode"), mode->currentData());
        settings.setValue(QStringLiteral("resolution"), resolution->currentData());
        settings.setValue(QStringLiteral("intervalMs"), interval->value());
        settings.setValue(QStringLiteral("minimumDb"), minimum->value());
        settings.setValue(QStringLiteral("maximumDb"), maximum->value());
        settings.setValue(QStringLiteral("viewMode"), viewMode ? viewMode->currentData() : 0);
        settings.setValue(QStringLiteral("axisX"), axisX ? axisX->currentData() : 0);
        settings.setValue(QStringLiteral("axisY"), axisY ? axisY->currentData() : 1);
        settings.setValue(QStringLiteral("surfaceStyle"),
                          surfaceStyle ? surfaceStyle->currentData() : 0);
        settings.setValue(QStringLiteral("surfaceSmoothing"),
                          surfaceSmoothing ? surfaceSmoothing->currentData() : 0);
        settings.setValue(QStringLiteral("surfaceLighting"),
                          surfaceLighting ? surfaceLighting->currentData() : 0);
        settings.setValue(QStringLiteral("colorMode"), colorMode ? colorMode->currentData() : 0);
        settings.setValue(QStringLiteral("frontProfileStyle"),
                          frontProfileStyle ? frontProfileStyle->currentData() : 0);
        settings.endGroup();
    }

    void render3D(float peak, double cutoff, double logDenominator) {
        if (!view3D || !viewMode || viewMode->currentData().toInt() == 0 || histogram.empty()) return;
        constexpr int targetRows = 128;
        const int sourceRowsPerRow = (std::max)(1, kLevelBins / targetRows);
        const int rows = (kLevelBins + sourceRowsPerRow - 1) / sourceRowsPerRow;
        view3D->setHistoryCapacity(rows);
        view3D->clearHistory();
        std::vector<float> levels(static_cast<std::size_t>(widthBins()), -100.0f);
        std::vector<unsigned char> colors(static_cast<std::size_t>(widthBins()) * 3U, 0);
        const bool colorByLevel = colorMode && colorMode->currentData().toInt() == 1;
        for (int outRow = rows - 1; outRow >= 0; --outRow) {
            const int firstRow = outRow * sourceRowsPerRow;
            const int lastRow = (std::min)(kLevelBins, firstRow + sourceRowsPerRow);
            for (int x = 0; x < widthBins(); ++x) {
                float value = 0.0f;
                for (int row = firstRow; row < lastRow; ++row) {
                    value = (std::max)(value, histogram[static_cast<std::size_t>(row * widthBins() + x)]);
                }
                const double normalized = value > cutoff && peak > 0.0f
                                              ? std::log1p(static_cast<double>(value)) / logDenominator
                                              : 0.0;
                levels[static_cast<std::size_t>(x)] = static_cast<float>(-100.0 + 100.0 * normalized);
                const double levelNormalized = rows > 1
                                                   ? 1.0 - static_cast<double>(outRow) /
                                                               static_cast<double>(rows - 1)
                                                   : 0.5;
                const QColor color = densityColor(colorByLevel ? levelNormalized : normalized);
                const std::size_t rgb = static_cast<std::size_t>(x) * 3U;
                colors[rgb] = static_cast<unsigned char>(color.red());
                colors[rgb + 1] = static_cast<unsigned char>(color.green());
                colors[rgb + 2] = static_cast<unsigned char>(color.blue());
            }
            view3D->appendHistoryRow(levels, colors, -100.0f, 0.0f);
        }
        std::vector<float> frontProfile;
        frontProfile.reserve(latestSpectrumLevels.size());
        const double minimumDb = minimum->value();
        const double levelRange = (std::max)(1.0, maximum->value() - minimumDb);
        for (const float level : latestSpectrumLevels) {
            frontProfile.push_back(static_cast<float>(std::clamp(
                (static_cast<double>(level) - minimumDb) / levelRange, 0.0, 1.0)));
        }
        view3D->setDensityFrontProfile(frontProfile);
        const bool withMini = viewMode->currentData().toInt() == 2;
        if (withMini) {
            view3D->setOverview(rendered.copy(), 0, kLevelBins - 1, -1, kLevelBins);
        }
        view3D->setOverviewVisible(withMini);
        view3D->setDensityAxisRanges(firstFrequencyHz,
                                     lastFrequencyHz,
                                     minimum->value(),
                                     maximum->value(),
                                     peak);
    }

    void render() {
        if (histogram.empty()) return;
        float peak = 0.0f;
        for (const float value : histogram) peak = (std::max)(peak, value);
        const double cutoff = peak * threshold->value() / 100.0;
        const double logDenominator = std::log1p((std::max)(1.0, static_cast<double>(peak)));
        const bool colorByLevel = colorMode && colorMode->currentData().toInt() == 1;
        for (int y = 0; y < kLevelBins; ++y) {
            QRgb *line = reinterpret_cast<QRgb *>(rendered.scanLine(y));
            for (int x = 0; x < widthBins(); ++x) {
                const float value = histogram[static_cast<std::size_t>(y * widthBins() + x)];
                if (value <= cutoff || peak <= 0.0f) {
                    line[x] = qRgb(0, 0, 0);
                } else {
                    const double normalized = std::log1p(static_cast<double>(value)) /
                                              logDenominator;
                    const double levelNormalized = 1.0 - static_cast<double>(y) /
                                                            static_cast<double>(kLevelBins - 1);
                    line[x] = densityColor(colorByLevel ? levelNormalized : normalized).rgb();
                }
            }
        }
        canvas->setImage(rendered,
                         firstFrequencyHz,
                         lastFrequencyHz,
                         minimum->value(),
                         maximum->value(),
                         amplitudeUnit,
                         cursorFrequencyHz);
        render3D(peak, cutoff, logDenominator);
        status->setText(text(QStringLiteral("research_density_status"),
                             QStringLiteral("%1 frames retained | peak density %2 | %3 bins"))
                            .arg(contributions.size())
                            .arg(peak, 0, 'g', 5)
                            .arg(widthBins()));
    }

    void exportImage() {
        const QString path = QFileDialog::getSaveFileName(
            owner,
            text(QStringLiteral("research_density_export_image"),
                 QStringLiteral("Export density image")),
            QStringLiteral("signal-density-%1.png")
                .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-hhmmss"))),
            QStringLiteral("PNG image (*.png)"));
        if (!path.isEmpty()) canvas->grab().save(path, "PNG");
    }

    void exportCsv() {
        if (histogram.empty()) return;
        const QString path = QFileDialog::getSaveFileName(
            owner,
            text(QStringLiteral("research_density_export_csv"),
                 QStringLiteral("Export density CSV")),
            QStringLiteral("signal-density-%1.csv")
                .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-hhmmss"))),
            QStringLiteral("CSV (*.csv)"));
        if (path.isEmpty()) return;
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) return;
        QTextStream stream(&file);
        stream.setRealNumberNotation(QTextStream::ScientificNotation);
        stream << "frequency_hz,level_db,density\n";
        for (int x = 0; x < widthBins(); ++x) {
            const double frequency = widthBins() > 1
                                         ? firstFrequencyHz +
                                               (lastFrequencyHz - firstFrequencyHz) *
                                                   x / static_cast<double>(widthBins() - 1)
                                         : firstFrequencyHz;
            for (int y = 0; y < kLevelBins; ++y) {
                const float value = histogram[static_cast<std::size_t>(y * widthBins() + x)];
                if (value <= 0.0f) continue;
                const double level = maximum->value() -
                                     (maximum->value() - minimum->value()) *
                                         y / static_cast<double>(kLevelBins - 1);
                stream << frequency << ',' << level << ',' << value << '\n';
            }
        }
    }
};

SignalDensityWidget::SignalDensityWidget(Translator translator,
                                         QWidget *parent,
                                         QString settingsGroup)
    : QWidget(parent), impl(std::make_unique<Impl>()) {
    impl->translator = std::move(translator);
    impl->owner = this;
    impl->settingsGroup = std::move(settingsGroup);
    QVBoxLayout *root = new QVBoxLayout(this);
    QGridLayout *controls = new QGridLayout();
    impl->history = new QSpinBox(this);
    impl->history->setRange(8, 4096);
    impl->decay = new QDoubleSpinBox(this);
    impl->decay->setRange(0.0, 100.0);
    impl->decay->setDecimals(2);
    impl->decay->setSuffix(QStringLiteral(" %"));
    impl->threshold = new QDoubleSpinBox(this);
    impl->threshold->setRange(0.0, 25.0);
    impl->threshold->setDecimals(2);
    impl->threshold->setSuffix(QStringLiteral(" %"));
    impl->mode = new QComboBox(this);
    impl->mode->addItem(impl->text(QStringLiteral("research_density_count"),
                                   QStringLiteral("Count")), 0);
    impl->mode->addItem(impl->text(QStringLiteral("research_density_power"),
                                   QStringLiteral("Linear power")), 1);
    impl->viewMode = new QComboBox(this);
    impl->viewMode->addItem(QStringLiteral("2D"), 0);
    impl->viewMode->addItem(QStringLiteral("3D"), 1);
    impl->viewMode->addItem(QStringLiteral("3D + mini"), 2);
    impl->viewMode->setToolTip(impl->text(
        QStringLiteral("research_density_camera_tooltip"),
        QStringLiteral("In 3D hold Ctrl: drag with the left button to orbit, right button to pan, and use the wheel to zoom.")));
    impl->axisX = new QComboBox(this);
    impl->axisY = new QComboBox(this);
    const auto addAxisDimensions = [this](QComboBox *combo) {
        combo->addItem(impl->text(QStringLiteral("research_density_axis_frequency"),
                                  QStringLiteral("Frequency")), 0);
        combo->addItem(impl->text(QStringLiteral("research_density_axis_accumulation"),
                                  QStringLiteral("Accumulation / density")), 1);
        combo->addItem(impl->text(QStringLiteral("research_density_axis_level"),
                                  QStringLiteral("Level (dBFS)")), 2);
    };
    addAxisDimensions(impl->axisX);
    addAxisDimensions(impl->axisY);
    const QString axisTooltip = impl->text(
        QStringLiteral("research_density_axis_selection_tooltip"),
        QStringLiteral("Choose parameters for X and Y. The unused parameter is assigned to Z automatically."));
    impl->axisX->setToolTip(axisTooltip);
    impl->axisY->setToolTip(axisTooltip);
    impl->surfaceStyle = new QComboBox(this);
    impl->surfaceStyle->addItem(impl->text(QStringLiteral("surface_style_original"),
                                           QStringLiteral("Original / needles")), 0);
    impl->surfaceStyle->addItem(impl->text(QStringLiteral("surface_style_solid"),
                                           QStringLiteral("Solid surface")), 1);
    impl->surfaceSmoothing = new QComboBox(this);
    impl->surfaceSmoothing->addItem(impl->text(QStringLiteral("surface_smoothing_off"),
                                               QStringLiteral("Off")), 0);
    impl->surfaceSmoothing->addItem(impl->text(QStringLiteral("surface_smoothing_soft"),
                                               QStringLiteral("Soft")), 1);
    impl->surfaceSmoothing->addItem(impl->text(QStringLiteral("surface_smoothing_strong"),
                                               QStringLiteral("Strong")), 2);
    impl->surfaceLighting = new QComboBox(this);
    impl->surfaceLighting->addItem(impl->text(QStringLiteral("surface_lighting_off"),
                                              QStringLiteral("Off")), 0);
    impl->surfaceLighting->addItem(impl->text(QStringLiteral("surface_lighting_soft"),
                                              QStringLiteral("Soft shadows")), 1);
    impl->surfaceLighting->addItem(impl->text(QStringLiteral("surface_lighting_strong"),
                                              QStringLiteral("Strong shadows")), 2);
    impl->colorMode = new QComboBox(this);
    impl->colorMode->addItem(impl->text(QStringLiteral("research_density_color_density"),
                                        QStringLiteral("Density")), 0);
    impl->colorMode->addItem(impl->text(QStringLiteral("research_density_color_level"),
                                        QStringLiteral("Level (dBFS)")), 1);
    impl->frontProfileStyle = new QComboBox(this);
    impl->frontProfileStyle->addItem(
        impl->text(QStringLiteral("research_density_profile_line"),
                   QStringLiteral("Thin line")), 0);
    impl->frontProfileStyle->addItem(
        impl->text(QStringLiteral("research_density_profile_filled"),
                   QStringLiteral("Filled color spectrum")), 1);
    const QString surfaceTooltip = impl->text(
        QStringLiteral("surface_processing_tooltip"),
        QStringLiteral("GPU surface processing. Smoothing and lighting cost additional GPU time; keep them off on weak systems."));
    impl->surfaceStyle->setToolTip(surfaceTooltip);
    impl->surfaceSmoothing->setToolTip(surfaceTooltip);
    impl->surfaceLighting->setToolTip(surfaceTooltip);
    impl->colorMode->setToolTip(impl->text(
        QStringLiteral("research_density_color_tooltip"),
        QStringLiteral("Choose whether color represents accumulated density or amplitude level. Geometry always represents density.")));
    impl->frontProfileStyle->setToolTip(impl->text(
        QStringLiteral("research_density_profile_tooltip"),
        QStringLiteral("Show the latest spectrum on the zero-density face as a thin line or a filled amplitude-colored 2D plot.")));
    impl->resolution = new QComboBox(this);
    for (const int value : {128, 256, 512, 1024, 2048}) {
        impl->resolution->addItem(QString::number(value), value);
    }
    impl->interval = new QSpinBox(this);
    impl->interval->setRange(10, 2000);
    impl->interval->setSuffix(QStringLiteral(" ms"));
    impl->minimum = new QDoubleSpinBox(this);
    impl->minimum->setRange(-240.0, 80.0);
    impl->minimum->setSuffix(QStringLiteral(" dB"));
    impl->maximum = new QDoubleSpinBox(this);
    impl->maximum->setRange(-240.0, 100.0);
    impl->maximum->setSuffix(QStringLiteral(" dB"));
    impl->paused = new QCheckBox(impl->text(QStringLiteral("pause"), QStringLiteral("Pause")), this);
    QPushButton *clearButton = new QPushButton(
        impl->text(QStringLiteral("clear"), QStringLiteral("Clear")), this);
    QPushButton *imageButton = new QPushButton(
        impl->text(QStringLiteral("research_export_png"), QStringLiteral("Export PNG")), this);
    QPushButton *csvButton = new QPushButton(
        impl->text(QStringLiteral("research_export_csv"), QStringLiteral("Export CSV")), this);
    controls->addWidget(new QLabel(impl->text(QStringLiteral("research_density_history"),
                                               QStringLiteral("History frames:")), this), 0, 0);
    controls->addWidget(impl->history, 0, 1);
    controls->addWidget(new QLabel(impl->text(QStringLiteral("research_density_decay"),
                                               QStringLiteral("Decay:")), this), 0, 2);
    controls->addWidget(impl->decay, 0, 3);
    controls->addWidget(new QLabel(impl->text(QStringLiteral("research_density_threshold"),
                                               QStringLiteral("Visible density:")), this), 0, 4);
    controls->addWidget(impl->threshold, 0, 5);
    controls->addWidget(impl->paused, 0, 6);
    controls->addWidget(new QLabel(impl->text(QStringLiteral("research_density_view"),
                                               QStringLiteral("View:")), this), 0, 7);
    controls->addWidget(impl->viewMode, 0, 8);
    controls->addWidget(new QLabel(impl->text(QStringLiteral("research_density_mode"),
                                               QStringLiteral("Accumulation:")), this), 1, 0);
    controls->addWidget(impl->mode, 1, 1);
    controls->addWidget(new QLabel(impl->text(QStringLiteral("research_density_resolution"),
                                               QStringLiteral("Frequency bins:")), this), 1, 2);
    controls->addWidget(impl->resolution, 1, 3);
    controls->addWidget(new QLabel(impl->text(QStringLiteral("research_density_interval"),
                                               QStringLiteral("Update:")), this), 1, 4);
    controls->addWidget(impl->interval, 1, 5);
    controls->addWidget(new QLabel(impl->text(QStringLiteral("minimum"), QStringLiteral("Minimum:")), this), 2, 0);
    controls->addWidget(impl->minimum, 2, 1);
    controls->addWidget(new QLabel(impl->text(QStringLiteral("maximum"), QStringLiteral("Maximum:")), this), 2, 2);
    controls->addWidget(impl->maximum, 2, 3);
    controls->addWidget(clearButton, 2, 4);
    controls->addWidget(imageButton, 2, 5);
    controls->addWidget(csvButton, 2, 6);
    controls->addWidget(new QLabel(impl->text(QStringLiteral("research_density_axis_x"),
                                               QStringLiteral("Axis X:")), this), 3, 0);
    controls->addWidget(impl->axisX, 3, 1, 1, 2);
    controls->addWidget(new QLabel(impl->text(QStringLiteral("research_density_axis_y"),
                                               QStringLiteral("Axis Y:")), this), 3, 3);
    controls->addWidget(impl->axisY, 3, 4, 1, 2);
    controls->addWidget(new QLabel(impl->text(QStringLiteral("surface_style"),
                                               QStringLiteral("Surface:")), this), 4, 0);
    controls->addWidget(impl->surfaceStyle, 4, 1, 1, 2);
    controls->addWidget(new QLabel(impl->text(QStringLiteral("surface_smoothing"),
                                               QStringLiteral("Smoothing:")), this), 4, 3);
    controls->addWidget(impl->surfaceSmoothing, 4, 4, 1, 2);
    controls->addWidget(new QLabel(impl->text(QStringLiteral("surface_lighting"),
                                               QStringLiteral("Lighting:")), this), 4, 6);
    controls->addWidget(impl->surfaceLighting, 4, 7, 1, 2);
    controls->addWidget(new QLabel(impl->text(QStringLiteral("research_density_color"),
                                               QStringLiteral("Color:")), this), 5, 0);
    controls->addWidget(impl->colorMode, 5, 1, 1, 2);
    controls->addWidget(new QLabel(impl->text(QStringLiteral("research_density_profile"),
                                               QStringLiteral("Front profile:")), this), 5, 3);
    controls->addWidget(impl->frontProfileStyle, 5, 4, 1, 3);
    controls->setColumnStretch(9, 1);
    root->addLayout(controls);
    impl->status = new QLabel(this);
    root->addWidget(impl->status);
    impl->canvas = new DensityCanvas(this);
    impl->view3D = new Waterfall3DView(this);
    impl->view3D->setResolutionDivisor(1);
    impl->view3D->setDensityAxes(true);
    impl->view3D->setDensityAxisLabels(
        impl->text(QStringLiteral("research_density_axis_frequency"), QStringLiteral("Frequency")),
        impl->text(QStringLiteral("research_density_axis_accumulation"), QStringLiteral("Accumulation / density")),
        impl->text(QStringLiteral("research_density_axis_level"), QStringLiteral("Level (dBFS)")));
    impl->viewStack = new QStackedWidget(this);
    impl->viewStack->addWidget(impl->canvas);
    impl->viewStack->addWidget(impl->view3D);
    root->addWidget(impl->viewStack, 1);

    QSettings settings(persistentSettingsFilePath(), QSettings::IniFormat);
    settings.beginGroup(impl->settingsGroup);
    impl->history->setValue(settings.value(QStringLiteral("history"), 256).toInt());
    impl->decay->setValue(settings.value(QStringLiteral("decay"), 98.0).toDouble());
    impl->threshold->setValue(settings.value(QStringLiteral("threshold"), 0.5).toDouble());
    impl->mode->setCurrentIndex((std::max)(0, impl->mode->findData(settings.value(QStringLiteral("mode"), 0))));
    impl->resolution->setCurrentIndex((std::max)(0, impl->resolution->findData(settings.value(QStringLiteral("resolution"), 512))));
    impl->interval->setValue(settings.value(QStringLiteral("intervalMs"), 50).toInt());
    impl->minimum->setValue(settings.value(QStringLiteral("minimumDb"), -140.0).toDouble());
    impl->maximum->setValue(settings.value(QStringLiteral("maximumDb"), -20.0).toDouble());
    impl->viewMode->setCurrentIndex((std::max)(0, impl->viewMode->findData(settings.value(QStringLiteral("viewMode"), 0))));
    int savedAxisX = std::clamp(settings.value(QStringLiteral("axisX"), 0).toInt(), 0, 2);
    int savedAxisY = std::clamp(settings.value(QStringLiteral("axisY"), 1).toInt(), 0, 2);
    if (savedAxisX == savedAxisY) {
        savedAxisX = 0;
        savedAxisY = 1;
    }
    impl->axisX->setCurrentIndex((std::max)(0, impl->axisX->findData(savedAxisX)));
    impl->axisY->setCurrentIndex((std::max)(0, impl->axisY->findData(savedAxisY)));
    impl->axisXDimension = savedAxisX;
    impl->axisYDimension = savedAxisY;
    impl->surfaceStyle->setCurrentIndex((std::max)(
        0, impl->surfaceStyle->findData(settings.value(QStringLiteral("surfaceStyle"), 0))));
    impl->surfaceSmoothing->setCurrentIndex((std::max)(
        0, impl->surfaceSmoothing->findData(settings.value(QStringLiteral("surfaceSmoothing"), 0))));
    impl->surfaceLighting->setCurrentIndex((std::max)(
        0, impl->surfaceLighting->findData(settings.value(QStringLiteral("surfaceLighting"), 0))));
    impl->colorMode->setCurrentIndex((std::max)(
        0, impl->colorMode->findData(settings.value(QStringLiteral("colorMode"), 0))));
    impl->frontProfileStyle->setCurrentIndex((std::max)(
        0, impl->frontProfileStyle->findData(
               settings.value(QStringLiteral("frontProfileStyle"), 0))));
    settings.endGroup();
    impl->view3D->setDensityAxisMapping(impl->axisXDimension, impl->axisYDimension);
    impl->view3D->setSurfaceStyle(impl->surfaceStyle->currentData().toInt());
    impl->view3D->setSurfaceSmoothing(impl->surfaceSmoothing->currentData().toInt());
    impl->view3D->setSurfaceLighting(impl->surfaceLighting->currentData().toInt());
    impl->view3D->setDensityFrontProfileStyle(impl->frontProfileStyle->currentData().toInt());
    impl->viewStack->setCurrentIndex(impl->viewMode->currentData().toInt() == 0 ? 0 : 1);
    impl->view3D->setOverviewVisible(impl->viewMode->currentData().toInt() == 2);
    impl->updateClock.start();
    impl->clear();

    auto reset = [this]() {
        if (impl->maximum->value() <= impl->minimum->value() + 1.0) {
            impl->maximum->setValue(impl->minimum->value() + 1.0);
        }
        impl->saveSettings();
        impl->clear();
    };
    connect(impl->history, QOverload<int>::of(&QSpinBox::valueChanged), this, [reset](int) { reset(); });
    connect(impl->decay, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [reset](double) { reset(); });
    connect(impl->mode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [reset](int) { reset(); });
    connect(impl->resolution, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [reset](int) { reset(); });
    connect(impl->viewMode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        const int mode = impl->viewMode->currentData().toInt();
        impl->viewStack->setCurrentIndex(mode == 0 ? 0 : 1);
        impl->view3D->setOverviewVisible(mode == 2);
        impl->saveSettings();
        impl->render();
    });
    const auto applyAxisMapping = [this](bool xChanged) {
        int x = impl->axisX->currentData().toInt();
        int y = impl->axisY->currentData().toInt();
        if (x == y) {
            if (xChanged) {
                int replacement = impl->axisXDimension;
                if (replacement == x) replacement = (x + 1) % 3;
                const QSignalBlocker blocker(impl->axisY);
                impl->axisY->setCurrentIndex(impl->axisY->findData(replacement));
                y = replacement;
            } else {
                int replacement = impl->axisYDimension;
                if (replacement == y) replacement = (y + 1) % 3;
                const QSignalBlocker blocker(impl->axisX);
                impl->axisX->setCurrentIndex(impl->axisX->findData(replacement));
                x = replacement;
            }
        }
        impl->axisXDimension = x;
        impl->axisYDimension = y;
        impl->view3D->setDensityAxisMapping(x, y);
        impl->saveSettings();
        impl->render();
    };
    connect(impl->axisX, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [applyAxisMapping](int) { applyAxisMapping(true); });
    connect(impl->axisY, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [applyAxisMapping](int) { applyAxisMapping(false); });
    const auto applySurfaceProcessing = [this]() {
        impl->view3D->setSurfaceStyle(impl->surfaceStyle->currentData().toInt());
        impl->view3D->setSurfaceSmoothing(impl->surfaceSmoothing->currentData().toInt());
        impl->view3D->setSurfaceLighting(impl->surfaceLighting->currentData().toInt());
        impl->saveSettings();
        impl->render();
    };
    connect(impl->surfaceStyle, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [applySurfaceProcessing](int) { applySurfaceProcessing(); });
    connect(impl->surfaceSmoothing, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [applySurfaceProcessing](int) { applySurfaceProcessing(); });
    connect(impl->surfaceLighting, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [applySurfaceProcessing](int) { applySurfaceProcessing(); });
    connect(impl->colorMode, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) {
                impl->saveSettings();
                impl->render();
            });
    connect(impl->frontProfileStyle, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) {
                impl->view3D->setDensityFrontProfileStyle(
                    impl->frontProfileStyle->currentData().toInt());
                impl->saveSettings();
                impl->render();
            });
    connect(impl->minimum, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [reset](double) { reset(); });
    connect(impl->maximum, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [reset](double) { reset(); });
    connect(impl->threshold, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double) {
        impl->saveSettings();
        impl->render();
    });
    connect(impl->interval, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int) { impl->saveSettings(); });
    connect(clearButton, &QPushButton::clicked, this, [this]() { impl->clear(); });
    connect(imageButton, &QPushButton::clicked, this, [this]() { impl->exportImage(); });
    connect(csvButton, &QPushButton::clicked, this, [this]() { impl->exportCsv(); });
}

SignalDensityWidget::~SignalDensityWidget() = default;

void SignalDensityWidget::appendSpectrumFrame(const std::vector<float> &frequencies,
                                              const std::vector<float> &levels,
                                              int amplitudeUnit,
                                              double cursorFrequencyHz) {
    if (!impl || impl->paused->isChecked() || frequencies.empty() || levels.empty()) return;
    if (impl->updateClock.isValid() && impl->updateClock.elapsed() < impl->interval->value()) return;
    impl->updateClock.restart();
    const std::size_t count = (std::min)(frequencies.size(), levels.size());
    const double firstHz = frequencies.front();
    const double lastHz = frequencies[count - 1];
    const double span = std::abs(lastHz - firstHz);
    const bool rangeChanged = impl->histogram.size() !=
                                  static_cast<std::size_t>(impl->widthBins() * kLevelBins) ||
                              std::abs(firstHz - impl->firstFrequencyHz) >
                                  (std::max)(1.0, span * 1.0e-4) ||
                              std::abs(lastHz - impl->lastFrequencyHz) >
                                  (std::max)(1.0, span * 1.0e-4);
    impl->firstFrequencyHz = firstHz;
    impl->lastFrequencyHz = lastHz;
    impl->amplitudeUnit = amplitudeUnit;
    impl->cursorFrequencyHz = cursorFrequencyHz;
    if (rangeChanged) impl->clear();

    const float retain = static_cast<float>(impl->decay->value() / 100.0);
    for (float &value : impl->histogram) value *= retain;
    const int historyFrames = impl->history->value();
    if (static_cast<int>(impl->contributions.size()) >= historyFrames) {
        const Impl::Contribution &oldest = impl->contributions.front();
        const float ageScale = std::pow(retain, static_cast<float>(historyFrames));
        for (int x = 0; x < impl->widthBins(); ++x) {
            const int row = oldest.rows[static_cast<std::size_t>(x)];
            float &bin = impl->histogram[static_cast<std::size_t>(row * impl->widthBins() + x)];
            bin = (std::max)(0.0f, bin - oldest.weights[static_cast<std::size_t>(x)] * ageScale);
        }
        impl->contributions.pop_front();
    }

    const std::vector<float> sampled = resampleLevels(
        std::vector<float>(levels.begin(), levels.begin() + static_cast<std::ptrdiff_t>(count)),
        impl->widthBins());
    impl->latestSpectrumLevels = sampled;
    Impl::Contribution contribution;
    contribution.rows.resize(static_cast<std::size_t>(impl->widthBins()));
    contribution.weights.resize(static_cast<std::size_t>(impl->widthBins()));
    const double minimum = impl->minimum->value();
    const double maximum = impl->maximum->value();
    const double range = (std::max)(1.0, maximum - minimum);
    const bool linearPower = impl->mode->currentData().toInt() == 1;
    for (int x = 0; x < impl->widthBins(); ++x) {
        const double level = sampled[static_cast<std::size_t>(x)];
        const int row = (std::clamp)(
            static_cast<int>(std::lround((maximum - level) * (kLevelBins - 1) / range)),
            0,
            kLevelBins - 1);
        const float weight = linearPower
                                 ? static_cast<float>(std::pow(10.0, (level - maximum) / 10.0))
                                 : 1.0f;
        contribution.rows[static_cast<std::size_t>(x)] = static_cast<std::uint16_t>(row);
        contribution.weights[static_cast<std::size_t>(x)] = weight;
        impl->histogram[static_cast<std::size_t>(row * impl->widthBins() + x)] += weight;
    }
    impl->contributions.push_back(std::move(contribution));
    ++impl->frames;
    impl->render();
}

namespace {
class SpectrumMaskCanvas : public QWidget {
public:
    explicit SpectrumMaskCanvas(QWidget *parent = nullptr) : QWidget(parent) {
        setMinimumSize(620, 360);
        setMouseTracking(true);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }

    void setData(const std::vector<float> &newFrequencies,
                 const std::vector<float> &newCurrent,
                 const std::vector<float> &newBaseline,
                 const std::vector<float> &newUpper,
                 const std::vector<float> &newLower,
                 double newMinimum,
                 double newMaximum,
                 int newAmplitudeUnit) {
        frequencies = newFrequencies;
        current = newCurrent;
        baseline = newBaseline;
        upper = newUpper;
        lower = newLower;
        minimum = newMinimum;
        maximum = newMaximum;
        amplitudeUnit = newAmplitudeUnit;
        update();
    }

    void setEditUpper(bool value) { editUpper = value; }
    void setEditHandler(std::function<void(int, float)> handler) {
        editHandler = std::move(handler);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(12, 14, 18));
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::TextAntialiasing, true);
        const QRect plot = plotRect();
        painter.setPen(QColor(52, 59, 69));
        for (int i = 0; i <= 5; ++i) {
            const int y = plot.top() + plot.height() * i / 5;
            painter.drawLine(plot.left(), y, plot.right(), y);
            const double level = maximum - (maximum - minimum) * i / 5.0;
            painter.setPen(QColor(190, 198, 210));
            painter.drawText(QRect(2, y - 10, kPlotLeft - 8, 20),
                             Qt::AlignRight | Qt::AlignVCenter,
                             levelText(level, amplitudeUnit));
            painter.setPen(QColor(52, 59, 69));
        }
        if (!current.empty() && current.size() == upper.size() &&
            current.size() == lower.size()) {
            const int n = static_cast<int>(current.size());
            for (int i = 0; i < n; ++i) {
                const bool above = current[static_cast<std::size_t>(i)] >
                                   upper[static_cast<std::size_t>(i)];
                const bool below = current[static_cast<std::size_t>(i)] <
                                   lower[static_cast<std::size_t>(i)];
                if (!above && !below) continue;
                const int x0 = plot.left() + plot.width() * i / n;
                const int x1 = plot.left() + plot.width() * (i + 1) / n;
                const double deviation = above
                                             ? current[static_cast<std::size_t>(i)] -
                                                   upper[static_cast<std::size_t>(i)]
                                             : lower[static_cast<std::size_t>(i)] -
                                                   current[static_cast<std::size_t>(i)];
                const int alpha = (std::clamp)(
                    static_cast<int>(50.0 + deviation * 18.0), 50, 190);
                painter.fillRect(QRect(x0, plot.top(), (std::max)(1, x1 - x0), plot.height()),
                                 above ? QColor(255, 42, 42, alpha)
                                       : QColor(40, 120, 255, alpha));
            }
        }
        drawTrace(painter, plot, baseline, QColor(150, 155, 165), 1.0, Qt::DashLine);
        drawTrace(painter, plot, upper, QColor(255, 90, 70), 1.6, Qt::SolidLine);
        drawTrace(painter, plot, lower, QColor(65, 175, 255), 1.6, Qt::SolidLine);
        drawTrace(painter, plot, current, QColor(90, 255, 120), 1.4, Qt::SolidLine);
        painter.setPen(QColor(105, 115, 128));
        painter.drawRect(plot.adjusted(0, 0, -1, -1));
        painter.setPen(QColor(210, 216, 225));
        if (!frequencies.empty()) {
            for (int i = 0; i <= 4; ++i) {
                const int x = plot.left() + plot.width() * i / 4;
                const std::size_t index = static_cast<std::size_t>(
                    (frequencies.size() - 1) * i / 4);
                painter.drawText(QRect(x - 65, plot.bottom() + 6, 130, 22),
                                 Qt::AlignHCenter | Qt::AlignTop,
                                 frequencyText(frequencies[index]));
            }
        }
        painter.save();
        painter.translate(15, plot.center().y());
        painter.rotate(-90.0);
        painter.drawText(QRect(-100, -10, 200, 20),
                         Qt::AlignCenter,
                         amplitudeUnitName(amplitudeUnit));
        painter.restore();
    }

    void mousePressEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton) {
            editing = true;
            editAt(event->pos());
        }
    }

    void mouseMoveEvent(QMouseEvent *event) override {
        if (editing && (event->buttons() & Qt::LeftButton)) editAt(event->pos());
    }

    void mouseReleaseEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton) editing = false;
    }

private:
    QRect plotRect() const {
        return QRect(kPlotLeft,
                     kPlotTop,
                     (std::max)(1, width() - kPlotLeft - kPlotRight),
                     (std::max)(1, height() - kPlotTop - kPlotBottom));
    }

    void drawTrace(QPainter &painter,
                   const QRect &plot,
                   const std::vector<float> &values,
                   const QColor &color,
                   double width,
                   Qt::PenStyle style) {
        if (values.size() < 2) return;
        QPainterPath path;
        const double range = (std::max)(1.0, maximum - minimum);
        for (std::size_t i = 0; i < values.size(); ++i) {
            const double x = plot.left() +
                             plot.width() * i / static_cast<double>(values.size() - 1);
            const double normalized = (maximum - values[i]) / range;
            const double y = plot.top() + plot.height() * normalized;
            if (i == 0) path.moveTo(x, y);
            else path.lineTo(x, y);
        }
        painter.setPen(QPen(color, width, style));
        painter.drawPath(path);
    }

    void editAt(const QPoint &position) {
        if (!editHandler || current.empty()) return;
        const QRect plot = plotRect();
        if (!plot.contains(position)) return;
        const int index = (std::clamp)(
            static_cast<int>(std::lround((position.x() - plot.left()) *
                                         (current.size() - 1) /
                                         static_cast<double>((std::max)(1, plot.width())))),
            0,
            static_cast<int>(current.size()) - 1);
        const double normalized = (position.y() - plot.top()) /
                                  static_cast<double>((std::max)(1, plot.height()));
        const float level = static_cast<float>(maximum -
                                               normalized * (maximum - minimum));
        editHandler(editUpper ? index : -index - 1, level);
    }

    std::vector<float> frequencies;
    std::vector<float> current;
    std::vector<float> baseline;
    std::vector<float> upper;
    std::vector<float> lower;
    double minimum = -140.0;
    double maximum = -20.0;
    int amplitudeUnit = 0;
    bool editUpper = true;
    bool editing = false;
    std::function<void(int, float)> editHandler;
};
}

struct SpectrumMaskWidget::Impl {
    Translator translator;
    TriggerHandler triggerHandler;
    SpectrumMaskWidget *owner = nullptr;
    SpectrumMaskCanvas *canvas = nullptr;
    QLabel *status = nullptr;
    QDoubleSpinBox *upperMargin = nullptr;
    QDoubleSpinBox *lowerMargin = nullptr;
    QDoubleSpinBox *minimum = nullptr;
    QDoubleSpinBox *maximum = nullptr;
    QComboBox *editMode = nullptr;
    QSpinBox *interval = nullptr;
    QSpinBox *cooldown = nullptr;
    QCheckBox *paused = nullptr;
    QCheckBox *triggerEnabled = nullptr;
    std::vector<float> frequencies;
    std::vector<float> current;
    std::vector<float> baseline;
    std::vector<float> upper;
    std::vector<float> lower;
    QElapsedTimer updateClock;
    qint64 lastTriggerMs = std::numeric_limits<qint64>::min();
    bool failed = false;
    int amplitudeUnit = 0;
    double baselineFirstHz = 0.0;
    double baselineLastHz = 0.0;

    QString text(const QString &key, const QString &fallback) const {
        return translator ? translator(key, fallback) : fallback;
    }

    void saveSettings() const {
        QSettings settings(persistentSettingsFilePath(), QSettings::IniFormat);
        settings.beginGroup(QStringLiteral("researchMask"));
        settings.setValue(QStringLiteral("upperMarginDb"), upperMargin->value());
        settings.setValue(QStringLiteral("lowerMarginDb"), lowerMargin->value());
        settings.setValue(QStringLiteral("minimumDb"), minimum->value());
        settings.setValue(QStringLiteral("maximumDb"), maximum->value());
        settings.setValue(QStringLiteral("intervalMs"), interval->value());
        settings.setValue(QStringLiteral("cooldownSeconds"), cooldown->value());
        settings.setValue(QStringLiteral("triggerEnabled"), triggerEnabled->isChecked());
        settings.endGroup();
    }

    void refreshCanvas() {
        canvas->setData(frequencies,
                        current,
                        baseline,
                        upper,
                        lower,
                        minimum->value(),
                        maximum->value(),
                        amplitudeUnit);
    }

    void captureBaseline() {
        if (current.empty()) {
            status->setText(text(QStringLiteral("research_mask_waiting"),
                                 QStringLiteral("Waiting for spectrum data")));
            return;
        }
        baseline = current;
        baselineFirstHz = frequencies.empty() ? 0.0 : frequencies.front();
        baselineLastHz = frequencies.empty() ? 0.0 : frequencies.back();
        upper.resize(baseline.size());
        lower.resize(baseline.size());
        for (std::size_t i = 0; i < baseline.size(); ++i) {
            upper[i] = baseline[i] + static_cast<float>(upperMargin->value());
            lower[i] = baseline[i] - static_cast<float>(lowerMargin->value());
        }
        failed = false;
        refreshCanvas();
    }

    void resetMasks() {
        if (baseline.empty()) return;
        for (std::size_t i = 0; i < baseline.size(); ++i) {
            upper[i] = baseline[i] + static_cast<float>(upperMargin->value());
            lower[i] = baseline[i] - static_cast<float>(lowerMargin->value());
        }
        failed = false;
        refreshCanvas();
    }

    void clearBaseline() {
        baseline.clear();
        upper.clear();
        lower.clear();
        failed = false;
        refreshCanvas();
        status->setText(text(QStringLiteral("research_mask_no_baseline"),
                             QStringLiteral("Capture a baseline to enable mask comparison")));
    }

    void editMask(int encodedIndex, float level) {
        const bool isUpper = encodedIndex >= 0;
        const int index = isUpper ? encodedIndex : -encodedIndex - 1;
        std::vector<float> &mask = isUpper ? upper : lower;
        if (index < 0 || index >= static_cast<int>(mask.size())) return;
        const int radius = (std::max)(1, static_cast<int>(mask.size() / 512));
        for (int offset = -radius; offset <= radius; ++offset) {
            const int target = index + offset;
            if (target < 0 || target >= static_cast<int>(mask.size())) continue;
            const float mix = 1.0f - std::abs(offset) / static_cast<float>(radius + 1);
            mask[static_cast<std::size_t>(target)] =
                mask[static_cast<std::size_t>(target)] * (1.0f - mix) + level * mix;
            if (isUpper && !lower.empty()) {
                mask[static_cast<std::size_t>(target)] =
                    (std::max)(mask[static_cast<std::size_t>(target)],
                               lower[static_cast<std::size_t>(target)] + 0.1f);
            } else if (!isUpper && !upper.empty()) {
                mask[static_cast<std::size_t>(target)] =
                    (std::min)(mask[static_cast<std::size_t>(target)],
                               upper[static_cast<std::size_t>(target)] - 0.1f);
            }
        }
        refreshCanvas();
    }

    void exportCsv() {
        if (frequencies.empty() || current.empty()) return;
        const QString path = QFileDialog::getSaveFileName(
            owner,
            text(QStringLiteral("research_mask_export"),
                 QStringLiteral("Export spectrum mask")),
            QStringLiteral("spectrum-mask-%1.csv")
                .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-hhmmss"))),
            QStringLiteral("CSV (*.csv)"));
        if (path.isEmpty()) return;
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) return;
        QTextStream stream(&file);
        stream.setRealNumberNotation(QTextStream::ScientificNotation);
        stream << "frequency_hz,current_db,baseline_db,lower_mask_db,upper_mask_db,result\n";
        for (std::size_t i = 0; i < current.size(); ++i) {
            const bool hasMask = i < baseline.size() && i < upper.size() && i < lower.size();
            const bool violation = hasMask &&
                                   (current[i] > upper[i] || current[i] < lower[i]);
            stream << frequencies[i] << ',' << current[i] << ',';
            if (hasMask) {
                stream << baseline[i] << ',' << lower[i] << ',' << upper[i] << ','
                       << (violation ? "FAIL" : "PASS") << '\n';
            } else {
                stream << ",,,NO_BASELINE\n";
            }
        }
    }
};

SpectrumMaskWidget::SpectrumMaskWidget(Translator translator,
                                       TriggerHandler triggerHandler,
                                       QWidget *parent)
    : QWidget(parent), impl(std::make_unique<Impl>()) {
    impl->translator = std::move(translator);
    impl->triggerHandler = std::move(triggerHandler);
    impl->owner = this;
    QVBoxLayout *root = new QVBoxLayout(this);
    QGridLayout *controls = new QGridLayout();
    impl->upperMargin = new QDoubleSpinBox(this);
    impl->upperMargin->setRange(0.1, 100.0);
    impl->upperMargin->setSuffix(QStringLiteral(" dB"));
    impl->lowerMargin = new QDoubleSpinBox(this);
    impl->lowerMargin->setRange(0.1, 100.0);
    impl->lowerMargin->setSuffix(QStringLiteral(" dB"));
    impl->minimum = new QDoubleSpinBox(this);
    impl->minimum->setRange(-240.0, 80.0);
    impl->minimum->setSuffix(QStringLiteral(" dB"));
    impl->maximum = new QDoubleSpinBox(this);
    impl->maximum->setRange(-240.0, 100.0);
    impl->maximum->setSuffix(QStringLiteral(" dB"));
    impl->editMode = new QComboBox(this);
    impl->editMode->addItem(impl->text(QStringLiteral("research_mask_edit_upper"),
                                       QStringLiteral("Edit upper mask")), 1);
    impl->editMode->addItem(impl->text(QStringLiteral("research_mask_edit_lower"),
                                       QStringLiteral("Edit lower mask")), 0);
    impl->interval = new QSpinBox(this);
    impl->interval->setRange(10, 2000);
    impl->interval->setSuffix(QStringLiteral(" ms"));
    impl->cooldown = new QSpinBox(this);
    impl->cooldown->setRange(0, 3600);
    impl->cooldown->setPrefix(
        impl->text(QStringLiteral("research_mask_cooldown"), QStringLiteral("Cooldown: ")));
    impl->cooldown->setSuffix(QStringLiteral(" s"));
    impl->paused = new QCheckBox(impl->text(QStringLiteral("pause"), QStringLiteral("Pause")), this);
    impl->triggerEnabled = new QCheckBox(
        impl->text(QStringLiteral("research_mask_trigger"), QStringLiteral("Record on fail")), this);
    QPushButton *captureButton = new QPushButton(
        impl->text(QStringLiteral("research_mask_capture"), QStringLiteral("Capture baseline")), this);
    QPushButton *resetButton = new QPushButton(
        impl->text(QStringLiteral("research_mask_reset"), QStringLiteral("Reset masks")), this);
    QPushButton *clearButton = new QPushButton(
        impl->text(QStringLiteral("research_mask_clear"), QStringLiteral("Clear baseline")), this);
    QPushButton *exportButton = new QPushButton(
        impl->text(QStringLiteral("research_export_csv"), QStringLiteral("Export CSV")), this);

    controls->addWidget(new QLabel(impl->text(QStringLiteral("research_mask_upper_margin"),
                                               QStringLiteral("Upper margin:")), this), 0, 0);
    controls->addWidget(impl->upperMargin, 0, 1);
    controls->addWidget(new QLabel(impl->text(QStringLiteral("research_mask_lower_margin"),
                                               QStringLiteral("Lower margin:")), this), 0, 2);
    controls->addWidget(impl->lowerMargin, 0, 3);
    controls->addWidget(impl->editMode, 0, 4);
    controls->addWidget(impl->paused, 0, 5);
    controls->addWidget(new QLabel(impl->text(QStringLiteral("minimum"), QStringLiteral("Minimum:")), this), 1, 0);
    controls->addWidget(impl->minimum, 1, 1);
    controls->addWidget(new QLabel(impl->text(QStringLiteral("maximum"), QStringLiteral("Maximum:")), this), 1, 2);
    controls->addWidget(impl->maximum, 1, 3);
    controls->addWidget(new QLabel(impl->text(QStringLiteral("research_density_interval"),
                                               QStringLiteral("Update:")), this), 1, 4);
    controls->addWidget(impl->interval, 1, 5);
    controls->addWidget(captureButton, 2, 0);
    controls->addWidget(resetButton, 2, 1);
    controls->addWidget(clearButton, 2, 2);
    controls->addWidget(exportButton, 2, 3);
    controls->addWidget(impl->triggerEnabled, 2, 4);
    controls->addWidget(impl->cooldown, 2, 5);
    controls->setColumnStretch(6, 1);
    root->addLayout(controls);
    impl->status = new QLabel(this);
    root->addWidget(impl->status);
    impl->canvas = new SpectrumMaskCanvas(this);
    root->addWidget(impl->canvas, 1);

    QSettings settings(persistentSettingsFilePath(), QSettings::IniFormat);
    settings.beginGroup(QStringLiteral("researchMask"));
    impl->upperMargin->setValue(settings.value(QStringLiteral("upperMarginDb"), 6.0).toDouble());
    impl->lowerMargin->setValue(settings.value(QStringLiteral("lowerMarginDb"), 12.0).toDouble());
    impl->minimum->setValue(settings.value(QStringLiteral("minimumDb"), -140.0).toDouble());
    impl->maximum->setValue(settings.value(QStringLiteral("maximumDb"), -20.0).toDouble());
    impl->interval->setValue(settings.value(QStringLiteral("intervalMs"), 50).toInt());
    impl->cooldown->setValue(settings.value(QStringLiteral("cooldownSeconds"), 10).toInt());
    impl->triggerEnabled->setChecked(settings.value(QStringLiteral("triggerEnabled"), false).toBool());
    settings.endGroup();
    impl->updateClock.start();
    impl->status->setText(impl->text(QStringLiteral("research_mask_no_baseline"),
                                     QStringLiteral("Capture a baseline to enable mask comparison")));
    impl->canvas->setEditHandler([this](int index, float level) { impl->editMask(index, level); });
    impl->canvas->setEditUpper(true);

    connect(captureButton, &QPushButton::clicked, this, [this]() { impl->captureBaseline(); });
    connect(resetButton, &QPushButton::clicked, this, [this]() { impl->resetMasks(); });
    connect(clearButton, &QPushButton::clicked, this, [this]() { impl->clearBaseline(); });
    connect(exportButton, &QPushButton::clicked, this, [this]() { impl->exportCsv(); });
    connect(impl->editMode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        impl->canvas->setEditUpper(impl->editMode->currentData().toInt() != 0);
    });
    auto saveAndRefresh = [this]() {
        if (impl->maximum->value() <= impl->minimum->value() + 1.0) {
            impl->maximum->setValue(impl->minimum->value() + 1.0);
        }
        impl->saveSettings();
        impl->refreshCanvas();
    };
    connect(impl->minimum, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [saveAndRefresh](double) { saveAndRefresh(); });
    connect(impl->maximum, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [saveAndRefresh](double) { saveAndRefresh(); });
    connect(impl->upperMargin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double) { impl->saveSettings(); });
    connect(impl->lowerMargin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double) { impl->saveSettings(); });
    connect(impl->interval, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int) { impl->saveSettings(); });
    connect(impl->cooldown, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int) { impl->saveSettings(); });
    connect(impl->triggerEnabled, &QCheckBox::toggled, this, [this](bool) { impl->saveSettings(); });
}

SpectrumMaskWidget::~SpectrumMaskWidget() = default;

void SpectrumMaskWidget::appendSpectrumFrame(const std::vector<float> &frequencies,
                                             const std::vector<float> &levels,
                                             int amplitudeUnit) {
    if (!impl || impl->paused->isChecked() || frequencies.empty() || levels.empty()) return;
    if (impl->updateClock.isValid() && impl->updateClock.elapsed() < impl->interval->value()) return;
    impl->updateClock.restart();
    const std::size_t sourceCount = (std::min)(frequencies.size(), levels.size());
    const int displayBins = (std::min)(2048, static_cast<int>(sourceCount));
    const std::vector<float> sourceLevels(
        levels.begin(), levels.begin() + static_cast<std::ptrdiff_t>(sourceCount));
    impl->current = resampleLevels(sourceLevels, displayBins);
    impl->frequencies.resize(static_cast<std::size_t>(displayBins));
    for (int i = 0; i < displayBins; ++i) {
        const std::size_t sourceIndex = displayBins > 1
                                            ? static_cast<std::size_t>(
                                                  (sourceCount - 1) * i /
                                                  static_cast<std::size_t>(displayBins - 1))
                                            : 0;
        impl->frequencies[static_cast<std::size_t>(i)] = frequencies[sourceIndex];
    }
    impl->amplitudeUnit = amplitudeUnit;
    if (!impl->baseline.empty()) {
        const double span = std::abs(impl->frequencies.back() - impl->frequencies.front());
        const double tolerance = (std::max)(1.0, span * 1.0e-4);
        if (impl->baseline.size() != impl->current.size() ||
            std::abs(impl->frequencies.front() - impl->baselineFirstHz) > tolerance ||
            std::abs(impl->frequencies.back() - impl->baselineLastHz) > tolerance) {
            impl->clearBaseline();
        }
    }

    int aboveCount = 0;
    int belowCount = 0;
    float maximumDeviation = 0.0f;
    if (impl->baseline.size() == impl->current.size()) {
        for (std::size_t i = 0; i < impl->current.size(); ++i) {
            if (impl->current[i] > impl->upper[i]) {
                ++aboveCount;
                maximumDeviation = (std::max)(maximumDeviation,
                                              impl->current[i] - impl->upper[i]);
            } else if (impl->current[i] < impl->lower[i]) {
                ++belowCount;
                maximumDeviation = (std::max)(maximumDeviation,
                                              impl->lower[i] - impl->current[i]);
            }
        }
        const bool nowFailed = aboveCount > 0 || belowCount > 0;
        if (nowFailed) {
            impl->status->setText(
                impl->text(QStringLiteral("research_mask_fail"),
                           QStringLiteral("FAIL | above %1 | below %2 | max deviation %3 dB"))
                    .arg(aboveCount)
                    .arg(belowCount)
                    .arg(maximumDeviation, 0, 'f', 2));
        } else {
            impl->status->setText(
                impl->text(QStringLiteral("research_mask_pass"),
                           QStringLiteral("PASS | no mask violations")));
        }
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const qint64 cooldownMs = static_cast<qint64>(impl->cooldown->value()) * 1000;
        if (nowFailed && impl->triggerEnabled->isChecked() && impl->triggerHandler &&
            (!impl->failed || now - impl->lastTriggerMs >= cooldownMs)) {
            impl->lastTriggerMs = now;
            impl->triggerHandler();
        }
        impl->failed = nowFailed;
    } else {
        impl->status->setText(
            impl->text(QStringLiteral("research_mask_no_baseline"),
                       QStringLiteral("Capture a baseline to enable mask comparison")));
    }
    impl->refreshCanvas();
}

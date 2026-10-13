#include "dspflowpanel.h"
#include "dspcomputedialog.h"
#include "dspresearchwidget.h"
#include "dspbranchengine.h"
#include "finetunewidget.h"
#include "MyWaterfallWidget.h"
#include "appsettingsutils.h"
#include "radiosettings.h"

#include <QAction>
#include <QComboBox>
#include <QCheckBox>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QGraphicsPathItem>
#include <QGraphicsProxyWidget>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsSceneContextMenuEvent>
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsView>
#include <QHBoxLayout>
#include <QHash>
#include <QInputDialog>
#include <QImage>
#include <QElapsedTimer>
#include <QFrame>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QLinearGradient>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPathStroker>
#include <QPointer>
#include <QPushButton>
#include <QRegion>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSet>
#include <QSettings>
#include <QSignalBlocker>
#include <QShowEvent>
#include <QStyle>
#include <QStyleOptionGraphicsItem>
#include <cstring>
#include <QTimer>
#include <QToolButton>
#include <QUuid>
#include <QVarLengthArray>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtMath>

#include <algorithm>
#include <array>
#include <functional>
#include <limits>
#include <deque>

namespace {

constexpr qreal kBlockWidth = 172.0;
constexpr qreal kBlockHeight = 68.0;
constexpr qreal kMiniBlockWidth = 104.0;
constexpr qreal kMiniBlockHeight = 40.0;
constexpr qreal kFineTuneBlockWidth = 260.0;
constexpr qreal kFineTuneBlockHeight = 108.0;
constexpr qreal kVfoDisplayBlockWidth = 292.0;
constexpr qreal kVfoDisplayBlockHeight = 194.0;
constexpr qreal kVisualBlockMinWidth = 220.0;
constexpr qreal kVisualBlockMinHeight = 96.0;
constexpr qreal kVisualBlockMaxWidth = 2000.0;
constexpr qreal kVisualBlockMaxHeight = 1400.0;
constexpr qreal kPortRadius = 6.0;
constexpr int kWorkspacePlotLeftMargin = 42;
constexpr int kWorkspacePlotRightMargin = 8;

QColor workspaceSpectrumColor(float value) {
    value = std::clamp(std::isfinite(value) ? value : 0.0f, 0.0f, 1.0f);
    int red = 0;
    int green = 0;
    int blue = 0;
    if (value < 0.12f) {
        blue = int(255.0f * (value / 0.2f));
    } else if (value < 0.26f) {
        const float ratio = (value - 0.12f) / 0.2f;
        green = int(255.0f * (1.0f - ratio));
        blue = 255;
    } else if (value < 0.40f) {
        const float ratio = (value - 0.28f) / 0.2f;
        green = 255;
        blue = int(255.0f * (1.0f - ratio));
    } else if (value < 0.54f) {
        red = int(255.0f * ((value - 0.40f) / 0.2f));
        green = 255;
    } else if (value < 0.68f) {
        red = 255;
        green = int(255.0f * (1.0f - 0.5f * ((value - 0.54f) / 0.2f)));
    } else if (value < 0.86f) {
        red = 255;
        green = int(128.0f * (1.0f - 0.5f * ((value - 0.68f) / 0.2f)));
    } else {
        red = 255;
        blue = int(255.0f * (1.0f - ((value - 0.86f) / 0.2f)));
    }
    return QColor(std::clamp(red, 0, 255), std::clamp(green, 0, 255), std::clamp(blue, 0, 255));
}

QColor workspaceWaterfallColor(float normalizedValue, float contrast, float sensitivity) {
    static const std::array<QColor, 17> palette = {{
        QColor("#000020"), QColor("#000050"), QColor("#000090"), QColor("#0000F0"),
        QColor("#0000FF"), QColor("#50F030"), QColor("#1E90FF"), QColor("#FFFFFF"),
        QColor("#FFFF00"), QColor("#FE6D16"), QColor("#FE6D16"), QColor("#FF0000"),
        QColor("#FF0000"), QColor("#C60000"), QColor("#9F0000"), QColor("#750000"),
        QColor("#4A0000")
    }};
    const float value = std::clamp((std::isfinite(normalizedValue) ? normalizedValue : 0.0f) *
                                       sensitivity / 10.0f,
                                   0.0f, 1.0f);
    const QColor base = palette[std::size_t(std::clamp(int(value * float(palette.size() - 1)),
                                                       0, int(palette.size() - 1)))];
    const float contrastFactor = contrast / 10.0f;
    const int blue = (base.green() + base.blue()) / 3;
    return QColor(int(base.red() * contrastFactor + blue * (1.0f - contrastFactor)),
                  int(base.green() * contrastFactor + blue * (1.0f - contrastFactor)),
                  int(base.blue() * contrastFactor + blue * (1.0f - contrastFactor)));
}
struct DspWorkspaceProfile {
    QString name;
    QString configuration;
};

QVector<DspWorkspaceProfile> loadDspWorkspaceProfiles() {
    QSettings settings(persistentSettingsFilePath(), QSettings::IniFormat);
    QVector<DspWorkspaceProfile> profiles;
    const int count = settings.beginReadArray(QStringLiteral("dspFlow/profiles"));
    profiles.reserve(count);
    for (int index = 0; index < count; ++index) {
        settings.setArrayIndex(index);
        DspWorkspaceProfile profile;
        profile.name = settings.value(QStringLiteral("name")).toString().trimmed();
        profile.configuration = settings.value(QStringLiteral("configuration")).toString();
        if (!profile.name.isEmpty() && !profile.configuration.isEmpty()) profiles.push_back(profile);
    }
    settings.endArray();
    std::sort(profiles.begin(), profiles.end(), [](const auto &left, const auto &right) {
        return left.name.compare(right.name, Qt::CaseInsensitive) < 0;
    });
    return profiles;
}

void storeDspWorkspaceProfiles(const QVector<DspWorkspaceProfile> &profiles) {
    QSettings settings(persistentSettingsFilePath(), QSettings::IniFormat);
    settings.remove(QStringLiteral("dspFlow/profiles"));
    settings.beginWriteArray(QStringLiteral("dspFlow/profiles"), profiles.size());
    for (int index = 0; index < profiles.size(); ++index) {
        settings.setArrayIndex(index);
        settings.setValue(QStringLiteral("name"), profiles[index].name);
        settings.setValue(QStringLiteral("configuration"), profiles[index].configuration);
    }
    settings.endArray();
    settings.sync();
}

class DspConnectionItem;

bool isResearchSettingsType(const QString &type) {
    return type == QStringLiteral("oscilloscope") || type == QStringLiteral("constellation") ||
           type == QStringLiteral("eye_diagram") || type == QStringLiteral("digital_sync_lab");
}

bool isResearchViewType(const QString &type) {
    return type == QStringLiteral("oscilloscope_view") || type == QStringLiteral("constellation_view") ||
           type == QStringLiteral("eye_diagram_view") || type == QStringLiteral("digital_sync_view");
}

bool isWorkspaceDisplaySettingsType(const QString &type) {
    return type == QStringLiteral("spectrum_display") ||
           type == QStringLiteral("waterfall_2d") ||
           type == QStringLiteral("waterfall_3d");
}

bool workspaceSettingsMatchView(const QString &settingsType, const QString &viewType) {
    if (viewType == QStringLiteral("workspace_spectrum"))
        return settingsType == QStringLiteral("spectrum_display");
    if (viewType == QStringLiteral("workspace_waterfall"))
        return settingsType == QStringLiteral("waterfall_2d") ||
               settingsType == QStringLiteral("waterfall_3d");
    return false;
}

QString settingsTypeForResearchView(const QString &type) {
    if (type == QStringLiteral("oscilloscope_view")) return QStringLiteral("oscilloscope");
    if (type == QStringLiteral("constellation_view")) return QStringLiteral("constellation");
    if (type == QStringLiteral("eye_diagram_view")) return QStringLiteral("eye_diagram");
    if (type == QStringLiteral("digital_sync_view")) return QStringLiteral("digital_sync_lab");
    return QString();
}

DspResearchWidget::Mode researchModeForView(const QString &type) {
    if (type == QStringLiteral("constellation_view")) return DspResearchWidget::Mode::Constellation;
    if (type == QStringLiteral("eye_diagram_view")) return DspResearchWidget::Mode::EyeDiagram;
    if (type == QStringLiteral("digital_sync_view")) return DspResearchWidget::Mode::Synchronization;
    return DspResearchWidget::Mode::Oscilloscope;
}

class MiniVfoDisplay final : public QWidget {
public:
    explicit MiniVfoDisplay(bool waterfall, QWidget *parent = nullptr)
        : QWidget(parent), waterfall_(waterfall), waterfallImage_(256, 104, QImage::Format_RGB32) {
        setAttribute(Qt::WA_OpaquePaintEvent);
        setMinimumSize(270, 138);
        waterfallImage_.fill(QColor(4, 7, 12));
        updateTimer_.start();
    }

    void setSpectrum(const std::vector<float> &frequencies,
                     const std::vector<float> &levels,
                     double centerHz,
                     double bandwidthHz,
                     const QString &name,
                     bool levelsShifted = false) {
        if (frequencies.empty() || frequencies.size() != levels.size() ||
            !std::isfinite(centerHz) || !std::isfinite(bandwidthHz) || bandwidthHz <= 0.0) return;
        if (!levelsShifted && updateTimer_.elapsed() < 55) return;
        updateTimer_.restart();
        name_ = name;
        centerHz_ = centerHz;
        bandwidthHz_ = bandwidthHz;
        const double low = centerHz - bandwidthHz * 0.5;
        const double high = centerHz + bandwidthHz * 0.5;
        if (high < frequencies.front() || low > frequencies.back()) {
            setStatus(QStringLiteral("VFO outside visible span"));
            return;
        }
        auto begin = std::lower_bound(frequencies.begin(), frequencies.end(), static_cast<float>(low));
        auto end = std::upper_bound(frequencies.begin(), frequencies.end(), static_cast<float>(high));
        if (begin == end) {
            begin = std::lower_bound(frequencies.begin(), frequencies.end(), static_cast<float>(centerHz));
            if (begin == frequencies.end()) --begin;
            end = begin + 1;
        }
        status_.clear();
        const std::size_t first = static_cast<std::size_t>(std::distance(frequencies.begin(), begin));
        const std::size_t last = static_cast<std::size_t>(std::distance(frequencies.begin(), end));
        constexpr int bins = 256;
        displayLevels_.assign(bins, -160.0f);
        for (int pixel = 0; pixel < bins; ++pixel) {
            const std::size_t sourceBegin = first + (last - first) * static_cast<std::size_t>(pixel) / bins;
            const std::size_t sourceEnd = first + (last - first) * static_cast<std::size_t>(pixel + 1) / bins;
            for (std::size_t index = sourceBegin; index < (std::max)(sourceBegin + 1U, sourceEnd) && index < levels.size(); ++index) {
                const std::size_t shiftedIndex = levelsShifted ? index : (index + levels.size() / 2U) % levels.size();
                if (std::isfinite(levels[shiftedIndex])) displayLevels_[static_cast<std::size_t>(pixel)] =
                    (std::max)(displayLevels_[static_cast<std::size_t>(pixel)], levels[shiftedIndex]);
            }
        }
        if (waterfall_) {
            for (int y = waterfallImage_.height() - 1; y > 0; --y) {
                std::memcpy(waterfallImage_.scanLine(y), waterfallImage_.constScanLine(y - 1),
                            static_cast<std::size_t>(waterfallImage_.bytesPerLine()));
            }
            const float levelSpan = maximumDbfs_ - minimumDbfs_;
            const auto colorForLevel = [this, levelSpan](float level) {
                const float value = std::clamp((level - minimumDbfs_) / levelSpan, 0.0f, 1.0f);
                const int red = int(255.0f * std::clamp((value - 0.50f) * 2.0f, 0.0f, 1.0f));
                const int green = int(255.0f * std::clamp(1.0f - std::abs(value - 0.55f) * 2.2f, 0.0f, 1.0f));
                const int blue = int(255.0f * std::clamp(1.0f - value * 1.35f, 0.0f, 1.0f));
                return qRgb(red, green, blue);
            };
            QRgb *line = reinterpret_cast<QRgb *>(waterfallImage_.scanLine(0));
            for (int x = 0; x < waterfallImage_.width(); ++x) line[x] = colorForLevel(displayLevels_[static_cast<std::size_t>(x)]);
        }
        update();
    }

    void setStatus(const QString &status) {
        if (status_ == status) return;
        status_ = status;
        displayLevels_.clear();
        waterfallImage_.fill(QColor(4, 7, 12));
        update();
    }

    void setDbfsRange(float minimumDbfs, float maximumDbfs) {
        minimumDbfs = std::clamp(minimumDbfs, -200.0f, 19.0f);
        maximumDbfs = std::clamp(maximumDbfs, minimumDbfs + 1.0f, 20.0f);
        if (qFuzzyCompare(minimumDbfs_ + 201.0f, minimumDbfs + 201.0f) &&
            qFuzzyCompare(maximumDbfs_ + 201.0f, maximumDbfs + 201.0f)) {
            return;
        }
        minimumDbfs_ = minimumDbfs;
        maximumDbfs_ = maximumDbfs;
        waterfallImage_.fill(QColor(4, 7, 12));
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(5, 8, 13));
        const QRect plot = rect().adjusted(5, 18, -5, -18);
        painter.setPen(QPen(QColor(38, 48, 60), 1.0));
        for (int i = 1; i < 4; ++i) painter.drawLine(plot.left(), plot.top() + plot.height() * i / 4,
                                                    plot.right(), plot.top() + plot.height() * i / 4);
        if (waterfall_) {
            painter.drawImage(plot, waterfallImage_);
        } else if (!displayLevels_.empty()) {
            QPolygonF line;
            line.reserve(static_cast<int>(displayLevels_.size()));
            for (std::size_t index = 0; index < displayLevels_.size(); ++index) {
                const qreal x = plot.left() + plot.width() * qreal(index) / qreal(displayLevels_.size() - 1U);
                const qreal normalized = std::clamp(
                    (displayLevels_[index] - minimumDbfs_) / (maximumDbfs_ - minimumDbfs_),
                    0.0f,
                    1.0f);
                line << QPointF(x, plot.bottom() - normalized * plot.height());
            }
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.setPen(QPen(QColor(73, 220, 145), 1.4));
            painter.drawPolyline(line);
        }
        if (!status_.isEmpty()) {
            painter.setPen(QColor(176, 186, 198));
            painter.drawText(plot, Qt::AlignCenter | Qt::TextWordWrap, status_);
        }
        painter.setPen(QColor(220, 226, 234));
        const QString heading = QStringLiteral("%1  %2 MHz / %3 kHz").arg(name_)
            .arg(centerHz_ / 1.0e6, 0, 'f', 6).arg(bandwidthHz_ / 1000.0, 0, 'f', 1);
        painter.drawText(QRect(5, 1, width() - 10, 16), Qt::AlignLeft | Qt::AlignVCenter,
                         painter.fontMetrics().elidedText(heading, Qt::ElideRight, width()-10));
        painter.setPen(QColor(145, 157, 171));
        painter.drawText(QRect(5, height() - 17, width() - 10, 16), Qt::AlignLeft | Qt::AlignVCenter,
                         QStringLiteral("%1").arg((centerHz_ - bandwidthHz_ * 0.5) / 1.0e6, 0, 'f', 6));
        painter.drawText(QRect(5, height() - 17, width() - 10, 16), Qt::AlignRight | Qt::AlignVCenter,
                         QStringLiteral("%1 MHz").arg((centerHz_ + bandwidthHz_ * 0.5) / 1.0e6, 0, 'f', 6));
    }

private:
    bool waterfall_ = false;
    QImage waterfallImage_;
    std::vector<float> displayLevels_;
    QString name_ = QStringLiteral("VFO");
    double centerHz_ = 0.0;
    double bandwidthHz_ = 0.0;
    QString status_ = QStringLiteral("Connect VFO channel");
    QElapsedTimer updateTimer_;
    float minimumDbfs_ = -140.0f;
    float maximumDbfs_ = -40.0f;
};

struct WorkspaceDisplayCallbacks {
    std::function<void(int)> scale;
    std::function<void(int, int)> pan;
    std::function<void(double, const QPoint &)> tuneContext;
    std::function<void(double)> autoTune;
    std::function<void(double)> scienceMarker;
    std::function<void(double, double)> multiVfoSelection;
    std::function<void(double)> listeningFrequency;
    std::function<void(double)> centerFrequency;
    std::function<void(double, double)> tuning;
    std::function<void(const QString &)> fillGroup;
    std::function<void(const QString &)> openSettings;
    std::function<void(const QString &, bool)> pauseToggled;
    std::function<void(bool)> analogMeterToggled;
    std::function<void(int)> analogMeterStyleChanged;
    std::function<bool()> ukrainian;
};

class WorkspaceSpectrumDisplay final : public QWidget {
public:
    enum class Mode { Spectrum, Ruler, Waterfall };

    explicit WorkspaceSpectrumDisplay(Mode mode, QString blockId, QWidget *parent = nullptr)
        : QWidget(parent), mode_(mode), blockId_(std::move(blockId)) {
        setAttribute(Qt::WA_OpaquePaintEvent);
        setMinimumSize(120, mode_ == Mode::Ruler ? 48 : 80);
        setMouseTracking(true);
        setFocusPolicy(Qt::StrongFocus);
        if (mode_ != Mode::Ruler) {
            pauseControl_ = new QToolButton(this);
            pauseControl_->setAutoRaise(true);
            pauseControl_->setCheckable(true);
            pauseControl_->setFixedSize(24, 24);
            pauseControl_->setStyleSheet(QStringLiteral(
                "QToolButton { background:rgba(5,8,13,185); border:1px solid rgba(150,165,180,150);"
                " border-radius:3px; }"
                "QToolButton:checked { background:rgba(132,72,18,220); border-color:#ffc44e; }"));
            QObject::connect(pauseControl_, &QToolButton::toggled, pauseControl_,
                             [this](bool paused) {
                paused_ = paused;
                updatePauseControl();
                if (callbacks_.pauseToggled) callbacks_.pauseToggled(blockId_, paused);
                update();
            });
            updatePauseControl();
        }
        updateTimer_.start();
    }

    void setCallbacks(const WorkspaceDisplayCallbacks &callbacks) { callbacks_ = callbacks; }
    void setAnalogPeakMeterEnabled(bool enabled) {
        analogPeakMeterEnabled_ = enabled;
        update();
    }
    void setAnalogPeakMeterStyle(int style) {
        analogPeakMeterStyle_ = std::clamp(style, 0, 1);
        update();
    }
    void setAnalogPeakMeterTarget(double frequencyHz, bool valid) {
        analogPeakMeterTargetValid_ = valid && std::isfinite(frequencyHz);
        analogPeakMeterTargetHz_ = analogPeakMeterTargetValid_ ? frequencyHz : 0.0;
        update();
    }

    void setSettings(const QJsonObject &settings) {
        localSettings_ = settings;
        paused_ = localSettings_.value(QStringLiteral("paused")).toBool(false);
        if (pauseControl_) {
            const QSignalBlocker blocker(pauseControl_);
            pauseControl_->setChecked(paused_);
            updatePauseControl();
        }
        applyEffectiveSettings();
    }

    void setVisualizationSettings(const QJsonObject &settings) {
        globalVisualizationSettings_ = settings;
        applyEffectiveSettings();
    }

    void setControllerSettings(const QJsonObject &settings) {
        controllerSettings_ = settings;
        applyEffectiveSettings();
    }

    void applyEffectiveSettings() {
        const auto value = [this](const QString &key, const QJsonValue &fallback) {
            if (controllerSettings_.contains(key)) return controllerSettings_.value(key);
            if (localSettings_.contains(key)) return localSettings_.value(key);
            if (globalVisualizationSettings_.contains(key)) return globalVisualizationSettings_.value(key);
            return fallback;
        };

        globalWaterfallDisplayMode_ = std::clamp(
            globalVisualizationSettings_.value(QStringLiteral("displayMode")).toInt(0), 0, 2);
        waterfallDisplayMode_ = std::clamp(
            value(QStringLiteral("displayMode"), globalWaterfallDisplayMode_).toInt(), 0, 2);
        waterfallResolutionDivisor_ = std::clamp(
            value(QStringLiteral("resolutionDivisor"), 4).toInt(), 1, 64);
        waterfallHistoryLimit_ = std::clamp(
            value(QStringLiteral("historyRows"), 128).toInt(), 16, 2048);

        globalMinimumDbfs_ = static_cast<float>((std::clamp)(
            globalVisualizationSettings_.value(QStringLiteral("minimumDbfs")).toDouble(-140.0),
            -200.0, 19.0));
        globalMaximumDbfs_ = static_cast<float>((std::clamp)(
            globalVisualizationSettings_.value(QStringLiteral("maximumDbfs")).toDouble(-30.0),
            double(globalMinimumDbfs_ + 1.0f), 20.0));
        const bool controllerLevelOverride =
            controllerSettings_.value(QStringLiteral("levelOverride")).toBool(false);
        const bool localLevelOverride =
            localSettings_.value(QStringLiteral("levelOverride")).toBool(false);
        const QJsonObject *levelSource = controllerLevelOverride
                                             ? &controllerSettings_
                                             : (localLevelOverride ? &localSettings_ : nullptr);
        hasCustomLevelRange_ = levelSource != nullptr;
        minimumDbfs_ = hasCustomLevelRange_
            ? static_cast<float>((std::clamp)(
                  levelSource->value(QStringLiteral("minimumDbfs")).toDouble(globalMinimumDbfs_),
                  -200.0, 19.0))
            : globalMinimumDbfs_;
        maximumDbfs_ = hasCustomLevelRange_
            ? static_cast<float>((std::clamp)(
                  levelSource->value(QStringLiteral("maximumDbfs")).toDouble(globalMaximumDbfs_),
                  double(minimumDbfs_ + 1.0f), 20.0))
            : globalMaximumDbfs_;

        contrast_ = static_cast<float>((std::clamp)(
            value(QStringLiteral("contrast"), 10.0).toDouble(), 1.0, 20.0));
        sensitivity_ = static_cast<float>((std::clamp)(
            value(QStringLiteral("sensitivity"), 10.0).toDouble(), 1.0, 30.0));
        colorSpectrum_ = value(QStringLiteral("colorSpectrum"), true).toBool();
        spectrumGradientFill_ =
            value(QStringLiteral("spectrumGradientFill"), false).toBool();
        spectrumGradientOpacity_ = std::clamp(
            value(QStringLiteral("spectrumGradientOpacity"), 70).toInt(), 0, 100);
        secondSpectrum_ = value(QStringLiteral("secondSpectrum"), false).toBool();
        showSpectrumFps_ = value(QStringLiteral("showSpectrumFps"), false).toBool();
        showWaterfallFps_ = value(QStringLiteral("showWaterfallFps"), false).toBool();
        showExtendedInfo_ =
            value(QStringLiteral("showExtendedSpectrumInfo"), false).toBool();
        areaMeasurementEnabled_ =
            value(QStringLiteral("waterfallAreaMeasurementEnabled"), false).toBool();
        const bool nextFixedPlane =
            value(QStringLiteral("waterfall3DFixedPlane"), false).toBool();
        if (waterfall3DFixedPlane_ != nextFixedPlane) {
            cameraOverrideActive_ = false;
            cameraYaw_ = 0.0;
            cameraTilt_ = 0.68;
            cameraZoom_ = 1.0;
            cameraPanX_ = 0.0;
            cameraPanY_ = 0.0;
        }
        waterfall3DFixedPlane_ = nextFixedPlane;
        waterfall3DMonochrome_ =
            value(QStringLiteral("waterfall3DMonochrome"), false).toBool();
        waterfall3DSurfaceStyle_ = std::clamp(
            value(QStringLiteral("waterfall3DSurfaceStyle"), 0).toInt(), 0, 1);
        waterfall3DSmoothing_ = std::clamp(
            value(QStringLiteral("waterfall3DSmoothing"), 0).toInt(), 0, 2);
        waterfall3DLighting_ = std::clamp(
            value(QStringLiteral("waterfall3DLighting"), 0).toInt(), 0, 2);
        spectrumSliceCapture_ =
            value(QStringLiteral("waterfall3DSpectrumSliceCapture"), false).toBool();
        spectrumSliceCaptureFixed_ =
            value(QStringLiteral("waterfall3DSpectrumSliceCaptureFixed"), false).toBool();
        if (!spectrumSliceCapture_ || !spectrumSliceCaptureFixed_)
            capturedWaterfallHistory_.clear();
        modifierFreeSliceInput_ =
            value(QStringLiteral("waterfall3DVncSliceInput"), false).toBool();
        frequencySliceStep_ = std::clamp(
            value(QStringLiteral("waterfall3DSliceScrollStep"), 1).toInt(), 1, 256);
        frequencySliceWidth_ = std::clamp(
            value(QStringLiteral("waterfall3DSliceWidth"), 1).toInt(), 1, 4096);
        spectrumSliceStep_ = std::clamp(
            value(QStringLiteral("waterfall3DSpectrumSliceScrollStep"), 1).toInt(), 1, 2048);
        spectrumSliceRows_ = std::clamp(
            value(QStringLiteral("waterfall3DSpectrumSliceRows"), 1).toInt(), 1, 2048);
        sourceFftLength_ = (std::max)(
            0, value(QStringLiteral("fftLength"), 0).toInt());
        fftWindowType_ = normalizedFftWindowType(
            value(QStringLiteral("fftWindowType"), 0).toInt());
        if (!areaMeasurementEnabled_) {
            areaMeasurementActive_ = false;
            areaMeasurementVisible_ = false;
        }
        while (int(waterfallHistory_.size()) > waterfallHistoryLimit_)
            waterfallHistory_.pop_back();
        update();
    }
    void setReceiverRunning(bool running) {
        receiverRunning_ = running;
        update();
    }

    void setFrame(const std::vector<float> &frequencies,
                  const std::vector<float> &levels,
                  double centerHz,
                  double listeningHz,
                  double sampleRate,
                  double bandwidthHz,
                  int modulationType) {
        centerHz_ = centerHz;
        listeningHz_ = listeningHz;
        sampleRate_ = sampleRate;
        bandwidthHz_ = bandwidthHz;
        modulationType_ = modulationType;
        if (paused_ || frequencies.empty() || frequencies.size() != levels.size()) {
            update();
            return;
        }
        if (updateTimer_.isValid() && updateTimer_.elapsed() < 16) return;
        updateTimer_.restart();
        frequencies_ = frequencies;
        levels_ = levels;
        ++fpsFrameCount_;
        if (!fpsTimer_.isValid()) fpsTimer_.start();
        if (fpsTimer_.elapsed() >= 500) {
            displayedFps_ = 1000.0 * double(fpsFrameCount_) / double(fpsTimer_.elapsed());
            fpsFrameCount_ = 0;
            fpsTimer_.restart();
        }
        if (mode_ == Mode::Waterfall) {
            appendWaterfallRow();
            appendWaterfallHistory();
        }
        update();
    }

protected:
    void resizeEvent(QResizeEvent *event) override {
        QWidget::resizeEvent(event);
        if (pauseControl_) {
            pauseControl_->move((std::max)(2, width() - pauseControl_->width() - 7), 7);
            pauseControl_->raise();
        }
        if (mode_ == Mode::Waterfall) rebuildWaterfall();
    }

    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(4, 7, 12));
        const QRect plot = plotRect();
        if (plot.width() < 2 || plot.height() < 2) return;

        if (mode_ == Mode::Ruler) {
            drawRuler(painter, plot);
            drawInteractionOverlay(painter, plot);
            if (!receiverRunning_ || frequencies_.empty()) drawInactiveLabel(painter, plot);
            return;
        }

        if (mode_ == Mode::Waterfall && secondSpectrum_) {
            drawSpectrumTrace(painter, plot);
        } else if (mode_ == Mode::Waterfall && waterfallDisplayMode_ != 0 && !waterfallHistory_.empty()) {
            drawWaterfall3D(painter, plot);
            if (waterfallDisplayMode_ == 2) drawMiniWaterfall(painter, plot);
        } else if (mode_ == Mode::Waterfall && !waterfallImage_.isNull()) {
            const int h = waterfallImage_.height();
            const int firstRows = h - waterfallHead_;
            const int firstTarget = qRound(plot.height() * (double(firstRows) / h));
            painter.drawImage(QRect(plot.left(), plot.top(), plot.width(), firstTarget),
                              waterfallImage_,
                              QRect(0, waterfallHead_, waterfallImage_.width(), firstRows));
            if (waterfallHead_ > 0) {
                painter.drawImage(QRect(plot.left(), plot.top() + firstTarget,
                                        plot.width(), plot.height() - firstTarget),
                                  waterfallImage_,
                                  QRect(0, 0, waterfallImage_.width(), waterfallHead_));
            }
        } else if (!levels_.empty()) {
            drawSpectrumTrace(painter, plot);
        }

        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.setPen(QPen(QColor(42, 50, 61), 1));
        for (int row = 0; row <= 4; ++row) {
            const int y = plot.top() + plot.height() * row / 4;
            painter.drawLine(plot.left(), y, plot.right(), y);
        }
        painter.setPen(QColor(154, 166, 181));
        for (int row = 0; row <= 4; ++row) {
            const float level = maximumDbfs_ - (maximumDbfs_ - minimumDbfs_) * row / 4.0f;
            const int y = plot.top() + plot.height() * row / 4;
            painter.drawText(QRect(2, y - 8, kWorkspacePlotLeftMargin - 7, 16), Qt::AlignRight | Qt::AlignVCenter,
                             QString::number(qRound(level)));
        }
        drawListeningMarker(painter, plot);
        drawFrequencyEnds(painter, plot);
        drawInteractionOverlay(painter, plot);
        drawInformationOverlays(painter, plot);
        if (mode_ == Mode::Spectrum) drawAnalogPeakMeter(painter, plot);
        if (!receiverRunning_ || frequencies_.empty()) drawInactiveLabel(painter, plot);

    }

    void wheelEvent(QWheelEvent *event) override {
        const int direction = event->angleDelta().y() >= 0 ? 1 : -1;
        if (mode_ == Mode::Waterfall && waterfallDisplayMode_ != 0 && frequencySliceActive_) {
            const int columns = waterfallHistory_.empty()
                                    ? 1
                                    : int(waterfallHistory_.front().size());
            selectedFrequencySlice_ = std::clamp(
                selectedFrequencySlice_ + direction * double(frequencySliceStep_) /
                                              double((std::max)(1, columns - 1)),
                0.0, 1.0);
            update();
        } else if (mode_ == Mode::Waterfall && waterfallDisplayMode_ != 0 && spectrumSliceActive_) {
            selectedSpectrumRow_ = std::clamp(
                selectedSpectrumRow_ + direction * spectrumSliceStep_,
                0, (std::max)(0, int(waterfallHistory_.size()) - 1));
            update();
        } else if (mode_ == Mode::Waterfall && waterfallDisplayMode_ != 0 &&
                   event->modifiers().testFlag(Qt::ControlModifier)) {
            detachFixedPresentationForCamera();
            cameraZoom_ = std::clamp(cameraZoom_ * (direction > 0 ? 1.12 : 1.0 / 1.12), 0.35, 12.0);
            update();
        } else if (mode_ == Mode::Ruler && callbacks_.listeningFrequency && frequencySpan() > 0.0) {
            callbacks_.listeningFrequency(listeningHz_ + direction * frequencySpan() / 100.0);
        } else if (callbacks_.scale) {
            callbacks_.scale(direction);
        }
        event->accept();
    }

    void mousePressEvent(QMouseEvent *event) override {
        setFocus(Qt::MouseFocusReason);
        const bool sliceModifier = event->modifiers().testFlag(Qt::AltModifier) ||
                                   event->modifiers().testFlag(Qt::ShiftModifier) ||
                                   (modifierFreeSliceInput_ &&
                                    !event->modifiers().testFlag(Qt::ControlModifier));
        if (mode_ == Mode::Waterfall && waterfallDisplayMode_ != 0 && sliceModifier &&
            (event->button() == Qt::LeftButton || event->button() == Qt::RightButton)) {
            const QRect plot = plotRect();
            if (plot.contains(event->pos()) && !waterfallHistory_.empty()) {
                if (event->button() == Qt::LeftButton) {
                    frequencySliceActive_ = true;
                    spectrumSliceActive_ = false;
                    selectWaterfallSliceAt(event->pos(), true);
                    setCursor(Qt::CrossCursor);
                } else {
                    spectrumSliceActive_ = true;
                    frequencySliceActive_ = false;
                    selectWaterfallSliceAt(event->pos(), false);
                    if (spectrumSliceCapture_ && spectrumSliceCaptureFixed_)
                        capturedWaterfallHistory_ = waterfallHistory_;
                    else
                        capturedWaterfallHistory_.clear();
                    suppressNextContextMenu_ = true;
                    setCursor(Qt::SizeVerCursor);
                }
                update();
                event->accept();
                return;
            }
        }
        if (mode_ == Mode::Waterfall && waterfallDisplayMode_ != 0 &&
            event->modifiers().testFlag(Qt::ControlModifier) &&
            (event->button() == Qt::LeftButton || event->button() == Qt::RightButton)) {
            detachFixedPresentationForCamera();
            cameraDragActive_ = true;
            cameraPanDrag_ = event->button() == Qt::RightButton;
            cameraLast_ = event->pos();
            setCursor(cameraPanDrag_ ? Qt::SizeAllCursor : Qt::ClosedHandCursor);
            event->accept();
            return;
        }
        if (event->button() == Qt::RightButton) {
            event->accept();
            return;
        }
        if (mode_ == Mode::Ruler && event->button() == Qt::LeftButton) {
            rulerDrag_ = true;
            if (callbacks_.listeningFrequency) callbacks_.listeningFrequency(frequencyAtX(event->x()));
            event->accept();
            return;
        }
        if (mode_ == Mode::Spectrum && event->button() == Qt::LeftButton) {
            if (event->modifiers().testFlag(Qt::ShiftModifier)) {
                if (callbacks_.scienceMarker) callbacks_.scienceMarker(frequencyAtX(event->x()));
                event->accept();
                return;
            }
            measurementActive_ = true;
            measurementVisible_ = true;
            multiVfoSelection_ = event->modifiers().testFlag(Qt::ControlModifier);
            measureStart_ = measureEnd_ = event->pos();
            update();
            event->accept();
            return;
        }
        if (mode_ == Mode::Waterfall && waterfallDisplayMode_ == 0 && !secondSpectrum_ &&
            areaMeasurementEnabled_ && event->button() == Qt::LeftButton) {
            areaMeasurementActive_ = true;
            areaMeasurementVisible_ = true;
            measureStart_ = measureEnd_ = event->pos();
            update();
            event->accept();
            return;
        }
        if (event->button() == Qt::LeftButton || event->button() == Qt::MiddleButton) {
            panActive_ = true;
            panMoved_ = false;
            panButton_ = event->button();
            panLast_ = event->pos();
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override {
        hoverVisible_ = plotRect().contains(event->pos());
        hoverPosition_ = event->pos();
        if (frequencySliceActive_ && (event->buttons() & Qt::LeftButton)) {
            selectWaterfallSliceAt(event->pos(), true);
            update();
            event->accept();
            return;
        }
        if (spectrumSliceActive_ && (event->buttons() & Qt::RightButton)) {
            selectWaterfallSliceAt(event->pos(), false);
            update();
            event->accept();
            return;
        }
        if (cameraDragActive_) {
            const QPoint delta = event->pos() - cameraLast_;
            cameraLast_ = event->pos();
            if (cameraPanDrag_) {
                cameraPanX_ += double(delta.x()) / (std::max)(1, plotRect().width());
                cameraPanY_ += double(delta.y()) / (std::max)(1, plotRect().height());
            } else {
                cameraYaw_ += double(delta.x()) * 0.008;
                if (cameraYaw_ > M_PI) cameraYaw_ -= 2.0 * M_PI;
                if (cameraYaw_ < -M_PI) cameraYaw_ += 2.0 * M_PI;
                cameraTilt_ = std::clamp(cameraTilt_ - double(delta.y()) * 0.006, 0.12, 1.42);
            }
            update();
            event->accept();
            return;
        }
        if (rulerDrag_) {
            if (callbacks_.listeningFrequency) callbacks_.listeningFrequency(frequencyAtX(event->x()));
            event->accept();
            return;
        }
        if (measurementActive_ || areaMeasurementActive_) {
            measureEnd_ = event->pos();
            update();
            event->accept();
            return;
        }
        if (panActive_) {
            const int deltaPixels = event->x() - panLast_.x();
            if (deltaPixels != 0) {
                panMoved_ = true;
                panLast_ = event->pos();
                if (callbacks_.pan) callbacks_.pan(deltaPixels, plotRect().width());
            }
            event->accept();
            return;
        }
        update();
        QWidget::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton && frequencySliceActive_) {
            frequencySliceActive_ = false;
            unsetCursor();
            update();
            event->accept();
            return;
        }
        if (event->button() == Qt::RightButton && spectrumSliceActive_) {
            spectrumSliceActive_ = false;
            capturedWaterfallHistory_.clear();
            suppressNextContextMenu_ = true;
            unsetCursor();
            update();
            event->accept();
            return;
        }
        if (cameraDragActive_ && (event->button() == Qt::LeftButton || event->button() == Qt::RightButton)) {
            cameraDragActive_ = false;
            cameraPanDrag_ = false;
            unsetCursor();
            event->accept();
            return;
        }
        if (event->button() == Qt::LeftButton && rulerDrag_) {
            rulerDrag_ = false;
            event->accept();
            return;
        }
        if (event->button() == Qt::LeftButton && measurementActive_) {
            measureEnd_ = event->pos();
            measurementActive_ = false;
            if (std::abs(measureEnd_.x() - measureStart_.x()) < 4) {
                measurementVisible_ = false;
                if (multiVfoSelection_ && callbacks_.multiVfoSelection) {
                    const double frequency = signalCenterNearFrequency(frequencyAtX(event->x()));
                    callbacks_.multiVfoSelection(frequency, frequency);
                }
            } else if (multiVfoSelection_ && callbacks_.multiVfoSelection) {
                const double first = frequencyAtX(measureStart_.x());
                const double second = frequencyAtX(measureEnd_.x());
                callbacks_.multiVfoSelection((std::min)(first, second), (std::max)(first, second));
            }
            multiVfoSelection_ = false;
            update();
            event->accept();
            return;
        }
        if (event->button() == Qt::LeftButton && areaMeasurementActive_) {
            measureEnd_ = event->pos();
            areaMeasurementActive_ = false;
            if (std::abs(measureEnd_.x() - measureStart_.x()) < 3 ||
                std::abs(measureEnd_.y() - measureStart_.y()) < 3) areaMeasurementVisible_ = false;
            update();
            event->accept();
            return;
        }
        if (panActive_ && event->button() == panButton_) {
            const bool autoTune = panButton_ == Qt::MiddleButton && !panMoved_;
            panActive_ = false;
            panButton_ = Qt::NoButton;
            if (autoTune && callbacks_.autoTune)
                callbacks_.autoTune(signalCenterNearFrequency(frequencyAtX(event->x())));
            event->accept();
            return;
        }
        QWidget::mouseReleaseEvent(event);
    }
    void mouseDoubleClickEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton && callbacks_.autoTune) {
            callbacks_.autoTune(signalCenterNearFrequency(frequencyAtX(event->x())));
            event->accept();
            return;
        }
        QWidget::mouseDoubleClickEvent(event);
    }

    void contextMenuEvent(QContextMenuEvent *event) override {
        if (suppressNextContextMenu_) {
            suppressNextContextMenu_ = false;
            event->accept();
            return;
        }
        QMenu menu(this);
        const bool ukrainian = callbacks_.ukrainian && callbacks_.ukrainian();
        QAction *openSettings = menu.addAction(ukrainian ? QStringLiteral("Відкрити налаштування")
                                                          : QStringLiteral("Open settings"));
        const bool groupLocked = localSettings_.value(QStringLiteral("workspaceGroupLocked")).toBool(
            localSettings_.value(QStringLiteral("workspaceGroupLayout")).toBool(false));
        QAction *fill = menu.addAction(groupLocked
            ? (ukrainian ? QStringLiteral("Відкріпити групу") : QStringLiteral("Release group"))
            : (ukrainian ? QStringLiteral("Зафіксувати групу") : QStringLiteral("Lock group")));
        QAction *frequencyActions = menu.addAction(ukrainian ? QStringLiteral("Дії з частотою...")
                                                              : QStringLiteral("Frequency actions..."));
        QAction *meter = nullptr;
        QAction *transparentMeter = nullptr;
        QAction *retroMeter = nullptr;
        if (mode_ == Mode::Spectrum) {
            meter = menu.addAction(ukrainian ? QStringLiteral("Стрілковий індикатор піка")
                                              : QStringLiteral("Analog peak meter"));
            meter->setCheckable(true);
            meter->setChecked(analogPeakMeterEnabled_);
            QMenu *styleMenu = menu.addMenu(ukrainian ? QStringLiteral("Стиль індикатора")
                                                       : QStringLiteral("Meter style"));
            transparentMeter = styleMenu->addAction(ukrainian ? QStringLiteral("Прозорий")
                                                                : QStringLiteral("Transparent"));
            retroMeter = styleMenu->addAction(ukrainian ? QStringLiteral("Жовта ретро-підсвітка")
                                                         : QStringLiteral("Amber retro backlight"));
            transparentMeter->setCheckable(true);
            retroMeter->setCheckable(true);
            transparentMeter->setChecked(analogPeakMeterStyle_ == 0);
            retroMeter->setChecked(analogPeakMeterStyle_ == 1);
        }
        menu.addSeparator();
        QAction *pause = menu.addAction(paused_
            ? (ukrainian ? QStringLiteral("Продовжити відображення") : QStringLiteral("Resume display"))
            : (ukrainian ? QStringLiteral("Призупинити відображення") : QStringLiteral("Pause display")));
        QAction *selected = menu.exec(event->globalPos());
        if (selected == openSettings && callbacks_.openSettings) callbacks_.openSettings(blockId_);
        else if (selected == fill && callbacks_.fillGroup) callbacks_.fillGroup(blockId_);
        else if (selected == frequencyActions && callbacks_.tuneContext)
            callbacks_.tuneContext(frequencyAtX(event->pos().x()), event->globalPos());
        else if (selected == meter) {
            analogPeakMeterEnabled_ = meter->isChecked();
            if (callbacks_.analogMeterToggled) callbacks_.analogMeterToggled(analogPeakMeterEnabled_);
            update();
        } else if (selected == transparentMeter || selected == retroMeter) {
            analogPeakMeterStyle_ = selected == retroMeter ? 1 : 0;
            if (callbacks_.analogMeterStyleChanged) callbacks_.analogMeterStyleChanged(analogPeakMeterStyle_);
            update();
        } else if (selected == pause) {
            paused_ = !paused_;
            if (pauseControl_) {
                const QSignalBlocker blocker(pauseControl_);
                pauseControl_->setChecked(paused_);
                updatePauseControl();
            }
            if (callbacks_.pauseToggled) callbacks_.pauseToggled(blockId_, paused_);
            update();
        }
        event->accept();
    }

    void leaveEvent(QEvent *event) override {
        hoverVisible_ = false;
        update();
        QWidget::leaveEvent(event);
    }

private:
    void updatePauseControl() {
        if (!pauseControl_) return;
        pauseControl_->setIcon(style()->standardIcon(
            paused_ ? QStyle::SP_MediaPlay : QStyle::SP_MediaPause));
        const bool ukrainian = callbacks_.ukrainian && callbacks_.ukrainian();
        pauseControl_->setToolTip(paused_
            ? (ukrainian ? QStringLiteral("Продовжити відображення") : QStringLiteral("Resume display"))
            : (ukrainian ? QStringLiteral("Призупинити відображення") : QStringLiteral("Pause display")));
    }

    QRect plotRect() const {
        if (mode_ == Mode::Ruler)
            return rect().adjusted(kWorkspacePlotLeftMargin, 2,
                                   -kWorkspacePlotRightMargin, -2);
        return rect().adjusted(kWorkspacePlotLeftMargin, 7,
                               -kWorkspacePlotRightMargin, -23);
    }

    double frequencySpan() const {
        return frequencies_.size() >= 2 ? frequencies_.back() - frequencies_.front() : sampleRate_;
    }

    double frequencyAtX(int x) const {
        const QRect plot = plotRect();
        if (frequencies_.size() < 2 || plot.width() <= 0) return centerHz_;
        const double ratio = std::clamp(double(x - plot.left()) / double(plot.width()), 0.0, 1.0);
        return frequencies_.front() + ratio * (frequencies_.back() - frequencies_.front());
    }

    double signalCenterNearFrequency(double frequency) const {
        if (frequencies_.size() < 2 || levels_.size() != frequencies_.size()) return frequency;
        const double span = frequencies_.back() - frequencies_.front();
        if (span <= 0.0) return frequency;
        const int center = std::clamp(int(std::llround((frequency - frequencies_.front()) /
                                                       span * double(frequencies_.size() - 1))),
                                      0, int(frequencies_.size() - 1));
        const int radius = (std::max)(4, int(frequencies_.size() / 120));
        int best = center;
        for (int index = (std::max)(0, center - radius);
             index <= (std::min)(int(levels_.size() - 1), center + radius); ++index) {
            if (levels_[std::size_t(index)] > levels_[std::size_t(best)]) best = index;
        }
        return frequencies_[std::size_t(best)];
    }


    float normalizedLevel(float level) const {
        const float span = (std::max)(1.0f, maximumDbfs_ - minimumDbfs_);
        return std::clamp((level - minimumDbfs_) / span, 0.0f, 1.0f);
    }

    void drawSpectrumTrace(QPainter &painter, const QRect &plot) const {
        if (levels_.size() < 2) return;
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing, true);
        const qreal denominator = qreal((std::max<std::size_t>)(1, levels_.size() - 1));
        QPainterPath outline;
        QPointF previous(plot.left(), plot.bottom() - normalizedLevel(levels_.front()) * plot.height());
        outline.moveTo(previous);
        for (std::size_t index = 1; index < levels_.size(); ++index) {
            const QPointF current(plot.left() + plot.width() * qreal(index) / denominator,
                                  plot.bottom() - normalizedLevel(levels_[index]) * plot.height());
            outline.lineTo(current);
        }
        if (spectrumGradientFill_) {
            QPainterPath fillPath = outline;
            fillPath.lineTo(plot.right(), plot.bottom());
            fillPath.lineTo(plot.left(), plot.bottom());
            fillPath.closeSubpath();
            const QColor accent = colorSpectrum_ ? QColor(48, 178, 232) : QColor(64, 224, 139);
            QLinearGradient gradient(plot.topLeft(), plot.bottomLeft());
            QColor upper = accent;
            upper.setAlphaF(0.92 * double(spectrumGradientOpacity_) / 100.0);
            QColor lower = accent.darker(185);
            lower.setAlpha(0);
            gradient.setColorAt(0.0, upper);
            gradient.setColorAt(1.0, lower);
            painter.fillPath(fillPath, gradient);
        }
        painter.setPen(QPen(colorSpectrum_ ? QColor(52, 126, 148) : QColor(64, 224, 139), 1.15));
        painter.drawPath(outline);
        previous = QPointF(plot.left(), plot.bottom() - normalizedLevel(levels_.front()) * plot.height());
        float previousNormalized = normalizedLevel(levels_.front());
        for (std::size_t index = 1; index < levels_.size(); ++index) {
            const float normalized = normalizedLevel(levels_[index]);
            const QPointF current(plot.left() + plot.width() * qreal(index) / denominator,
                                  plot.bottom() - normalized * plot.height());
            const float segmentLevel = 0.5f * (previousNormalized + normalized);
            painter.setPen(QPen(colorSpectrum_ ? workspaceSpectrumColor(segmentLevel)
                                               : QColor(64, 224, 139),
                                1.25));
            painter.drawLine(previous, current);
            previous = current;
            previousNormalized = normalized;
        }
        painter.restore();
    }

    QString formatFrequencySpan(double valueHz) const {
        const double magnitude = std::abs(valueHz);
        if (magnitude >= 1.0e6) return QStringLiteral("%1 MHz").arg(valueHz / 1.0e6, 0, 'f', 3);
        if (magnitude >= 1.0e3) return QStringLiteral("%1 kHz").arg(valueHz / 1.0e3, 0, 'f', 3);
        return QStringLiteral("%1 Hz").arg(valueHz, 0, 'f', 2);
    }

    void drawInformationOverlays(QPainter &painter, const QRect &plot) const {
        const bool showFps = mode_ == Mode::Spectrum ? showSpectrumFps_ :
                             (mode_ == Mode::Waterfall && showWaterfallFps_);
        const int overlayRight = pauseControl_ && pauseControl_->isVisible()
            ? (std::min)(plot.right() - 5, pauseControl_->geometry().left() - 6)
            : plot.right() - 5;
        int top = plot.top() + 7;
        if (showFps) {
            const QString text = displayedFps_ > 0.0
                ? QStringLiteral("FPS %1").arg(displayedFps_, 0, 'f', 1)
                : QStringLiteral("FPS --");
            QRect box = painter.fontMetrics().boundingRect(text).adjusted(-6, -3, 6, 3);
            box.moveTopRight(QPoint(overlayRight, top));
            painter.fillRect(box, QColor(0, 0, 0, 190));
            painter.setPen(QColor(235, 245, 235));
            painter.drawText(box, Qt::AlignCenter, text);
            top = box.bottom() + 5;
        }
        if (!showExtendedInfo_ || mode_ == Mode::Ruler) return;

        QStringList lines;
        lines << QStringLiteral("center  %1 MHz").arg(centerHz_ / 1.0e6, 0, 'f', 6)
              << QStringLiteral("listen  %1 MHz").arg(listeningHz_ / 1.0e6, 0, 'f', 6)
              << QStringLiteral("SR      %1").arg(formatFrequencySpan(sampleRate_));
        const int fft = sourceFftLength_ > 0 ? sourceFftLength_ : int(levels_.size());
        if (fft > 0) {
            const double binWidth = sampleRate_ > 0.0 ? sampleRate_ / double(fft) : 0.0;
            lines << QStringLiteral("FFT     %1").arg(fft)
                  << QStringLiteral("window  %1").arg(QString::fromLatin1(fftWindowTypeName(fftWindowType_)));
            if (binWidth > 0.0) {
                lines << QStringLiteral("RBW     %1").arg(formatFrequencySpan(binWidth * fftWindowEnbwBins(fftWindowType_)))
                      << QStringLiteral("bin     %1").arg(formatFrequencySpan(binWidth));
            }
        }
        const QString text = lines.join(QLatin1Char('\n'));
        QRect box = painter.fontMetrics().boundingRect(QRect(0, 0, plot.width(), plot.height()),
                                                        Qt::AlignLeft | Qt::AlignTop, text)
                        .adjusted(-7, -5, 7, 5);
        box.moveTopRight(QPoint(overlayRight, top));
        painter.fillRect(box, QColor(0, 0, 0, 190));
        painter.setPen(QColor(255, 224, 140, 180));
        painter.drawRect(box.adjusted(0, 0, -1, -1));
        painter.setPen(QColor(235, 245, 235));
        painter.drawText(box.adjusted(7, 5, -7, -5), Qt::AlignLeft | Qt::AlignTop, text);
    }
    QRgb colorForLevel(float level) const {
        return workspaceWaterfallColor(normalizedLevel(level), contrast_, sensitivity_).rgb();
    }

    void rebuildWaterfall() {
        const int imageWidth = (std::clamp)(width() - 60, 128, 2048);
        const int imageHeight = (std::clamp)(height() - 30, 48, 1024);
        if (waterfallImage_.size() == QSize(imageWidth, imageHeight)) return;
        QImage replacement(imageWidth, imageHeight, QImage::Format_RGB32);
        replacement.fill(QColor(4, 7, 12));
        if (!waterfallImage_.isNull()) {
            QImage ordered(waterfallImage_.size(), QImage::Format_RGB32);
            ordered.fill(QColor(4, 7, 12));
            QPainter historyPainter(&ordered);
            const int firstRows = waterfallImage_.height() - waterfallHead_;
            historyPainter.drawImage(QRect(0, 0, ordered.width(), firstRows), waterfallImage_,
                                     QRect(0, waterfallHead_, waterfallImage_.width(), firstRows));
            if (waterfallHead_ > 0) {
                historyPainter.drawImage(QRect(0, firstRows, ordered.width(), waterfallHead_),
                                         waterfallImage_,
                                         QRect(0, 0, waterfallImage_.width(), waterfallHead_));
            }
            historyPainter.end();
            replacement = ordered.scaled(replacement.size(), Qt::IgnoreAspectRatio,
                                         Qt::SmoothTransformation);
        }
        waterfallImage_ = std::move(replacement);
        waterfallHead_ = 0;
    }

    void appendWaterfallRow() {
        if (waterfallImage_.isNull()) rebuildWaterfall();
        if (waterfallImage_.isNull()) return;
        waterfallHead_ = (waterfallHead_ - 1 + waterfallImage_.height()) % waterfallImage_.height();
        QRgb *line = reinterpret_cast<QRgb *>(waterfallImage_.scanLine(waterfallHead_));
        const std::size_t sourceSize = levels_.size();
        for (int x = 0; x < waterfallImage_.width(); ++x) {
            const std::size_t first = sourceSize * std::size_t(x) / std::size_t(waterfallImage_.width());
            const std::size_t last = sourceSize * std::size_t(x + 1) / std::size_t(waterfallImage_.width());
            float peak = -240.0f;
            for (std::size_t index = first;
                 index < (std::max)(first + 1U, last) && index < sourceSize; ++index) {
                if (std::isfinite(levels_[index])) peak = (std::max)(peak, levels_[index]);
            }
            line[x] = colorForLevel(peak);
        }
    }

    void resampleWaterfallHistory(int targetWidth) {
        targetWidth = (std::max)(2, targetWidth);
        const auto resampleRow = [targetWidth, this](const std::vector<float> &source) {
            std::vector<float> destination(std::size_t(targetWidth), minimumDbfs_);
            if (source.empty()) return destination;
            if (source.size() == 1U) {
                std::fill(destination.begin(), destination.end(), source.front());
                return destination;
            }
            if (targetWidth < int(source.size())) {
                for (int output = 0; output < targetWidth; ++output) {
                    const std::size_t first = source.size() * std::size_t(output) /
                                              std::size_t(targetWidth);
                    const std::size_t last = source.size() * std::size_t(output + 1) /
                                             std::size_t(targetWidth);
                    double sum = 0.0;
                    int count = 0;
                    for (std::size_t input = first;
                         input < (std::max)(first + 1U, last) && input < source.size(); ++input) {
                        if (!std::isfinite(source[input])) continue;
                        sum += source[input];
                        ++count;
                    }
                    if (count > 0) destination[std::size_t(output)] = float(sum / count);
                }
                return destination;
            }
            const double sourceLast = double(source.size() - 1U);
            const double destinationLast = double((std::max)(1, targetWidth - 1));
            for (int output = 0; output < targetWidth; ++output) {
                const double sourcePosition = double(output) * sourceLast / destinationLast;
                const std::size_t first = std::size_t(std::floor(sourcePosition));
                const std::size_t second = (std::min)(first + 1U, source.size() - 1U);
                const float ratio = float(sourcePosition - double(first));
                const float a = std::isfinite(source[first]) ? source[first] : minimumDbfs_;
                const float b = std::isfinite(source[second]) ? source[second] : a;
                destination[std::size_t(output)] = a + (b - a) * ratio;
            }
            return destination;
        };
        for (auto &row : waterfallHistory_) {
            if (int(row.size()) != targetWidth) row = resampleRow(row);
        }
        for (auto &row : capturedWaterfallHistory_) {
            if (int(row.size()) != targetWidth) row = resampleRow(row);
        }
    }
    void appendWaterfallHistory() {
        if (levels_.empty()) return;
        const int availableWidth = (std::max)(64, width() - 60);
        const int targetWidth = std::clamp(availableWidth / waterfallResolutionDivisor_, 64, 1024);
        const bool historyWidthChanged = std::any_of(
            waterfallHistory_.cbegin(), waterfallHistory_.cend(),
            [targetWidth](const std::vector<float> &row) {
                return int(row.size()) != targetWidth;
            });
        const bool capturedWidthChanged = std::any_of(
            capturedWaterfallHistory_.cbegin(), capturedWaterfallHistory_.cend(),
            [targetWidth](const std::vector<float> &row) {
                return int(row.size()) != targetWidth;
            });
        if (historyWidthChanged || capturedWidthChanged)
            resampleWaterfallHistory(targetWidth);
        std::vector<float> row(std::size_t(targetWidth), minimumDbfs_);
        for (int x = 0; x < targetWidth; ++x) {
            const std::size_t first = levels_.size() * std::size_t(x) / std::size_t(targetWidth);
            const std::size_t last = levels_.size() * std::size_t(x + 1) / std::size_t(targetWidth);
            double sum = 0.0;
            int count = 0;
            for (std::size_t index = first;
                 index < (std::max)(first + 1U, last) && index < levels_.size(); ++index) {
                if (std::isfinite(levels_[index])) { sum += levels_[index]; ++count; }
            }
            if (count > 0) row[std::size_t(x)] = float(sum / count);
        }
        waterfallHistory_.push_front(std::move(row));
        if (spectrumSliceActive_ && spectrumSliceCapture_ && !spectrumSliceCaptureFixed_)
            selectedSpectrumRow_ = (std::min)(selectedSpectrumRow_ + 1,
                                              int(waterfallHistory_.size()) - 1);
        while (int(waterfallHistory_.size()) > waterfallHistoryLimit_) waterfallHistory_.pop_back();
    }

    void detachFixedPresentationForCamera() {
        if (!waterfall3DFixedPlane_ || cameraOverrideActive_) return;
        cameraOverrideActive_ = true;
        cameraYaw_ = 0.0;
        cameraTilt_ = 0.68;
        cameraZoom_ = 1.0;
        cameraPanX_ = 0.0;
        cameraPanY_ = 0.0;
    }

    struct WaterfallProjection {
        qreal yawCos = 1.0;
        qreal yawSin = 0.0;
        qreal pitchCos = 1.0;
        qreal pitchSin = 0.0;
        qreal zoom = 1.0;
        qreal rawCenterX = 0.0;
        qreal rawCenterY = 0.0;
        qreal scaleX = 1.0;
        qreal scaleY = 1.0;
        QPointF screenCenter;
    };

    struct WaterfallRawPoint {
        qreal x = 0.0;
        qreal y = 0.0;
        qreal depth = 0.0;
    };

    WaterfallRawPoint waterfallRawPoint(const WaterfallProjection &projection,
                                        qreal frequencyPosition,
                                        qreal historyPosition,
                                        qreal normalizedAmplitude) const {
        const qreal u = frequencyPosition * 2.0 - 1.0;
        const qreal v = historyPosition - 0.5;
        const qreal z = normalizedAmplitude * 0.48;
        const qreal rotatedX = u * projection.yawCos - v * projection.yawSin;
        const qreal rotatedDepth = u * projection.yawSin + v * projection.yawCos;
        const qreal screenVertical = -rotatedDepth * projection.pitchSin - z * projection.pitchCos;
        const qreal cameraDepth = rotatedDepth * projection.pitchCos - z * projection.pitchSin;
        const qreal perspective = 1.0 / (std::max)(0.55, 1.0 + cameraDepth * 0.34);
        return {rotatedX * perspective, screenVertical * perspective, cameraDepth};
    }

    WaterfallProjection waterfallProjection(const QRect &plot) const {
        const bool fixedPresentation = waterfall3DFixedPlane_ && !cameraOverrideActive_;
        const qreal yaw = fixedPresentation ? 0.0 : cameraYaw_;
        const qreal tilt = fixedPresentation ? 0.68 : cameraTilt_;
        WaterfallProjection projection;
        projection.yawCos = qCos(yaw);
        projection.yawSin = qSin(yaw);
        projection.pitchSin = qSin(tilt);
        projection.pitchCos = qCos(tilt);
        projection.zoom = fixedPresentation ? 0.92 : cameraZoom_;
        const qreal panX = fixedPresentation ? 0.0 : cameraPanX_;
        const qreal panY = fixedPresentation ? 0.10 : cameraPanY_;
        qreal minimumX = std::numeric_limits<qreal>::max();
        qreal maximumX = std::numeric_limits<qreal>::lowest();
        qreal minimumY = std::numeric_limits<qreal>::max();
        qreal maximumY = std::numeric_limits<qreal>::lowest();
        for (const qreal frequency : {qreal(0.0), qreal(1.0)}) {
            for (const qreal history : {qreal(0.0), qreal(1.0)}) {
                for (const qreal amplitude : {qreal(0.0), qreal(1.0)}) {
                    const WaterfallRawPoint point = waterfallRawPoint(
                        projection, frequency, history, amplitude);
                    minimumX = (std::min)(minimumX, point.x);
                    maximumX = (std::max)(maximumX, point.x);
                    minimumY = (std::min)(minimumY, point.y);
                    maximumY = (std::max)(maximumY, point.y);
                }
            }
        }
        projection.rawCenterX = 0.5 * (minimumX + maximumX);
        projection.rawCenterY = 0.5 * (minimumY + maximumY);
        projection.scaleX = plot.width() * 0.92 /
                            (std::max)(qreal(0.001), maximumX - minimumX);
        projection.scaleY = plot.height() * 0.88 /
                            (std::max)(qreal(0.001), maximumY - minimumY);
        projection.screenCenter = plot.center() +
                                  QPointF(panX * plot.width(), panY * plot.height());
        return projection;
    }

    qreal waterfallCameraDepth(const WaterfallProjection &projection,
                               qreal frequencyPosition,
                               qreal historyPosition,
                               qreal normalizedAmplitude) const {
        return waterfallRawPoint(projection, frequencyPosition,
                                 historyPosition, normalizedAmplitude).depth;
    }

    QPointF projectWaterfallPoint(const WaterfallProjection &projection,
                                  qreal frequencyPosition,
                                  qreal historyPosition,
                                  qreal normalizedAmplitude) const {
        const WaterfallRawPoint raw = waterfallRawPoint(
            projection, frequencyPosition, historyPosition, normalizedAmplitude);
        return QPointF(
            projection.screenCenter.x() +
                (raw.x - projection.rawCenterX) * projection.scaleX * projection.zoom,
            projection.screenCenter.y() +
                (raw.y - projection.rawCenterY) * projection.scaleY * projection.zoom);
    }
    void selectWaterfallSliceAt(const QPoint &position, bool frequencySlice) {
        if (waterfallHistory_.empty()) return;
        const QRect plot = plotRect();
        const WaterfallProjection projection = waterfallProjection(plot);
        const int rowCount = (std::min)(int(waterfallHistory_.size()), waterfallHistoryLimit_);
        qreal bestDistance = std::numeric_limits<qreal>::max();
        double bestFrequency = selectedFrequencySlice_;
        int bestRow = selectedSpectrumRow_;
        const int rowStep = (std::max)(1, rowCount / 96);
        for (int rowIndex = 0; rowIndex < rowCount; rowIndex += rowStep) {
            const auto &row = waterfallHistory_[std::size_t(rowIndex)];
            if (row.empty()) continue;
            const int columnStep = (std::max)(1, int(row.size()) / 256);
            const qreal depth = rowCount > 1 ? qreal(rowIndex) / qreal(rowCount - 1) : 0.0;
            for (int column = 0; column < int(row.size()); column += columnStep) {
                const qreal ratio = row.size() > 1 ? qreal(column) / qreal(row.size() - 1) : 0.0;
                const QPointF projected = projectWaterfallPoint(
                    projection, ratio, depth, normalizedLevel(row[std::size_t(column)]));
                const qreal dx = projected.x() - position.x();
                const qreal dy = projected.y() - position.y();
                const qreal distance = dx * dx + dy * dy;
                if (distance < bestDistance) {
                    bestDistance = distance;
                    bestFrequency = ratio;
                    bestRow = rowIndex;
                }
            }
        }
        if (frequencySlice) selectedFrequencySlice_ = std::clamp(bestFrequency, 0.0, 1.0);
        else selectedSpectrumRow_ = std::clamp(bestRow, 0, rowCount - 1);
    }

    void drawWaterfall3D(QPainter &painter, const QRect &plot) const {
        if (waterfallHistory_.empty()) return;
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing, waterfall3DSurfaceStyle_ == 0);
        painter.setClipRect(plot);
        const WaterfallProjection projection = waterfallProjection(plot);
        const auto &history = spectrumSliceActive_ && spectrumSliceCapture_ &&
                              spectrumSliceCaptureFixed_ && !capturedWaterfallHistory_.empty()
                                  ? capturedWaterfallHistory_
                                  : waterfallHistory_;
        const int rowCount = (std::min)(int(history.size()), waterfallHistoryLimit_);
        const int targetRows = std::clamp(plot.height() / 5, 28, 80);
        const int normalRowStep = (std::max)(1, (rowCount + targetRows - 1) / targetRows);
        const int firstSpectrumRow = spectrumSliceActive_
            ? (std::max)(0, selectedSpectrumRow_ - spectrumSliceRows_ / 2)
            : 0;
        const int lastSpectrumRow = spectrumSliceActive_
            ? (std::min)(rowCount - 1, firstSpectrumRow + spectrumSliceRows_ - 1)
            : rowCount - 1;
        const int selectedRowCount = lastSpectrumRow - firstSpectrumRow + 1;
        const int rowStep = spectrumSliceActive_
            ? (std::max)(1, (selectedRowCount + targetRows - 1) / targetRows)
            : normalRowStep;
        const int nearestDrawnIndex = spectrumSliceActive_
            ? firstSpectrumRow + (lastSpectrumRow - firstSpectrumRow) % rowStep
            : (rowCount - 1) % normalRowStep;

        struct RenderedRow {
            int rowIndex = 0;
            qreal historyPosition = 0.0;
            QVector<QPointF> tops;
            QVector<QPointF> bases;
            QVector<float> normalized;
            QVector<qreal> depths;
        };
        QVector<RenderedRow> renderedRows;
        renderedRows.reserve((lastSpectrumRow - firstSpectrumRow) / rowStep + 1);

        for (int rowIndex = lastSpectrumRow; rowIndex >= firstSpectrumRow; rowIndex -= rowStep) {
            const auto &row = history[std::size_t(rowIndex)];
            if (row.empty()) continue;
            RenderedRow rendered;
            rendered.rowIndex = rowIndex;
            rendered.historyPosition = rowCount > 1
                ? qreal(rowIndex) / qreal(rowCount - 1) : 0.0;
            int firstColumn = 0;
            int lastColumn = int(row.size()) - 1;
            if (frequencySliceActive_) {
                const int centerColumn = std::clamp(
                    int(std::llround(selectedFrequencySlice_ * double((std::max)(1, int(row.size()) - 1)))),
                    0, int(row.size()) - 1);
                firstColumn = (std::max)(0, centerColumn - frequencySliceWidth_ / 2);
                lastColumn = (std::min)(int(row.size()) - 1,
                                        firstColumn + frequencySliceWidth_ - 1);
                firstColumn = (std::max)(0, lastColumn - frequencySliceWidth_ + 1);
            }
            const int columnSpan = lastColumn - firstColumn + 1;
            const int targetColumns = std::clamp(plot.width() / 3, 96, 320);
            const int columnStep = (std::max)(
                1, (columnSpan + targetColumns - 1) / targetColumns);
            rendered.tops.reserve(columnSpan / columnStep + 2);
            rendered.bases.reserve(columnSpan / columnStep + 2);
            rendered.normalized.reserve(columnSpan / columnStep + 2);
            rendered.depths.reserve(columnSpan / columnStep + 2);

            const auto appendColumn = [&](int column, int last) {
                double sum = 0.0;
                int samples = 0;
                const int radius = waterfall3DSmoothing_ == 0 ? 0
                                   : (waterfall3DSmoothing_ == 1 ? 1 : 3);
                for (int source = (std::max)(firstColumn, column - radius);
                     source <= (std::min)(lastColumn, last + radius); ++source) {
                    sum += normalizedLevel(row[std::size_t(source)]);
                    ++samples;
                }
                const float level = samples > 0 ? float(sum / samples) : 0.0f;
                const qreal ratio = row.size() > 1
                    ? qreal(column + last) * 0.5 / qreal(row.size() - 1) : 0.0;
                rendered.tops.append(projectWaterfallPoint(
                    projection, ratio, rendered.historyPosition, level));
                rendered.bases.append(projectWaterfallPoint(
                    projection, ratio, rendered.historyPosition, 0.0));
                rendered.normalized.append(level);
                rendered.depths.append(waterfallCameraDepth(
                    projection, ratio, rendered.historyPosition, level));
            };
            for (int column = firstColumn; column <= lastColumn; column += columnStep)
                appendColumn(column, (std::min)(lastColumn, column + columnStep - 1));
            if (lastColumn > firstColumn && columnStep > 1 &&
                (lastColumn - firstColumn) % columnStep != 0)
                appendColumn(lastColumn, lastColumn);
            if (!rendered.tops.isEmpty()) renderedRows.append(std::move(rendered));
        }

        constexpr int kSurfaceDepthBuckets = 20;
        constexpr int kSurfaceColorBuckets = 32;
        constexpr int kSurfaceShadeBuckets = 4;
        QVector<QPainterPath> surfacePaths(
            kSurfaceDepthBuckets * kSurfaceColorBuckets * kSurfaceShadeBuckets);
        if (waterfall3DSurfaceStyle_ == 1 && renderedRows.size() > 1) {
            qreal minimumDepth = std::numeric_limits<qreal>::max();
            qreal maximumDepth = std::numeric_limits<qreal>::lowest();
            for (const RenderedRow &row : renderedRows) {
                for (const qreal depth : row.depths) {
                    minimumDepth = (std::min)(minimumDepth, depth);
                    maximumDepth = (std::max)(maximumDepth, depth);
                }
            }
            const qreal depthRange = (std::max)(qreal(0.0001), maximumDepth - minimumDepth);
            const int maximumDarkening = waterfall3DLighting_ == 1 ? 85 : 150;
            const auto appendSurfaceTriangle = [&](const QPolygonF &polygon,
                                                   float level,
                                                   float slope,
                                                   qreal depth) {
                const int depthBucket = std::clamp(
                    int((depth - minimumDepth) / depthRange * kSurfaceDepthBuckets),
                    0, kSurfaceDepthBuckets - 1);
                const int colorBucket = std::clamp(
                    int(level * kSurfaceColorBuckets), 0, kSurfaceColorBuckets - 1);
                const float limitedSlope = std::clamp(slope, 0.0f, 1.0f);
                const int shadeBucket = waterfall3DLighting_ == 0
                    ? 0
                    : std::clamp(int(limitedSlope * kSurfaceShadeBuckets),
                                 0, kSurfaceShadeBuckets - 1);
                const int pathIndex =
                    (depthBucket * kSurfaceColorBuckets + colorBucket) *
                        kSurfaceShadeBuckets + shadeBucket;
                QPainterPath &path = surfacePaths[pathIndex];
                if (path.isEmpty()) path.setFillRule(Qt::WindingFill);
                path.addPolygon(polygon);
            };

            for (int row = 1; row < renderedRows.size(); ++row) {
                const RenderedRow &farRow = renderedRows[row - 1];
                const RenderedRow &nearRow = renderedRows[row];
                const int columns = (std::min)(farRow.tops.size(), nearRow.tops.size());
                for (int column = 1; column < columns; ++column) {
                    const float level = 0.25f * (
                        farRow.normalized[column - 1] + farRow.normalized[column] +
                        nearRow.normalized[column - 1] + nearRow.normalized[column]);
                    const float slope = std::abs(nearRow.normalized[column] -
                                                 nearRow.normalized[column - 1]) +
                                        std::abs(nearRow.normalized[column] -
                                                 farRow.normalized[column]);

                    QPolygonF firstTriangle;
                    firstTriangle << farRow.tops[column - 1]
                                  << farRow.tops[column]
                                  << nearRow.tops[column];
                    appendSurfaceTriangle(
                        firstTriangle, level, slope,
                        (farRow.depths[column - 1] + farRow.depths[column] +
                         nearRow.depths[column]) / 3.0);

                    QPolygonF secondTriangle;
                    secondTriangle << farRow.tops[column - 1]
                                   << nearRow.tops[column]
                                   << nearRow.tops[column - 1];
                    appendSurfaceTriangle(
                        secondTriangle, level, slope,
                        (farRow.depths[column - 1] + nearRow.depths[column] +
                         nearRow.depths[column - 1]) / 3.0);
                }
            }

            painter.setPen(Qt::NoPen);
            for (int depthBucket = kSurfaceDepthBuckets - 1;
                 depthBucket >= 0; --depthBucket) {
                for (int colorBucket = 0; colorBucket < kSurfaceColorBuckets; ++colorBucket) {
                    const float colorLevel =
                        (float(colorBucket) + 0.5f) / float(kSurfaceColorBuckets);
                    for (int shadeBucket = 0;
                         shadeBucket < kSurfaceShadeBuckets; ++shadeBucket) {
                        const int pathIndex =
                            (depthBucket * kSurfaceColorBuckets + colorBucket) *
                                kSurfaceShadeBuckets + shadeBucket;
                        const QPainterPath &path = surfacePaths[pathIndex];
                        if (path.isEmpty()) continue;
                        QColor surface = waterfall3DMonochrome_
                            ? QColor(45, 104, 196)
                            : workspaceWaterfallColor(colorLevel, contrast_, sensitivity_);
                        if (waterfall3DLighting_ > 0) {
                            const qreal shadePosition =
                                (qreal(shadeBucket) + 0.5) / qreal(kSurfaceShadeBuckets);
                            surface = surface.darker(
                                100 + qRound(shadePosition * maximumDarkening));
                        }
                        surface.setAlpha(255);
                        painter.fillPath(path, surface);
                    }
                }
            }
        }
        const RenderedRow *frontRow = nullptr;
        for (const RenderedRow &row : renderedRows) {
            if (row.rowIndex == nearestDrawnIndex) { frontRow = &row; break; }
        }
        if (spectrumGradientFill_ && frontRow && frontRow->tops.size() > 1) {
            for (int index = 1; index < frontRow->tops.size(); ++index) {
                const float level = 0.5f * (frontRow->normalized[index - 1] +
                                            frontRow->normalized[index]);
                QColor accent = waterfall3DMonochrome_
                    ? QColor(72, 138, 232)
                    : (colorSpectrum_
                           ? workspaceWaterfallColor(level, contrast_, sensitivity_)
                           : QColor(64, 224, 139));
                QColor upper = accent;
                upper.setAlphaF(0.98 * double(spectrumGradientOpacity_) / 100.0);
                QColor lower = accent.darker(220);
                lower.setAlphaF(0.22 * double(spectrumGradientOpacity_) / 100.0);
                QLinearGradient gradient((frontRow->tops[index - 1] + frontRow->tops[index]) * 0.5,
                                         (frontRow->bases[index - 1] + frontRow->bases[index]) * 0.5);
                gradient.setColorAt(0.0, upper);
                gradient.setColorAt(1.0, lower);
                QPolygonF strip;
                strip << frontRow->bases[index - 1] << frontRow->tops[index - 1]
                      << frontRow->tops[index] << frontRow->bases[index];
                painter.setPen(Qt::NoPen);
                painter.setBrush(gradient);
                painter.drawPolygon(strip);
            }
        }

        constexpr int kContourBuckets = 48;
        QVector<QPainterPath> contourPaths(kContourBuckets * 2);
        for (int renderedRowIndex = 0;
             renderedRowIndex < renderedRows.size(); ++renderedRowIndex) {
            const RenderedRow &row = renderedRows[renderedRowIndex];
            if (row.tops.size() < 2) continue;
            const bool front = row.rowIndex == nearestDrawnIndex;
            if (waterfall3DSurfaceStyle_ == 1 && !front &&
                renderedRowIndex % 2 != 0) continue;
            for (int index = 1; index < row.tops.size(); ++index) {
                const float level = 0.5f *
                    (row.normalized[index - 1] + row.normalized[index]);
                const int bucket = std::clamp(
                    int(level * kContourBuckets), 0, kContourBuckets - 1);
                QPainterPath &path = contourPaths[bucket * 2 + (front ? 1 : 0)];
                path.moveTo(row.tops[index - 1]);
                path.lineTo(row.tops[index]);
            }
        }
        painter.setBrush(Qt::NoBrush);
        for (int bucket = 0; bucket < kContourBuckets; ++bucket) {
            const float level = (float(bucket) + 0.5f) / float(kContourBuckets);
            QColor color = waterfall3DMonochrome_
                ? QColor(72, 138, 232)
                : (colorSpectrum_
                       ? workspaceWaterfallColor(level, contrast_, sensitivity_)
                       : QColor(64, 224, 139));
            const int amplitudeAlpha = qRound(72.0 + 183.0 * level);
            for (int front = 0; front < 2; ++front) {
                const QPainterPath &path = contourPaths[bucket * 2 + front];
                if (path.isEmpty()) continue;
                QColor lineColor = color;
                lineColor.setAlpha(waterfall3DSurfaceStyle_ == 1
                                       ? (front ? 220 : qRound(34.0 + 90.0 * level))
                                       : (std::max)(amplitudeAlpha, 125));
                const qreal width = front
                    ? 1.8
                    : (waterfall3DSurfaceStyle_ == 1
                           ? 0.55 + 0.55 * level
                           : 0.8 + 1.25 * level);
                painter.setPen(QPen(lineColor, width, Qt::SolidLine, Qt::RoundCap));
                painter.drawPath(path);
            }
        }
        if (frequencySliceActive_ && !renderedRows.isEmpty()) {
            bool havePrevious = false;
            QPointF previousPoint;
            float previousLevel = 0.0f;
            for (const RenderedRow &row : renderedRows) {
                if (row.tops.isEmpty()) continue;
                const int middle = row.tops.size() / 2;
                const QPointF point = row.tops[middle];
                const float level = row.normalized[middle];
                if (havePrevious) {
                    const QColor color = waterfall3DMonochrome_
                        ? QColor(72, 138, 232)
                        : workspaceWaterfallColor(
                              0.5f * (previousLevel + level), contrast_, sensitivity_);
                    painter.setPen(QPen(color, 2.4, Qt::SolidLine, Qt::RoundCap));
                    painter.drawLine(previousPoint, point);
                }
                previousPoint = point;
                previousLevel = level;
                havePrevious = true;
            }
            const QPointF labelPoint = renderedRows.back().tops.isEmpty()
                ? QPointF(plot.left(), plot.top())
                : renderedRows.back().tops[renderedRows.back().tops.size() / 2];
            const double frequency = frequencies_.empty()
                ? 0.0 : frequencies_.front() + selectedFrequencySlice_ * frequencySpan();
            painter.drawText(QRectF(labelPoint + QPointF(6.0, -24.0), QSizeF(180.0, 20.0)),
                             QStringLiteral("%1 MHz").arg(frequency / 1.0e6, 0, 'f', 6));
        } else if (spectrumSliceActive_) {
            painter.setPen(QPen(QColor(255, 221, 92), 2.0));
            const qreal depth = rowCount > 1
                ? qreal(selectedSpectrumRow_) / qreal(rowCount - 1) : 0.0;
            const QPointF left = projectWaterfallPoint(projection, 0.0, depth, 0.0);
            const QPointF right = projectWaterfallPoint(projection, 1.0, depth, 0.0);
            painter.drawLine(left, right);
            painter.drawText(QRectF(left + QPointF(6.0, -24.0), QSizeF(180.0, 20.0)),
                             QStringLiteral("row %1").arg(selectedSpectrumRow_));
        }
        painter.restore();
    }    void drawMiniWaterfall(QPainter &painter, const QRect &plot) const {
        if (waterfallImage_.isNull()) return;
        const QSize miniSize((std::max)(120, plot.width() / 3), (std::max)(70, plot.height() / 3));
        const QRect mini(plot.left() + 8, plot.top() + 8, miniSize.width(), miniSize.height());
        painter.fillRect(mini.adjusted(-2, -2, 2, 2), QColor(0, 0, 0, 185));
        const int h = waterfallImage_.height();
        const int firstRows = h - waterfallHead_;
        const int firstTarget = qRound(mini.height() * (double(firstRows) / h));
        painter.drawImage(QRect(mini.left(), mini.top(), mini.width(), firstTarget), waterfallImage_,
                          QRect(0, waterfallHead_, waterfallImage_.width(), firstRows));
        if (waterfallHead_ > 0) {
            painter.drawImage(QRect(mini.left(), mini.top() + firstTarget, mini.width(), mini.height() - firstTarget),
                              waterfallImage_, QRect(0, 0, waterfallImage_.width(), waterfallHead_));
        }
        painter.setPen(QColor(166, 179, 194));
        painter.drawRect(mini.adjusted(0, 0, -1, -1));
    }

    void drawInactiveLabel(QPainter &painter, const QRect &plot) const {
        const bool ukrainian = callbacks_.ukrainian && callbacks_.ukrainian();
        const QString text = mode_ == Mode::Spectrum
            ? (ukrainian ? QStringLiteral("СПЕКТР") : QStringLiteral("SPECTRUM"))
            : (mode_ == Mode::Ruler
                   ? (ukrainian ? QStringLiteral("ШКАЛА ЧАСТОТ") : QStringLiteral("FREQUENCY RULER"))
                   : (ukrainian ? QStringLiteral("ВОДОСПАД") : QStringLiteral("WATERFALL")));
        QFont font = painter.font();
        font.setBold(true);
        font.setPointSizeF((std::max)(8.0, font.pointSizeF()));
        painter.setFont(font);
        QRect label = painter.fontMetrics().boundingRect(text).adjusted(-8, -4, 8, 4);
        label.moveTopLeft(plot.topLeft() + QPoint(8, 8));
        painter.setPen(QPen(QColor(112, 132, 154), 1.0));
        painter.setBrush(QColor(12, 18, 27, 220));
        painter.drawRoundedRect(label, 3.0, 3.0);
        painter.setPen(QColor(199, 214, 229));
        painter.drawText(label, Qt::AlignCenter, text);
    }
    void drawListeningMarker(QPainter &painter, const QRect &plot) const {
        if (frequencies_.size() < 2 || listeningHz_ < frequencies_.front() ||
            listeningHz_ > frequencies_.back()) return;
        const double span = frequencies_.back() - frequencies_.front();
        if (span <= 0.0) return;
        const int x = plot.left() + qRound(plot.width() * ((listeningHz_ - frequencies_.front()) / span));
        painter.setPen(QPen(QColor(255, 193, 72), 1.0, Qt::DashLine));
        painter.drawLine(x, plot.top(), x, plot.bottom());
    }

    void drawFrequencyEnds(QPainter &painter, const QRect &plot) const {
        if (frequencies_.empty()) return;
        painter.setPen(QColor(154, 166, 181));
        const QString left = QStringLiteral("%1 MHz").arg(frequencies_.front() / 1.0e6, 0, 'f', 6);
        const QString right = QStringLiteral("%1 MHz").arg(frequencies_.back() / 1.0e6, 0, 'f', 6);
        painter.drawText(QRect(plot.left(), plot.bottom() + 3, plot.width() / 2, 18),
                         Qt::AlignLeft | Qt::AlignVCenter, left);
        painter.drawText(QRect(plot.center().x(), plot.bottom() + 3, plot.width() / 2, 18),
                         Qt::AlignRight | Qt::AlignVCenter, right);
    }

    void drawRuler(QPainter &painter, const QRect &plot) const {
        painter.setPen(QPen(QColor(87, 101, 119), 1));
        const int axisY = plot.center().y();
        painter.drawLine(plot.left(), axisY, plot.right(), axisY);
        if (frequencies_.size() < 2) return;
        const double span = frequencies_.back() - frequencies_.front();
        if (span <= 0.0) return;
        const int markerX = plot.left() + qRound(plot.width() *
                            ((listeningHz_ - frequencies_.front()) / span));
        const int bandwidthPixels = (std::max)(1, qRound(plot.width() * bandwidthHz_ / span));
        int start = markerX - bandwidthPixels / 2;
        int end = markerX + bandwidthPixels / 2;
        if (isUpperSidebandMode(modulationType_)) {
            start = markerX;
            end = markerX + bandwidthPixels;
        } else if (isLowerSidebandMode(modulationType_)) {
            start = markerX - bandwidthPixels;
            end = markerX;
        }
        start = std::clamp(start, plot.left(), plot.right());
        end = std::clamp(end, plot.left(), plot.right());
        if (end > start) painter.fillRect(QRect(start, plot.top(), end - start, plot.height()), QColor(45, 220, 95, 72));
        constexpr int ticks = 10;
        constexpr int subdivisions = 5;
        painter.setPen(QPen(QColor(70, 83, 99), 1));
        for (int minor = 0; minor <= ticks * subdivisions; ++minor) {
            if (minor % subdivisions == 0) continue;
            const int x = plot.left() + plot.width() * minor / (ticks * subdivisions);
            painter.drawLine(x, axisY - 3, x, axisY + 3);
        }
        for (int tick = 0; tick <= ticks; ++tick) {
            const int x = plot.left() + plot.width() * tick / ticks;
            painter.setPen(QPen(QColor(87, 101, 119), 1));
            painter.drawLine(x, axisY - 7, x, axisY + 7);
            const double frequency = frequencies_.front() + span * tick / ticks;
            painter.setPen(QColor(202, 211, 222));
            painter.drawText(QRect(x - 64, axisY + 8, 128, 18), Qt::AlignHCenter | Qt::AlignTop,
                             QString::number(frequency / 1.0e6, 'f', 4));
        }
        painter.setPen(QPen(QColor(255, 78, 78), 2.0));
        painter.drawLine(markerX, plot.top(), markerX, plot.bottom());
        QPolygon marker;
        marker << QPoint(markerX - 5, plot.top()) << QPoint(markerX + 5, plot.top())
               << QPoint(markerX, plot.top() + 7);
        painter.setBrush(QColor(255, 78, 78));
        painter.drawPolygon(marker);
    }

    void drawInteractionOverlay(QPainter &painter, const QRect &plot) const {
        if ((measurementVisible_ || areaMeasurementVisible_) && mode_ != Mode::Ruler) {
            QRect selection(measureStart_, measureEnd_);
            selection = selection.normalized().intersected(plot);
            if (mode_ == Mode::Spectrum && measurementVisible_ && selection.width() > 0) {
                selection.setTop(plot.top());
                selection.setBottom(plot.bottom());
            }
            if (!selection.isEmpty()) {
                QRegion outside(plot);
                outside = outside.subtracted(QRegion(selection));
                painter.save();
                painter.setClipRegion(outside);
                painter.fillRect(plot, QColor(3, 6, 10, 118));
                painter.restore();
                painter.setPen(QPen(QColor(118, 196, 255), 1.0, Qt::DashLine));
                painter.drawLine(selection.left(), plot.top(), selection.left(), plot.bottom());
                painter.drawLine(selection.right(), plot.top(), selection.right(), plot.bottom());
                painter.drawLine(plot.left(), selection.top(), plot.right(), selection.top());
                painter.drawLine(plot.left(), selection.bottom(), plot.right(), selection.bottom());
                const double low = frequencyAtX(selection.left());
                const double high = frequencyAtX(selection.right());
                QString text = QStringLiteral("%1 - %2 MHz   dF %3 kHz")
                                   .arg(low / 1.0e6, 0, 'f', 6)
                                   .arg(high / 1.0e6, 0, 'f', 6)
                                   .arg(std::abs(high - low) / 1000.0, 0, 'f', 2);
                if (mode_ == Mode::Spectrum && measurementVisible_ &&
                    frequencies_.size() >= 2 && levels_.size() == frequencies_.size()) {
                    const double plotWidth = (std::max)(1, plot.width());
                    const auto indexAt = [&](int x) {
                        const double ratio = std::clamp(double(x - plot.left()) / plotWidth, 0.0, 1.0);
                        return (std::min)(levels_.size() - 1,
                            std::size_t(std::llround(ratio * double(levels_.size() - 1))));
                    };
                    std::size_t first = indexAt(selection.left());
                    std::size_t last = indexAt(selection.right());
                    if (first > last) std::swap(first, last);
                    float minimum = std::numeric_limits<float>::infinity();
                    float maximum = -std::numeric_limits<float>::infinity();
                    double sum = 0.0;
                    std::size_t count = 0;
                    for (std::size_t index = first; index <= last && index < levels_.size(); ++index) {
                        const float level = levels_[index];
                        if (!std::isfinite(level)) continue;
                        minimum = (std::min)(minimum, level);
                        maximum = (std::max)(maximum, level);
                        sum += level;
                        ++count;
                    }
                    if (count > 0) {
                        const float average = float(sum / double(count));
                        const auto drawLevel = [&](float level, const QColor &color, const QString &caption) {
                            const int y = plot.bottom() - qRound(normalizedLevel(level) * plot.height());
                            painter.setPen(QPen(color, 1.2, Qt::DashLine));
                            painter.drawLine(plot.left(), y, plot.right(), y);
                            const QString labelText = QStringLiteral("%1 %2 dBFS").arg(caption).arg(level, 0, 'f', 1);
                            QRect levelLabel = painter.fontMetrics().boundingRect(labelText).adjusted(-4, -2, 4, 2);
                            levelLabel.moveTopRight(QPoint(selection.right() - 3,
                                std::clamp(y - levelLabel.height() - 2, plot.top(), plot.bottom() - levelLabel.height())));
                            painter.fillRect(levelLabel, QColor(0, 0, 0, 190));
                            painter.setPen(color);
                            painter.drawText(levelLabel, Qt::AlignCenter, labelText);
                        };
                        drawLevel(maximum, QColor(255, 92, 72), QStringLiteral("MAX"));
                        drawLevel(average, QColor(255, 211, 82), QStringLiteral("AVG"));
                        drawLevel(minimum, QColor(82, 196, 255), QStringLiteral("MIN"));
                        text += QStringLiteral("   MIN %1   AVG %2   MAX %3 dBFS")
                                    .arg(minimum, 0, 'f', 1)
                                    .arg(average, 0, 'f', 1)
                                    .arg(maximum, 0, 'f', 1);
                    }
                } else if (areaMeasurementVisible_) {
                    const double rows = waterfallImage_.isNull() || plot.height() <= 0
                        ? double(selection.height())
                        : double(selection.height()) * waterfallImage_.height() / double(plot.height());
                    if (displayedFps_ > 0.0)
                        text += QStringLiteral("   dT %1 s").arg(rows / displayedFps_, 0, 'f', 3);
                    else
                        text += QStringLiteral("   dY %1 px").arg(selection.height());
                }
                QRect label = painter.fontMetrics().boundingRect(text).adjusted(-5, -3, 5, 3);
                label.moveTopLeft(selection.topLeft() + QPoint(5, 5));
                if (label.right() > plot.right() - 3) label.moveRight(plot.right() - 3);
                painter.fillRect(label, QColor(0, 0, 0, 205));
                painter.setPen(QColor(225, 240, 255));
                painter.drawText(label, Qt::AlignCenter, text);
            }
        }
        if (hoverVisible_ && mode_ == Mode::Spectrum && plot.contains(hoverPosition_)) {
            const double frequency = frequencyAtX(hoverPosition_.x());
            float level = -240.0f;
            if (frequencies_.size() >= 2 && levels_.size() == frequencies_.size()) {
                const double ratio = std::clamp(double(hoverPosition_.x() - plot.left()) / plot.width(), 0.0, 1.0);
                const std::size_t index = (std::min)(levels_.size() - 1,
                    std::size_t(std::llround(ratio * double(levels_.size() - 1))));
                level = levels_[index];
            }
            const QString text = QStringLiteral("%1 MHz   %2 dBFS")
                                     .arg(frequency / 1.0e6, 0, 'f', 6)
                                     .arg(level, 0, 'f', 1);
            QRect label = painter.fontMetrics().boundingRect(text).adjusted(-5, -3, 5, 3);
            label.moveTopLeft(QPoint((std::min)(hoverPosition_.x() + 10, width() - label.width() - 3),
                                      (std::max)(3, hoverPosition_.y() - label.height() - 4)));
            painter.fillRect(label, QColor(0, 0, 0, 205));
            painter.setPen(QColor(235, 245, 235));
            painter.drawText(label, Qt::AlignCenter, text);
        }
    }
    void drawAnalogPeakMeter(QPainter &painter, const QRect &plot) const {
        if (!analogPeakMeterEnabled_ || levels_.empty() || frequencies_.size() != levels_.size()) return;
        std::size_t peakIndex = 0;
        if (analogPeakMeterTargetValid_ && frequencies_.size() > 1) {
            const double span = frequencies_.back() - frequencies_.front();
            const int center = span > 0.0
                ? std::clamp(int(std::llround((analogPeakMeterTargetHz_ - frequencies_.front()) /
                                              span * double(frequencies_.size() - 1))),
                             0, int(frequencies_.size() - 1))
                : 0;
            const int radius = (std::max)(4, int(frequencies_.size() / 120));
            peakIndex = std::size_t(center);
            for (int index = (std::max)(0, center - radius);
                 index <= (std::min)(int(levels_.size() - 1), center + radius); ++index) {
                if (std::isfinite(levels_[std::size_t(index)]) &&
                    levels_[std::size_t(index)] > levels_[peakIndex]) peakIndex = std::size_t(index);
            }
        } else {
            for (std::size_t index = 1; index < levels_.size(); ++index) {
                if (std::isfinite(levels_[index]) &&
                    (!std::isfinite(levels_[peakIndex]) || levels_[index] > levels_[peakIndex])) peakIndex = index;
            }
        }
        const float level = levels_[peakIndex];
        if (!std::isfinite(level)) return;
        const int meterWidth = std::clamp(plot.width() / 4, 138, 195);
        const int meterHeight = std::clamp(meterWidth * 54 / 100, 76, 106);
        const QRect meterRect(plot.left() + 7, plot.top() + 7, meterWidth, meterHeight);
        const QPointF center(meterRect.center().x(), meterRect.bottom() - 13.0);
        const qreal radius = (std::min)(meterRect.width() * 0.43, meterRect.height() * 0.80);
        const double meterMinimum = (std::min)(double(minimumDbfs_), -120.0);
        const double meterMaximum = (std::max)(double(maximumDbfs_), 0.0);
        const double normalized = std::clamp((double(level) - meterMinimum) /
                                                 (meterMaximum - meterMinimum), 0.0, 1.0);
        constexpr double startDegrees = 210.0;
        constexpr double sweepDegrees = 120.0;
        constexpr double pi = 3.14159265358979323846;
        const auto pointAt = [&](double degrees, double scale) {
            const double radians = degrees * pi / 180.0;
            return QPointF(center.x() + std::cos(radians) * radius * scale,
                           center.y() + std::sin(radians) * radius * scale);
        };
        const bool retro = analogPeakMeterStyle_ == 1;
        const QColor scaleColor = retro ? QColor(60, 45, 18) : QColor(183, 197, 210);
        const QColor textColor = retro ? QColor(52, 37, 12) : QColor(226, 234, 241);
        const QColor secondaryText = retro ? QColor(91, 62, 18) : QColor(146, 161, 177);
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing, true);
        if (retro) {
            painter.setPen(QPen(QColor(116, 79, 20), 1.2));
            painter.setBrush(QColor(242, 190, 72, 232));
            painter.drawRoundedRect(meterRect, 4.0, 4.0);
        } else {
            painter.setPen(QPen(QColor(140, 154, 169, 165), 1.0));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(meterRect, 4.0, 4.0);
        }
        for (int tick = 0; tick <= 10; ++tick) {
            const double degrees = startDegrees + sweepDegrees * tick / 10.0;
            painter.setPen(QPen(tick >= 8 ? QColor(210, 51, 38) : scaleColor,
                                tick % 5 == 0 ? 1.8 : 1.0));
            painter.drawLine(pointAt(degrees, tick % 5 == 0 ? 0.76 : 0.82), pointAt(degrees, 0.96));
            if (tick % 5 == 0) {
                painter.save();
                QFont scaleFont = painter.font();
                scaleFont.setPointSizeF((std::max)(6.0, scaleFont.pointSizeF() - 2.0));
                painter.setFont(scaleFont);
                painter.setPen(scaleColor);
                const QString label = QString::number(
                    meterMinimum + (meterMaximum - meterMinimum) * tick / 10.0, 'f', 0);
                const QPointF labelCenter = pointAt(degrees, 0.60);
                painter.drawText(QRectF(labelCenter.x() - 24.0, labelCenter.y() - 8.0, 48.0, 16.0),
                                 Qt::AlignCenter, label);
                painter.restore();
            }
        }
        const double needleDegrees = startDegrees + sweepDegrees * normalized;
        painter.setPen(QPen(QColor(255, 91, 72), 2.1, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(center, pointAt(needleDegrees, 0.83));
        painter.setPen(Qt::NoPen);
        painter.setBrush(retro ? QColor(55, 39, 12) : QColor(226, 232, 238));
        painter.drawEllipse(center, 3.5, 3.5);
        QFont font = painter.font();
        font.setPointSizeF((std::max)(7.0, font.pointSizeF() - 1.0));
        painter.setFont(font);
        painter.setPen(textColor);
        painter.drawText(meterRect.adjusted(7, 3, -7, -3), Qt::AlignTop | Qt::AlignLeft,
                         QStringLiteral("%1  %2 dBFS")
                             .arg(analogPeakMeterTargetValid_ ? QStringLiteral("MARKER") : QStringLiteral("PEAK"))
                             .arg(level, 0, 'f', 1));
        painter.setPen(secondaryText);
        painter.drawText(meterRect.adjusted(7, 3, -7, -5), Qt::AlignBottom | Qt::AlignCenter,
                         QStringLiteral("%1 MHz").arg(frequencies_[peakIndex] / 1.0e6, 0, 'f', 6));
        painter.restore();
    }

    Mode mode_;
    QString blockId_;
    WorkspaceDisplayCallbacks callbacks_;
    std::vector<float> frequencies_;
    std::vector<float> levels_;
    QImage waterfallImage_;
    int waterfallHead_ = 0;
    std::deque<std::vector<float>> waterfallHistory_;
    std::deque<std::vector<float>> capturedWaterfallHistory_;
    QJsonObject globalVisualizationSettings_;
    QJsonObject localSettings_;
    QJsonObject controllerSettings_;
    int waterfallDisplayMode_ = 0;
    int globalWaterfallDisplayMode_ = 0;
    int waterfallResolutionDivisor_ = 4;
    int waterfallHistoryLimit_ = 128;
    double centerHz_ = 0.0;
    double listeningHz_ = 0.0;
    double sampleRate_ = 0.0;
    double bandwidthHz_ = 0.0;
    int modulationType_ = MOD_AM;
    float minimumDbfs_ = -140.0f;
    float maximumDbfs_ = -30.0f;
    float globalMinimumDbfs_ = -140.0f;
    float globalMaximumDbfs_ = -30.0f;
    float contrast_ = 10.0f;
    float sensitivity_ = 10.0f;
    bool colorSpectrum_ = true;
    bool spectrumGradientFill_ = false;
    int spectrumGradientOpacity_ = 70;
    bool secondSpectrum_ = false;
    bool showSpectrumFps_ = false;
    bool showWaterfallFps_ = false;
    bool showExtendedInfo_ = false;
    bool areaMeasurementEnabled_ = false;
    bool waterfall3DFixedPlane_ = false;
    bool cameraOverrideActive_ = false;
    bool waterfall3DMonochrome_ = false;
    int waterfall3DSurfaceStyle_ = 0;
    int waterfall3DSmoothing_ = 0;
    int waterfall3DLighting_ = 0;
    bool spectrumSliceCapture_ = false;
    bool spectrumSliceCaptureFixed_ = false;
    bool modifierFreeSliceInput_ = false;
    int frequencySliceStep_ = 1;
    int frequencySliceWidth_ = 1;
    int spectrumSliceStep_ = 1;
    int spectrumSliceRows_ = 1;
    bool frequencySliceActive_ = false;
    bool spectrumSliceActive_ = false;
    bool suppressNextContextMenu_ = false;
    double selectedFrequencySlice_ = 0.5;
    int selectedSpectrumRow_ = 0;
    int sourceFftLength_ = 0;
    int fftWindowType_ = 0;
    bool hasCustomLevelRange_ = false;
    bool receiverRunning_ = false;
    bool paused_ = false;
    QToolButton *pauseControl_ = nullptr;
    bool analogPeakMeterEnabled_ = false;
    int analogPeakMeterStyle_ = 0;
    bool analogPeakMeterTargetValid_ = false;
    double analogPeakMeterTargetHz_ = 0.0;
    bool hoverVisible_ = false;
    bool measurementActive_ = false;
    bool measurementVisible_ = false;
    bool areaMeasurementActive_ = false;
    bool areaMeasurementVisible_ = false;
    bool multiVfoSelection_ = false;
    bool panActive_ = false;
    bool panMoved_ = false;
    bool rulerDrag_ = false;
    Qt::MouseButton panButton_ = Qt::NoButton;
    QPoint hoverPosition_;
    QPoint measureStart_;
    QPoint measureEnd_;
    QPoint panLast_;
    bool cameraDragActive_ = false;
    bool cameraPanDrag_ = false;
    QPoint cameraLast_;
    double cameraYaw_ = 0.0;
    double cameraTilt_ = 0.68;
    double cameraZoom_ = 1.0;
    double cameraPanX_ = 0.0;
    double cameraPanY_ = 0.0;
    QElapsedTimer updateTimer_;
    QElapsedTimer fpsTimer_;
    int fpsFrameCount_ = 0;
    double displayedFps_ = 0.0;
};

bool isMiniControlBlockType(const QString &type) {
    return type == QStringLiteral("run_control") || type == QStringLiteral("enable_control");
}

bool isVerticalControlSourceType(const QString &type) {
    return isMiniControlBlockType(type) || type == QStringLiteral("frequency_control") ||
           type == QStringLiteral("fine_tune_control");
}

bool acceptsControlType(const QString &targetType, const QString &controlType) {
    if (controlType == QStringLiteral("run_control")) {
        return targetType == QStringLiteral("receiver_source") || targetType == QStringLiteral("iq_source") ||
               targetType == QStringLiteral("network_input") || targetType == QStringLiteral("playback");
    }
    if (controlType == QStringLiteral("frequency_control") ||
        controlType == QStringLiteral("fine_tune_control")) {
        return targetType == QStringLiteral("receiver_source") || targetType == QStringLiteral("iq_source") ||
               targetType == QStringLiteral("network_input") || targetType == QStringLiteral("playback");
    }
    if (controlType != QStringLiteral("enable_control")) return false;
    static const QSet<QString> toggleable = {
        QStringLiteral("spectrum_display"), QStringLiteral("second_spectrum"),
        QStringLiteral("waterfall_3d"), QStringLiteral("audio_output"),
        QStringLiteral("decoder"), QStringLiteral("dmr_decoder"), QStringLiteral("cw_decoder"),
        QStringLiteral("sstv_decoder"), QStringLiteral("digital_video"),
        QStringLiteral("digital_audio_settings"), QStringLiteral("digital_video_settings"),
        QStringLiteral("agile_scan"), QStringLiteral("standard_scan"), QStringLiteral("listening_scan"),
        QStringLiteral("spectrum_measurement"), QStringLiteral("spur_suppression"),
        QStringLiteral("hf_interference"), QStringLiteral("gnss_sdr"), QStringLiteral("gnss_serial"),
        QStringLiteral("ddc_channelizer")
        , QStringLiteral("multi_vfo_channelizer")
    };
    return toggleable.contains(targetType);
}

bool acceptsAnyControl(const QString &targetType) {
    return acceptsControlType(targetType, QStringLiteral("run_control")) ||
           acceptsControlType(targetType, QStringLiteral("frequency_control")) ||
           acceptsControlType(targetType, QStringLiteral("enable_control"));
}

QColor blockColor(const QString &type) {
    if (type == QStringLiteral("receiver_source") || type == QStringLiteral("iq_source")) {
        return QColor(37, 115, 155);
    }
    if (type == QStringLiteral("channel_filter") || type == QStringLiteral("resampler") ||
        type == QStringLiteral("audio_filter") || type == QStringLiteral("multi_vfo_channelizer") ||
        type == QStringLiteral("vfo_channel")) {
        return QColor(43, 130, 91);
    }
    if (type == QStringLiteral("demodulator") || type == QStringLiteral("decoder")) {
        return QColor(148, 94, 36);
    }
    if (type == QStringLiteral("fft")) {
        return QColor(112, 76, 158);
    }
    if (type.contains(QStringLiteral("scan")) || type.contains(QStringLiteral("hunter"))) {
        return QColor(44, 126, 139);
    }
    if (type.contains(QStringLiteral("waterfall")) || type.contains(QStringLiteral("spectrum")) ||
        type == QStringLiteral("oscilloscope") || type == QStringLiteral("constellation") ||
        type == QStringLiteral("eye_diagram") || type == QStringLiteral("digital_sync_lab") ||
        isResearchViewType(type) ||
        type == QStringLiteral("zero_span") || type.startsWith(QStringLiteral("research_")) ||
        type == QStringLiteral("zoom_density")) {
        return QColor(112, 76, 158);
    }
    if (type.startsWith(QStringLiteral("gnss")) || type == QStringLiteral("qth_map")) {
        return QColor(45, 132, 98);
    }
    if (type.contains(QStringLiteral("decoder")) || type.startsWith(QStringLiteral("digital_"))) {
        return QColor(148, 94, 36);
    }
    return QColor(82, 92, 108);
}

bool typeHasInput(const QString &type) {
    static const QSet<QString> noInput = {
        QStringLiteral("receiver_source"), QStringLiteral("iq_source"), QStringLiteral("network_input"),
        QStringLiteral("run_control"), QStringLiteral("enable_control"), QStringLiteral("frequency_control"),
        QStringLiteral("fine_tune_control"), QStringLiteral("display_scale"),
        QStringLiteral("playback"), QStringLiteral("gnss_serial"), QStringLiteral("agile_scan"),
        QStringLiteral("standard_scan"), QStringLiteral("listening_scan"), QStringLiteral("qth_map"),
        QStringLiteral("gpio"), QStringLiteral("transmitter"), QStringLiteral("presets"),
        QStringLiteral("calibration"), QStringLiteral("spectrum_replay"),
        QStringLiteral("wireless_lab"),
        QStringLiteral("radio_propagation"),
        QStringLiteral("application_settings")
    };
    return !noInput.contains(type);
}

bool typeHasOutput(const QString &type) {
    if (type == QStringLiteral("branch_spectrum") || type == QStringLiteral("branch_waterfall")) return false;
    if (type.startsWith(QStringLiteral("research_")) ||
        type == QStringLiteral("zoom_density")) {
        return false;
    }
    static const QSet<QString> noOutput = {
        QStringLiteral("audio_output"), QStringLiteral("recorder"), QStringLiteral("network_output"),
        QStringLiteral("decoder"), QStringLiteral("second_spectrum"),
        QStringLiteral("spectrum_measurement"), QStringLiteral("zoom_spectrum"), QStringLiteral("zero_span"),
        QStringLiteral("research_analysis"), QStringLiteral("oscilloscope_view"),
        QStringLiteral("wireless_lab"),
        QStringLiteral("radio_propagation"),
        QStringLiteral("constellation_view"), QStringLiteral("eye_diagram_view"),
        QStringLiteral("digital_sync_view"),
        QStringLiteral("spur_suppression"),
        QStringLiteral("spectrum_recorder"), QStringLiteral("spectrum_replay"),
        QStringLiteral("dmr_decoder"), QStringLiteral("cw_decoder"), QStringLiteral("sstv_decoder"),
        QStringLiteral("digital_video"), QStringLiteral("digital_text_output"),
        QStringLiteral("digital_image_output"), QStringLiteral("vfo_spectrum"), QStringLiteral("vfo_waterfall"),
        QStringLiteral("dmr_hunter"), QStringLiteral("fpv_hunter"),
        QStringLiteral("digital_video_hunter"), QStringLiteral("gnss_sdr"), QStringLiteral("gnss_serial"),
        QStringLiteral("workspace_spectrum"), QStringLiteral("workspace_ruler"),
        QStringLiteral("workspace_waterfall"),
        QStringLiteral("qth_map"), QStringLiteral("agile_scan"), QStringLiteral("standard_scan"),
        QStringLiteral("listening_scan"), QStringLiteral("gpio"), QStringLiteral("presets"),
        QStringLiteral("calibration"), QStringLiteral("display_scale"),
        QStringLiteral("application_settings")
    };
    return !noOutput.contains(type);
}

QStringList allBlockTypes() {
    return {QStringLiteral("run_control"), QStringLiteral("enable_control"),
            QStringLiteral("receiver_source"), QStringLiteral("iq_source"),
            QStringLiteral("network_input"), QStringLiteral("playback"),
            QStringLiteral("frequency_control"), QStringLiteral("fine_tune_control"),
            QStringLiteral("display_scale"), QStringLiteral("channel_filter"),
            QStringLiteral("ddc_channelizer"), QStringLiteral("branch_fft"),
            QStringLiteral("branch_spectrum"), QStringLiteral("branch_waterfall"),
            QStringLiteral("multi_vfo_channelizer"), QStringLiteral("vfo_channel"),
            QStringLiteral("vfo_spectrum"), QStringLiteral("vfo_waterfall"),
            QStringLiteral("resampler"), QStringLiteral("fft"),
            QStringLiteral("hf_interference"),
            QStringLiteral("demodulator"), QStringLiteral("audio_filter"),
            QStringLiteral("filter_low_pass"), QStringLiteral("filter_high_pass"),
            QStringLiteral("filter_band_pass"), QStringLiteral("filter_notch"),
            QStringLiteral("filter_dc_blocker"), QStringLiteral("filter_de_emphasis"),
            QStringLiteral("filter_parametric_eq"), QStringLiteral("filter_low_shelf"),
            QStringLiteral("filter_high_shelf"), QStringLiteral("filter_adaptive_notch"),
            QStringLiteral("filter_noise_blanker"), QStringLiteral("filter_cw"),
            QStringLiteral("filter_ctcss"), QStringLiteral("filter_spectral_denoise"),
            QStringLiteral("filter_custom_fir"), QStringLiteral("filter_gain"),
            QStringLiteral("filter_compressor"), QStringLiteral("filter_limiter"),
            QStringLiteral("filter_noise_gate"), QStringLiteral("decoder"),
            QStringLiteral("digital_audio_settings"), QStringLiteral("digital_video_settings"),
            QStringLiteral("digital_text_output"), QStringLiteral("digital_image_output"),
            QStringLiteral("workspace_spectrum"), QStringLiteral("workspace_ruler"),
            QStringLiteral("workspace_waterfall"),
            QStringLiteral("spectrum_display"), QStringLiteral("waterfall_2d"),
            QStringLiteral("waterfall_3d"), QStringLiteral("second_spectrum"),
            QStringLiteral("agile_scan"), QStringLiteral("standard_scan"),
            QStringLiteral("listening_scan"), QStringLiteral("spectrum_measurement"),
            QStringLiteral("zoom_spectrum"), QStringLiteral("zoom_density"), QStringLiteral("zero_span"),
            QStringLiteral("research_analysis"), QStringLiteral("research_interference"),
            QStringLiteral("wireless_lab"),
            QStringLiteral("radio_propagation"),
            QStringLiteral("research_statistics"), QStringLiteral("research_iq"),
            QStringLiteral("research_dual_input"), QStringLiteral("research_analyzer"),
            QStringLiteral("research_density"), QStringLiteral("research_masks"),
            QStringLiteral("research_pulse"), QStringLiteral("research_session"),
            QStringLiteral("oscilloscope"),
            QStringLiteral("constellation"), QStringLiteral("eye_diagram"),
            QStringLiteral("digital_sync_lab"), QStringLiteral("oscilloscope_view"),
            QStringLiteral("constellation_view"), QStringLiteral("eye_diagram_view"),
            QStringLiteral("digital_sync_view"),
            QStringLiteral("spur_suppression"),
            QStringLiteral("spectrum_recorder"),
            QStringLiteral("spectrum_replay"), QStringLiteral("dmr_decoder"),
            QStringLiteral("cw_decoder"), QStringLiteral("sstv_decoder"),
            QStringLiteral("digital_video"), QStringLiteral("dmr_hunter"),
            QStringLiteral("fpv_hunter"), QStringLiteral("digital_video_hunter"),
            QStringLiteral("gnss_sdr"), QStringLiteral("gnss_serial"),
            QStringLiteral("qth_map"), QStringLiteral("gpio"),
            QStringLiteral("recorder"), QStringLiteral("network_output"),
            QStringLiteral("audio_output"), QStringLiteral("transmitter"),
            QStringLiteral("presets"), QStringLiteral("calibration"),
            QStringLiteral("application_settings")};
}

bool isFilterBlockType(const QString &type) {
    return type.startsWith(QStringLiteral("filter_"));
}

QString blockCategory(const QString &type) {
    if (type == QStringLiteral("branch_spectrum") || type == QStringLiteral("branch_waterfall")) return QStringLiteral("visualization");
    if (isFilterBlockType(type)) return QStringLiteral("filters");
    if (type.contains(QStringLiteral("scan")) || type.contains(QStringLiteral("hunter")))
        return QStringLiteral("scanning");
    if (type == QStringLiteral("display_scale") || type == QStringLiteral("spectrum_display") ||
        type.startsWith(QStringLiteral("waterfall_")) || type.startsWith(QStringLiteral("workspace_")) ||
        type == QStringLiteral("vfo_spectrum") || type == QStringLiteral("vfo_waterfall") ||
        type == QStringLiteral("second_spectrum") || type == QStringLiteral("oscilloscope") ||
        type == QStringLiteral("constellation") || type == QStringLiteral("eye_diagram") ||
        type == QStringLiteral("digital_sync_lab") || isResearchViewType(type))
        return QStringLiteral("visualization");
    if (type == QStringLiteral("spectrum_measurement") || type == QStringLiteral("zoom_spectrum") ||
        type == QStringLiteral("zoom_density") || type == QStringLiteral("zero_span") ||
        type.startsWith(QStringLiteral("research_")) ||
        type == QStringLiteral("spur_suppression") || type == QStringLiteral("calibration") ||
        type == QStringLiteral("spectrum_recorder") || type == QStringLiteral("spectrum_replay"))
        return QStringLiteral("measurement");
    if (type == QStringLiteral("decoder") || type == QStringLiteral("dmr_decoder") ||
        type == QStringLiteral("cw_decoder") || type == QStringLiteral("sstv_decoder") ||
        type.startsWith(QStringLiteral("digital_"))) return QStringLiteral("digital");
    if (type.startsWith(QStringLiteral("gnss_")) || type == QStringLiteral("qth_map"))
        return QStringLiteral("navigation");
    if (type == QStringLiteral("recorder") || type == QStringLiteral("network_input") ||
        type == QStringLiteral("network_output") || type == QStringLiteral("playback") ||
        type == QStringLiteral("audio_output") || type == QStringLiteral("transmitter") ||
        type == QStringLiteral("gpio") || type == QStringLiteral("presets") ||
        type == QStringLiteral("application_settings")) return QStringLiteral("io_tools");
    return QStringLiteral("signal_path");
}

QString categoryTitle(const QString &category, bool ukrainian) {
    if (category == QStringLiteral("signal_path")) return ukrainian ? QStringLiteral("Тракт сигналу") : QStringLiteral("Signal path");
    if (category == QStringLiteral("filters")) return ukrainian ? QStringLiteral("Аудіофільтри") : QStringLiteral("Audio filters");
    if (category == QStringLiteral("scanning")) return ukrainian ? QStringLiteral("Сканування і пошук") : QStringLiteral("Scanning and hunting");
    if (category == QStringLiteral("visualization")) return ukrainian ? QStringLiteral("Візуалізація") : QStringLiteral("Visualization");
    if (category == QStringLiteral("measurement")) return ukrainian ? QStringLiteral("Вимірювання і дослідження") : QStringLiteral("Measurement and research");
    if (category == QStringLiteral("digital")) return ukrainian ? QStringLiteral("Цифрові режими") : QStringLiteral("Digital modes");
    if (category == QStringLiteral("navigation")) return QStringLiteral("GNSS / QTH");
    return ukrainian ? QStringLiteral("Ввід, вивід і сервіс") : QStringLiteral("I/O and tools");
}

class DspSectorItem final : public QGraphicsRectItem {
public:
    explicit DspSectorItem(int index) : index_(index) {
        setAcceptedMouseButtons(Qt::NoButton);
        setFlag(ItemClipsChildrenToShape, true);
        setFlag(ItemIsSelectable, false);
        setHandlesChildEvents(false);
        setZValue(0.0);
    }

    void setSectorRect(const QRectF &sceneRect) {
        setPos(sceneRect.topLeft());
        setRect(QRectF(QPointF(0.0, 0.0), sceneRect.size()));
        update();
    }

    int index() const { return index_; }

protected:
    void paint(QPainter *painter, const QStyleOptionGraphicsItem *, QWidget *) override {
        const QColor background = (index_ % 2 == 0) ? QColor(17, 21, 27) : QColor(15, 19, 25);
        painter->fillRect(rect(), background);
        QPen border(QColor(55, 64, 75), 1.0, Qt::SolidLine);
        border.setCosmetic(true);
        painter->setPen(border);
        painter->setBrush(Qt::NoBrush);
        painter->drawRect(rect().adjusted(0.5, 0.5, -0.5, -0.5));
        const QRectF badge(8.0, 8.0, 30.0, 20.0);
        painter->fillRect(badge, QColor(26, 32, 40, 235));
        painter->setPen(QColor(132, 146, 161));
        painter->drawText(badge, Qt::AlignCenter, QString::number(index_ + 1));
    }

private:
    int index_ = 0;
};

class DspBlockItem final : public QGraphicsItem {
public:
    DspBlockItem(QString id, QString type, QString title, bool customTitle,
                 const std::function<void(double)> &fineTuneDelta = {}, int vfoIndex = -1,
                 const WorkspaceDisplayCallbacks &workspaceCallbacks = {})
        : id_(std::move(id)), type_(std::move(type)), title_(std::move(title)), customTitle_(customTitle),
          workspaceCallbacks_(workspaceCallbacks), vfoIndex_(vfoIndex) {
        setFlags(ItemIsMovable | ItemIsSelectable | ItemSendsGeometryChanges);
        setZValue(10.0);
        setCacheMode(NoCache);
        setCursor(Qt::OpenHandCursor);
        if (type_ == QStringLiteral("fine_tune_control")) {
            fineTuneWidget_ = new FineTuneScaleWidget();
            fineTuneWidget_->setFixedSize(int(kFineTuneBlockWidth - 16.0), 56);
            fineTuneProxy_ = new QGraphicsProxyWidget(this);
            fineTuneProxy_->setWidget(fineTuneWidget_);
            fineTuneProxy_->setPos(8.0, 42.0);
            QObject::connect(fineTuneWidget_, &FineTuneScaleWidget::fineTuneDelta,
                             fineTuneWidget_, [fineTuneDelta](double deltaHz) {
                if (fineTuneDelta) fineTuneDelta(deltaHz);
            });
        }
        if (type_ == QStringLiteral("vfo_spectrum") || type_ == QStringLiteral("vfo_waterfall") ||
            type_ == QStringLiteral("branch_spectrum") || type_ == QStringLiteral("branch_waterfall")) {
            miniVfoDisplay_ = new MiniVfoDisplay(type_ == QStringLiteral("vfo_waterfall") || type_ == QStringLiteral("branch_waterfall"));
            miniVfoDisplay_->setAttribute(Qt::WA_TransparentForMouseEvents);
            miniVfoDisplay_->setObjectName(QStringLiteral("dspPreview_%1").arg(id_));
            miniVfoProxy_ = new QGraphicsProxyWidget(this);
            miniVfoProxy_->setWidget(miniVfoDisplay_);
            miniVfoProxy_->setPos(8.0, 43.0);
        }
        if (isResearchViewType(type_)) {
            researchDisplay_ = new DspResearchWidget(researchModeForView(type_));
            researchDisplay_->setAttribute(Qt::WA_TransparentForMouseEvents);
            researchProxy_ = new QGraphicsProxyWidget(this);
            researchProxy_->setWidget(researchDisplay_);
            researchProxy_->setPos(8.0, 43.0);
        }
        if (type_ == QStringLiteral("workspace_spectrum") ||
            type_ == QStringLiteral("workspace_ruler") ||
            type_ == QStringLiteral("workspace_waterfall")) {
            const auto mode = type_ == QStringLiteral("workspace_spectrum")
                                  ? WorkspaceSpectrumDisplay::Mode::Spectrum
                                  : (type_ == QStringLiteral("workspace_ruler")
                                         ? WorkspaceSpectrumDisplay::Mode::Ruler
                                         : WorkspaceSpectrumDisplay::Mode::Waterfall);
            workspaceDisplay_ = new WorkspaceSpectrumDisplay(mode, id_);
            workspaceDisplay_->setCallbacks(workspaceCallbacks);
            workspaceProxy_ = new QGraphicsProxyWidget(this);
            workspaceProxy_->setWidget(workspaceDisplay_);
            workspaceProxy_->setPos(3.0, 3.0);
        }
        if (isVisualBlock()) {
            setCacheMode(NoCache);
            if (miniVfoProxy_) miniVfoProxy_->setCacheMode(NoCache);
            if (researchProxy_) researchProxy_->setCacheMode(NoCache);
            if (workspaceProxy_) workspaceProxy_->setCacheMode(NoCache);
            resizeHandleOverlay_ = new QGraphicsPathItem(this);
            resizeHandleOverlay_->setAcceptedMouseButtons(Qt::NoButton);
            resizeHandleOverlay_->setPen(QPen(QColor(238, 244, 250), 1.8,
                                              Qt::SolidLine, Qt::RoundCap));
            resizeHandleOverlay_->setBrush(QColor(91, 113, 139, 175));
            resizeHandleOverlay_->setZValue(250.0);
            updateResizeHandleOverlay();
            resizeEmbeddedWidget();
        }
    }

    ~DspBlockItem() override {
        if (nativeWaterfall_) {
            nativeWaterfall_->hide();
            delete nativeWaterfall_.data();
        }
    }

    QRectF boundingRect() const override {
        const qreal width = blockWidth();
        const qreal height = blockHeight();
        return QRectF(-kPortRadius - 2.0, -kPortRadius - 2.0,
                      width + (kPortRadius + 2.0) * 2.0,
                      height + (kPortRadius + 2.0) * 2.0);
    }

    void paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *) override {
        painter->setRenderHint(QPainter::Antialiasing, true);
        const QRectF body(0.0, 0.0, blockWidth(), blockHeight());
        const bool selected = option->state.testFlag(QStyle::State_Selected);
        if (isMiniControlBlock()) {
            const QColor fill = controlActive_ ? QColor(181, 55, 62) : QColor(39, 151, 83);
            painter->setPen(QPen(selected ? QColor(255, 205, 92) : fill.lighter(135), selected ? 2.0 : 1.2));
            painter->setBrush(fill);
            painter->drawRoundedRect(body, 7.0, 7.0);
            painter->setPen(Qt::white);
            QFont font = painter->font();
            font.setBold(true);
            painter->setFont(font);
            painter->drawText(body.adjusted(8.0, 3.0, -8.0, -3.0), Qt::AlignCenter,
                              controlLabel_.isEmpty() ? title_ : controlLabel_);
            painter->setPen(QPen(QColor(238, 224, 255), 1.0));
            painter->setBrush(QColor(178, 107, 230));
            painter->drawEllipse(controlTopPort(), kPortRadius, kPortRadius);
            painter->drawEllipse(controlBottomPort(), kPortRadius, kPortRadius);
            return;
        }
        if (isWorkspaceDisplayBlock()) {
            painter->setPen(QPen(selected ? QColor(255, 194, 72) : QColor(55, 64, 75),
                                 selected ? 2.0 : 1.0));
            painter->setBrush(QColor(4, 7, 12));
            painter->drawRect(body.adjusted(0.5, 0.5, -0.5, -0.5));
            return;
        }
        painter->setPen(QPen(selected ? QColor(255, 194, 72) : QColor(79, 88, 101),
                             selected ? 2.0 : 1.0));
        painter->setBrush(QColor(31, 35, 42));
        painter->drawRoundedRect(body, 6.0, 6.0);
        painter->setPen(Qt::NoPen);
        painter->setBrush(blockColor(type_));
        painter->drawRoundedRect(QRectF(1.0, 1.0, blockWidth() - 2.0, 9.0), 5.0, 5.0);
        painter->drawRect(QRectF(1.0, 6.0, blockWidth() - 2.0, 5.0));

        painter->setPen(QColor(239, 242, 246));
        QFont titleFont = painter->font();
        titleFont.setBold(true);
        painter->setFont(titleFont);
        painter->drawText(QRectF(13.0, 18.0, blockWidth() - 26.0, 25.0),
                          Qt::AlignCenter | Qt::TextSingleLine,
                          painter->fontMetrics().elidedText(
                              settings_.value(QStringLiteral("bindingId")).toString().trimmed().isEmpty()
                                  ? title_
                                  : QStringLiteral("%1  [%2]").arg(
                                        title_, settings_.value(QStringLiteral("bindingId")).toString().trimmed()),
                              Qt::ElideRight, int(blockWidth() - 28.0)));
        QFont typeFont = painter->font();
        typeFont.setBold(false);
        typeFont.setPointSizeF(std::max(7.0, typeFont.pointSizeF() - 1.0));
        painter->setFont(typeFont);
        painter->setPen(QColor(151, 161, 174));
        if (!isVisualBlock()) {
            painter->drawText(QRectF(12.0, 43.0, blockWidth() - 24.0, 17.0),
                              Qt::AlignCenter | Qt::TextSingleLine, type_.toUpper());
        }

        painter->setPen(QPen(QColor(219, 226, 234), 1.0));
        if (hasInput()) {
            painter->setBrush(QColor(87, 181, 255));
            painter->drawEllipse(inputPort(), kPortRadius, kPortRadius);
        }
        if (hasOutput()) {
            if (isVerticalControlSource()) {
                painter->setBrush(QColor(178, 107, 230));
                painter->drawEllipse(controlTopPort(), kPortRadius, kPortRadius);
                painter->drawEllipse(controlBottomPort(), kPortRadius, kPortRadius);
            } else {
                painter->setBrush(QColor(255, 172, 72));
                painter->drawEllipse(outputPort(), kPortRadius, kPortRadius);
            }
        }
        if (acceptsAnyControl(type_)) {
            painter->setBrush(QColor(178, 107, 230));
            painter->drawEllipse(controlTopPort(), kPortRadius, kPortRadius);
            painter->drawEllipse(controlBottomPort(), kPortRadius, kPortRadius);
        }

    }

    QString id() const { return id_; }
    QString blockType() const { return type_; }
    QString title() const { return title_; }
    bool customTitle() const { return customTitle_; }
    bool isMiniControlBlock() const { return isMiniControlBlockType(type_); }
    bool isVerticalControlSource() const { return isVerticalControlSourceType(type_); }
    bool isWorkspaceDisplayBlock() const { return workspaceDisplay_ != nullptr; }
    bool isWorkspaceGroupLocked() const {
        if (!isWorkspaceDisplayBlock()) return false;
        const bool grouped = settings_.value(QStringLiteral("workspaceGroupLayout")).toBool(false);
        return settings_.value(QStringLiteral("workspaceGroupLocked")).toBool(grouped);
    }
    bool isVisualBlock() const { return miniVfoDisplay_ || researchDisplay_ || workspaceDisplay_; }
    bool researchCameraAt(const QPointF &scenePosition) const {
        return researchDisplay_ && researchProxy_ &&
               !researchDisplay_->testAttribute(Qt::WA_TransparentForMouseEvents) &&
               researchProxy_->contains(researchProxy_->mapFromScene(scenePosition));
    }
    qreal blockWidth() const {
        if (type_ == QStringLiteral("fine_tune_control")) return kFineTuneBlockWidth;
        if (isVisualBlock()) return visualWidth_;
        return isMiniControlBlock() ? kMiniBlockWidth : kBlockWidth;
    }
    qreal blockHeight() const {
        if (type_ == QStringLiteral("fine_tune_control")) return kFineTuneBlockHeight;
        if (isVisualBlock()) return visualHeight_;
        return isMiniControlBlock() ? kMiniBlockHeight : kBlockHeight;
    }
    qreal visualWidth() const { return visualWidth_; }
    qreal visualHeight() const { return visualHeight_; }
    void setVisualSize(qreal width, qreal height) {
        if (!isVisualBlock()) return;
        width = std::clamp(width, kVisualBlockMinWidth, kVisualBlockMaxWidth);
        const bool compactWorkspaceGroup = isWorkspaceDisplayBlock() &&
                                           settings_.value(QStringLiteral("workspaceGroupLayout")).toBool(false);
        const qreal minimumHeight = type_ == QStringLiteral("workspace_ruler")
                                        ? (compactWorkspaceGroup ? 34.0 : 58.0)
                                        : (compactWorkspaceGroup ? 48.0 : kVisualBlockMinHeight);
        height = std::clamp(height, minimumHeight, kVisualBlockMaxHeight);
        if (qFuzzyCompare(width + 1.0, visualWidth_ + 1.0) &&
            qFuzzyCompare(height + 1.0, visualHeight_ + 1.0)) return;
        prepareGeometryChange();
        visualWidth_ = width;
        visualHeight_ = height;
        updateResizeHandleOverlay();
        resizeEmbeddedWidget();
        updateConnections();
        update();
    }
    QJsonObject settings() const { return settings_; }
    void setSettings(const QJsonObject &settings) {
        settings_ = settings;
        const bool managedWorkspaceGroup =
            isWorkspaceDisplayBlock() &&
            settings_.value(QStringLiteral("workspaceGroupLayout")).toBool(false);
        setScale(managedWorkspaceGroup
                     ? 1.0
                     : std::clamp(settings_.value(QStringLiteral("workspaceCanvasScale")).toDouble(1.0),
                                  0.35, 2.5));
        const bool groupLocked = isWorkspaceGroupLocked();
        if (!resizing_) setFlag(ItemIsMovable, !groupLocked);
        if (resizeHandleOverlay_) resizeHandleOverlay_->setVisible(!groupLocked);
        setCursor(groupLocked ? Qt::ArrowCursor : Qt::OpenHandCursor);
        if (researchDisplay_) researchDisplay_->setSettings(settings_);
        if (workspaceDisplay_) workspaceDisplay_->setSettings(settings_);
        applyNativeWaterfallSettings();
        if (miniVfoDisplay_) {
            miniVfoDisplay_->setDbfsRange(
                static_cast<float>(settings_.value(QStringLiteral("minimumDbfs")).toDouble(-140.0)),
                static_cast<float>(settings_.value(QStringLiteral("maximumDbfs")).toDouble(-40.0)));
        }
        update();
    }
    void setResearchConnection(bool connected, const QJsonObject &settings, bool ukrainian) {
        if (!researchDisplay_) return;
        researchDisplay_->setSettings(settings);
        researchDisplay_->setConnected(connected, ukrainian);
    }
    void setResearchExternalInput(bool external) {
        if (researchDisplay_) researchDisplay_->setExternalInput(external);
    }
    void setResearchFrame(const DspBranchFrame &frame) {
        if (researchDisplay_) researchDisplay_->setExternalFrame(frame.iq ? *frame.iq : std::vector<float>(), frame.sampleRateHz, frame.sequence);
    }
    void setBranchSpectrum(const DspBranchFrame &frame, const QString &label) {
        if (miniVfoDisplay_) miniVfoDisplay_->setSpectrum(frame.frequencies, frame.levels, frame.centerHz, frame.sampleRateHz, label, true);
    }
    bool hasInput() const { return typeHasInput(type_); }
    bool hasOutput() const { return typeHasOutput(type_); }
    QPointF inputPort() const { return QPointF(0.0, blockHeight() * 0.5); }
    QPointF outputPort() const { return QPointF(blockWidth(), blockHeight() * 0.5); }
    QPointF controlTopPort() const { return QPointF(blockWidth() * 0.5, 0.0); }
    QPointF controlBottomPort() const { return QPointF(blockWidth() * 0.5, blockHeight()); }
    QPointF inputScenePos() const { return mapToScene(inputPort()); }
    QPointF outputScenePos() const { return mapToScene(outputPort()); }
    QPointF controlOutputScenePos(const QPointF &targetPosition) const {
        return mapToScene(targetPosition.y() < sceneBoundingRect().center().y() ? controlTopPort() : controlBottomPort());
    }
    QPointF controlInputScenePos(const QPointF &sourcePosition) const {
        return mapToScene(sourcePosition.y() < sceneBoundingRect().center().y() ? controlTopPort() : controlBottomPort());
    }
    bool inputContains(const QPointF &pos) const {
        return hasInput() && QLineF(inputScenePos(), pos).length() <= kPortRadius + 10.0;
    }
    bool outputContains(const QPointF &pos) const {
        if (!hasOutput()) return false;
        if (isVerticalControlSource()) {
            return QLineF(mapToScene(controlTopPort()), pos).length() <= kPortRadius + 10.0 ||
                   QLineF(mapToScene(controlBottomPort()), pos).length() <= kPortRadius + 10.0;
        }
        return QLineF(outputScenePos(), pos).length() <= kPortRadius + 10.0;
    }
    bool controlInputContains(const QPointF &pos, const QString &controlType) const {
        if (!acceptsControlType(type_, controlType)) return false;
        return QLineF(mapToScene(controlTopPort()), pos).length() <= kPortRadius + 10.0 ||
               QLineF(mapToScene(controlBottomPort()), pos).length() <= kPortRadius + 10.0;
    }
    bool bodyContainsScenePoint(const QPointF &pos) const {
        return QRectF(0.0, 0.0, blockWidth(), blockHeight()).contains(mapFromScene(pos));
    }
    bool resizeHandleContainsScenePoint(const QPointF &pos) const {
        return isVisualBlock() && resizeHandleHitRect().contains(mapFromScene(pos));
    }
    void beginSceneResize(const QPointF &scenePos) {
        QJsonObject settings = settings_;
        settings.insert(QStringLiteral("workspaceAutoFit"), false);
        settings.insert(QStringLiteral("workspaceAutoWidth"), false);
        settings.remove(QStringLiteral("workspaceStackRole"));
        settings.insert(QStringLiteral("workspaceGroupLayout"), false);
        settings.remove(QStringLiteral("workspaceGroupId"));
        settings.remove(QStringLiteral("workspaceGroupOrder"));
        settings.remove(QStringLiteral("workspaceGroupCuts"));
        setSettings(settings);
        resizing_ = true;
        resizeStartScene_ = scenePos;
        resizeStartSize_ = QSizeF(visualWidth_, visualHeight_);
        setFlag(ItemIsMovable, false);
        setCursor(Qt::SizeFDiagCursor);
    }
    void continueSceneResize(const QPointF &scenePos) {
        if (!resizing_) return;
        const QPointF delta = scenePos - resizeStartScene_;
        setVisualSize(resizeStartSize_.width() + delta.x(),
                      resizeStartSize_.height() + delta.y());
    }
    void finishSceneResize() {
        resizing_ = false;
        setFlag(ItemIsMovable, !isWorkspaceGroupLocked());
        setCursor(Qt::OpenHandCursor);
    }
    void setTitle(const QString &title, bool custom) {
        title_ = title;
        customTitle_ = custom;
        update();
    }
    void setControlState(bool active, bool ukrainian) {
        controlActive_ = active;
        if (type_ == QStringLiteral("run_control"))
            controlLabel_ = active ? (ukrainian ? QStringLiteral("СТОП") : QStringLiteral("STOP"))
                                   : (ukrainian ? QStringLiteral("СТАРТ") : QStringLiteral("START"));
        else if (type_ == QStringLiteral("enable_control"))
            controlLabel_ = active ? (ukrainian ? QStringLiteral("ВИКЛ") : QStringLiteral("OFF"))
                                   : (ukrainian ? QStringLiteral("ВКЛ") : QStringLiteral("ON"));
        update();
    }
    void setFineTuneRangeHz(double rangeHz) {
        if (fineTuneWidget_) fineTuneWidget_->setRangeHz(rangeHz);
    }
    int vfoIndex() const { return vfoIndex_; }
    void setVfoIndex(int index) {
        vfoIndex_ = index;
        update();
    }
    void setMiniVfoSpectrum(const std::vector<float> &frequencies,
                            const std::vector<float> &levels,
                            double centerHz,
                            double bandwidthHz,
                            const QString &name) {
        if (miniVfoDisplay_) miniVfoDisplay_->setSpectrum(frequencies, levels, centerHz, bandwidthHz, name);
    }
    void setMiniVfoStatus(const QString &status) {
        if (miniVfoDisplay_) miniVfoDisplay_->setStatus(status);
    }
    void attachNativeWorkspaceView(QGraphicsView *view) {
        if (type_ != QStringLiteral("workspace_waterfall") || !view || !view->viewport()) return;
        if (nativeWaterfall_ && nativeView_ == view) {
            syncNativeWaterfallGeometry();
            return;
        }
        if (nativeWaterfall_) delete nativeWaterfall_.data();
        nativeView_ = view;
        nativeWaterfall_ = new MyWaterfallWidget(view->viewport());
        nativeWaterfall_->setObjectName(QStringLiteral("dspWorkspaceNativeWaterfall_%1").arg(id_));
        nativeWaterfall_->setPauseControlVisible(true);
        nativeWaterfall_->setAlternativeInterfaceMode(false);
        nativeWaterfall_->setTuneContextEnabled(false);
        nativeWaterfall_->setMinimumSize(1, 1);
        nativeWaterfall_->hide();
        QObject::connect(nativeWaterfall_, &MyWaterfallWidget::scaleChanged,
                         nativeWaterfall_, [this](int direction) {
            if (workspaceCallbacks_.scale) workspaceCallbacks_.scale(direction);
        });
        QObject::connect(nativeWaterfall_, &MyWaterfallWidget::panRequested,
                         nativeWaterfall_, [this](int deltaPixels, int widthPixels) {
            if (workspaceCallbacks_.pan) workspaceCallbacks_.pan(deltaPixels, widthPixels);
        });
        QObject::connect(nativeWaterfall_, &MyWaterfallWidget::tuneContextRequested,
                         nativeWaterfall_, [this](double frequency, const QPoint &globalPos) {
            if (workspaceCallbacks_.tuneContext)
                workspaceCallbacks_.tuneContext(frequency, globalPos);
        });
        QObject::connect(nativeWaterfall_, &MyWaterfallWidget::autoTuneRequested,
                         nativeWaterfall_, [this](double frequency) {
            if (workspaceCallbacks_.autoTune) workspaceCallbacks_.autoTune(frequency);
        });
        QObject::connect(nativeWaterfall_, &MyWaterfallWidget::displayPausedChanged,
                         nativeWaterfall_, [this](bool paused) {
            if (workspaceCallbacks_.pauseToggled)
                workspaceCallbacks_.pauseToggled(id_, paused);
        });
        applyNativeWaterfallSettings();
        syncNativeWaterfallGeometry();
    }

void syncNativeWaterfallGeometry() {
        if (!nativeWaterfall_ || !nativeView_ || !nativeView_->viewport()) return;
        const bool shouldShow = nativeWaterfall3DActive_ && isVisible() && scene() &&
                                nativeView_->isVisible();
        if (!shouldShow) {
            if (nativeWaterfall_->isVisible()) nativeWaterfall_->hide();
            queueNativeWaterfallPresentation(false);
            return;
        }

        const QRectF localContent(3.0 + kWorkspacePlotLeftMargin,
                                  3.0,
                                  (std::max)(1.0,
                                             visualWidth_ - 6.0 -
                                                 kWorkspacePlotLeftMargin -
                                                 kWorkspacePlotRightMargin),
                                  (std::max)(1.0, visualHeight_ - 6.0));
        const QRect viewportRect = nativeView_->mapFromScene(mapRectToScene(localContent))
                                       .boundingRect()
                                       .normalized();
        QRect clippedRect = viewportRect.intersected(nativeView_->viewport()->rect());
        if (auto *sector = dynamic_cast<DspSectorItem *>(parentItem())) {
            const QRect sectorRect = nativeView_->mapFromScene(sector->mapRectToScene(sector->rect()))
                                         .boundingRect()
                                         .normalized();
            clippedRect = clippedRect.intersected(sectorRect);
        }
        if (viewportRect.width() < 2 || viewportRect.height() < 2 || clippedRect.isEmpty()) {
            if (nativeWaterfall_->isVisible()) nativeWaterfall_->hide();
            queueNativeWaterfallPresentation(false);
            return;
        }

        QRegion visibleRegion(clippedRect.translated(-viewportRect.topLeft()));
        if (!isWorkspaceGroupLocked()) {
            const QRect handleRect = nativeView_->mapFromScene(mapRectToScene(resizeHandleHitRect()))
                                         .boundingRect()
                                         .normalized()
                                         .translated(-viewportRect.topLeft());
            visibleRegion -= QRegion(handleRect);
        }
        const bool presentationChanged = nativeWaterfall_->geometry() != viewportRect ||
                                         nativeWaterfall_->mask() != visibleRegion;
        if (presentationChanged || !nativeWaterfall_->isVisible()) {
            nativeWaterfall_->hide();
            nativeWaterfall_->setGeometry(viewportRect);
            nativeWaterfall_->setMask(visibleRegion);
            queueNativeWaterfallPresentation(true);
            return;
        }
        nativeWaterfall_->raise();
    }
    void setWorkspaceSpectrum(const std::vector<float> &frequencies,
                              const std::vector<float> &levels,
                              double centerHz,
                              double listeningHz,
                              double sampleRate,
                              double bandwidthHz,
                              int modulationType) {
        if (nativeWaterfall_ && nativeWaterfall3DActive_) {
            if (frequencies.empty() || levels.empty() || frequencies.size() != levels.size()) return;
            double minimumFrequency = static_cast<double>(frequencies.front());
            double maximumFrequency = static_cast<double>(frequencies.back());
            if (!std::isfinite(minimumFrequency) || !std::isfinite(maximumFrequency) ||
                qFuzzyCompare(minimumFrequency, maximumFrequency)) {
                minimumFrequency = centerHz - sampleRate * 0.5;
                maximumFrequency = centerHz + sampleRate * 0.5;
            }
            nativeWaterfall_->setSpectrumMetadata(centerHz, listeningHz, sampleRate,
                                                   nativeSourceFftLength_, nativeFftWindowType_);
            nativeWaterfall_->setData(frequencies, levels,
                                      minimumFrequency, maximumFrequency,
                                      static_cast<int>(levels.size()),
                                      nativeSecondGraph_, nativeColorSpectrum_,
                                      nativeContrast_, nativeSensitivity_,
                                      nativeMinimumDbfs_, nativeMaximumDbfs_, true);
            return;
        }
        if (workspaceDisplay_) {
            workspaceDisplay_->setFrame(frequencies, levels, centerHz, listeningHz, sampleRate,
                                        bandwidthHz, modulationType);
        }
    }
    void setWorkspaceAnalogPeakMeterEnabled(bool enabled) {
        if (workspaceDisplay_) workspaceDisplay_->setAnalogPeakMeterEnabled(enabled);
    }
    void setWorkspaceAnalogPeakMeterStyle(int style) {
        if (workspaceDisplay_) workspaceDisplay_->setAnalogPeakMeterStyle(style);
    }
    void setWorkspaceAnalogPeakMeterTarget(double frequencyHz, bool valid) {
        if (workspaceDisplay_) workspaceDisplay_->setAnalogPeakMeterTarget(frequencyHz, valid);
    }
    void setWorkspaceVisualizationSettings(const QJsonObject &settings) {
        if (workspaceVisualizationSettings_ == settings) return;
        workspaceVisualizationSettings_ = settings;
        if (workspaceDisplay_) workspaceDisplay_->setVisualizationSettings(settings);
        applyNativeWaterfallSettings();
    }
    void setWorkspaceControllerSettings(const QJsonObject &settings) {
        if (workspaceControllerSettings_ == settings) return;
        workspaceControllerSettings_ = settings;
        if (workspaceDisplay_) workspaceDisplay_->setControllerSettings(settings);
        applyNativeWaterfallSettings();
    }
    void setWorkspaceReceiverRunning(bool running) {
        if (workspaceDisplay_) workspaceDisplay_->setReceiverRunning(running);
    }
    void addConnection(DspConnectionItem *edge) {
        if (edge && !connections_.contains(edge)) connections_.append(edge);
    }
    void removeConnection(DspConnectionItem *edge) { connections_.removeAll(edge); }
    QList<DspConnectionItem*> connections() const { return connections_; }

protected:
    QVariant itemChange(GraphicsItemChange change, const QVariant &value) override;
    void mousePressEvent(QGraphicsSceneMouseEvent *event) override {
        if (isWorkspaceGroupLocked()) {
            setCursor(Qt::ArrowCursor);
            QGraphicsItem::mousePressEvent(event);
            return;
        }
        if (isVisualBlock() && event->button() == Qt::LeftButton && resizeHandleHitRect().contains(event->pos())) {
            QJsonObject settings = settings_;
            settings.insert(QStringLiteral("workspaceAutoFit"), false);
            settings.insert(QStringLiteral("workspaceAutoWidth"), false);
            settings.remove(QStringLiteral("workspaceStackRole"));
            settings.insert(QStringLiteral("workspaceGroupLayout"), false);
        settings.remove(QStringLiteral("workspaceGroupId"));
        settings.remove(QStringLiteral("workspaceGroupOrder"));
        settings.remove(QStringLiteral("workspaceGroupCuts"));
            setSettings(settings);
            resizing_ = true;
            resizeStartScene_ = event->scenePos();
            resizeStartSize_ = QSizeF(visualWidth_, visualHeight_);
            setFlag(ItemIsMovable, false);
            setCursor(Qt::SizeFDiagCursor);
            event->accept();
            return;
        }
        setCursor(Qt::ClosedHandCursor);
        QGraphicsItem::mousePressEvent(event);
    }
    void mouseMoveEvent(QGraphicsSceneMouseEvent *event) override {
        if (resizing_) {
            const QPointF delta = event->scenePos() - resizeStartScene_;
            setVisualSize(resizeStartSize_.width() + delta.x(), resizeStartSize_.height() + delta.y());
            event->accept();
            return;
        }
        QGraphicsItem::mouseMoveEvent(event);
    }
    void mouseReleaseEvent(QGraphicsSceneMouseEvent *event) override {
        if (resizing_) {
            resizing_ = false;
            setFlag(ItemIsMovable, !isWorkspaceGroupLocked());
            setCursor(Qt::OpenHandCursor);
            event->accept();
            return;
        }
        setCursor(isWorkspaceGroupLocked() ? Qt::ArrowCursor : Qt::OpenHandCursor);
        QGraphicsItem::mouseReleaseEvent(event);
    }

private:
    void queueNativeWaterfallPresentation(bool showAfterRefresh) {
        nativeShowRequested_ = showAfterRefresh;
        if (nativePresentationQueued_ || !nativeWaterfall_) return;
        nativePresentationQueued_ = true;
        QTimer::singleShot(0, nativeWaterfall_.data(), [this]() {
            nativePresentationQueued_ = false;
            if (!nativeWaterfall_ || !nativeView_ || !nativeView_->viewport()) return;
            nativeView_->viewport()->repaint();
            if (nativeShowRequested_ && nativeWaterfall3DActive_ && isVisible() && scene() &&
                nativeView_->isVisible()) {
                nativeWaterfall_->show();
                nativeWaterfall_->raise();
            }
        });
    }
    void applyNativeWaterfallSettings() {
        const auto value = [this](const QString &key, const QJsonValue &fallback) {
            if (workspaceControllerSettings_.contains(key))
                return workspaceControllerSettings_.value(key);
            if (settings_.contains(key)) return settings_.value(key);
            if (workspaceVisualizationSettings_.contains(key))
                return workspaceVisualizationSettings_.value(key);
            return fallback;
        };

        const int displayMode = std::clamp(
            value(QStringLiteral("displayMode"), 0).toInt(), 0, 2);
        const bool wasNativeActive = nativeWaterfall3DActive_;
        nativeWaterfall3DActive_ = displayMode != 0;
        nativeSecondGraph_ = value(QStringLiteral("secondSpectrum"), false).toBool();
        nativeColorSpectrum_ = value(QStringLiteral("colorSpectrum"), true).toBool();
        nativeContrast_ = static_cast<float>((std::clamp)(
            value(QStringLiteral("contrast"), 10.0).toDouble(), 1.0, 20.0));
        nativeSensitivity_ = static_cast<float>((std::clamp)(
            value(QStringLiteral("sensitivity"), 10.0).toDouble(), 1.0, 30.0));
        nativeSourceFftLength_ = (std::max)(
            0, value(QStringLiteral("fftLength"), 0).toInt());
        nativeFftWindowType_ = value(QStringLiteral("fftWindowType"), 0).toInt();

        const float globalMinimum = static_cast<float>((std::clamp)(
            workspaceVisualizationSettings_.value(QStringLiteral("minimumDbfs"))
                .toDouble(-140.0), -200.0, 19.0));
        const float globalMaximum = static_cast<float>((std::clamp)(
            workspaceVisualizationSettings_.value(QStringLiteral("maximumDbfs"))
                .toDouble(-30.0), double(globalMinimum + 1.0f), 20.0));
        const bool controllerLevelOverride =
            workspaceControllerSettings_.value(QStringLiteral("levelOverride")).toBool(false);
        const bool localLevelOverride =
            settings_.value(QStringLiteral("levelOverride")).toBool(false);
        const QJsonObject *levelSource = controllerLevelOverride
                                             ? &workspaceControllerSettings_
                                             : (localLevelOverride ? &settings_ : nullptr);
        nativeMinimumDbfs_ = levelSource
            ? static_cast<float>((std::clamp)(
                  levelSource->value(QStringLiteral("minimumDbfs")).toDouble(globalMinimum),
                  -200.0, 19.0))
            : globalMinimum;
        nativeMaximumDbfs_ = levelSource
            ? static_cast<float>((std::clamp)(
                  levelSource->value(QStringLiteral("maximumDbfs")).toDouble(globalMaximum),
                  double(nativeMinimumDbfs_ + 1.0f), 20.0))
            : globalMaximum;

        if (workspaceProxy_)
            workspaceProxy_->setVisible(!nativeWaterfall3DActive_ || !nativeWaterfall_);
        if (!nativeWaterfall_) return;

        const auto requestedMode = static_cast<MyWaterfallWidget::DisplayMode>(displayMode);
        if (nativeWaterfall_->displayMode() != requestedMode)
            nativeWaterfall_->setDisplayMode(requestedMode);
        nativeWaterfall_->setRowsPerFrame(std::clamp(
            value(QStringLiteral("rowsPerFrame"), 1).toInt(), 1, 8));
        nativeWaterfall_->setRenderBackend(
            value(QStringLiteral("gpuPrepared"), false).toBool()
                ? MyWaterfallWidget::RenderBackend::GpuPrepared
                : MyWaterfallWidget::RenderBackend::CpuTexture);
        nativeWaterfall_->set3DResolutionDivisor(std::clamp(
            value(QStringLiteral("resolutionDivisor"), 4).toInt(), 1, 64));
        nativeWaterfall_->set3DHistoryRows(std::clamp(
            value(QStringLiteral("historyRows"), 128).toInt(), 16, 2048));
        nativeWaterfall_->set3DSurfaceStyle(std::clamp(
            value(QStringLiteral("waterfall3DSurfaceStyle"), 0).toInt(), 0, 1));
        nativeWaterfall_->set3DSurfaceSmoothing(std::clamp(
            value(QStringLiteral("waterfall3DSmoothing"), 0).toInt(), 0, 2));
        nativeWaterfall_->set3DSurfaceLighting(std::clamp(
            value(QStringLiteral("waterfall3DLighting"), 0).toInt(), 0, 2));
        nativeWaterfall_->set3DSliceScrollStep(std::clamp(
            value(QStringLiteral("waterfall3DSliceScrollStep"), 1).toInt(), 1, 256));
        nativeWaterfall_->set3DSliceWidth(std::clamp(
            value(QStringLiteral("waterfall3DSliceWidth"), 1).toInt(), 1, 4096));
        nativeWaterfall_->set3DSpectrumSliceScrollStep(std::clamp(
            value(QStringLiteral("waterfall3DSpectrumSliceScrollStep"), 1).toInt(), 1, 2048));
        nativeWaterfall_->set3DSpectrumSliceWidth(std::clamp(
            value(QStringLiteral("waterfall3DSpectrumSliceRows"), 1).toInt(), 1, 2048));
        nativeWaterfall_->set3DSpectrumSliceCapture(
            value(QStringLiteral("waterfall3DSpectrumSliceCapture"), false).toBool());
        nativeWaterfall_->set3DSpectrumSliceCaptureFixed(
            value(QStringLiteral("waterfall3DSpectrumSliceCaptureFixed"), false).toBool());
        nativeWaterfall_->set3DModifierFreeSliceInput(
            value(QStringLiteral("waterfall3DVncSliceInput"), false).toBool());
        const bool fixedPlane =
            value(QStringLiteral("waterfall3DFixedPlane"), false).toBool();
        if (!nativeFixedPlaneInitialized_ || nativeAppliedFixedPlane_ != fixedPlane) {
            nativeWaterfall_->set3DFixedPlane(fixedPlane);
            nativeAppliedFixedPlane_ = fixedPlane;
            nativeFixedPlaneInitialized_ = true;
        }
        nativeWaterfall_->set3DMonochrome(
            value(QStringLiteral("waterfall3DMonochrome"), false).toBool());
        nativeWaterfall_->setAlternativeSpectrumGradientFill(
            value(QStringLiteral("spectrumGradientFill"), false).toBool());
        nativeWaterfall_->setAlternativeSpectrumGradientOpacity(std::clamp(
            value(QStringLiteral("spectrumGradientOpacity"), 70).toInt(), 0, 100));
        nativeWaterfall_->setLevelRange(nativeMinimumDbfs_, nativeMaximumDbfs_);
        nativeWaterfall_->setFpsOverlayEnabled(
            value(QStringLiteral("showWaterfallFps"), false).toBool());
        nativeWaterfall_->setExtendedInfoOverlayEnabled(
            value(QStringLiteral("showExtendedSpectrumInfo"), false).toBool());
        nativeWaterfall_->setAreaMeasurementEnabled(
            value(QStringLiteral("waterfallAreaMeasurementEnabled"), false).toBool());
        {
            const QSignalBlocker blocker(nativeWaterfall_);
            nativeWaterfall_->setDisplayPaused(
                value(QStringLiteral("paused"), false).toBool());
        }
        if (!wasNativeActive && nativeWaterfall3DActive_)
            nativeWaterfall_->clearData();
        syncNativeWaterfallGeometry();
    }
    void updateConnections();
    QRectF resizeHandleRect() const { return QRectF(blockWidth() - 18.0, blockHeight() - 18.0, 16.0, 16.0); }
    QRectF resizeHandleHitRect() const { return QRectF(blockWidth() - 28.0, blockHeight() - 28.0, 26.0, 26.0); }
    void updateResizeHandleOverlay() {
        if (!resizeHandleOverlay_) return;
        const QRectF handle = resizeHandleRect();
        QPainterPath path;
        path.moveTo(handle.bottomLeft());
        path.lineTo(handle.bottomRight());
        path.lineTo(handle.topRight());
        path.closeSubpath();
        path.moveTo(handle.bottomLeft() + QPointF(5.0, -2.0));
        path.lineTo(handle.topRight() + QPointF(-2.0, 5.0));
        path.moveTo(handle.bottomLeft() + QPointF(10.0, -2.0));
        path.lineTo(handle.topRight() + QPointF(-2.0, 10.0));
        resizeHandleOverlay_->setPath(path);
    }
    void resizeEmbeddedWidget() {
        const QSize framedSize(std::max(1, int(visualWidth_ - 16.0)),
                               std::max(1, int(visualHeight_ - 51.0)));
        if (miniVfoDisplay_) miniVfoDisplay_->setFixedSize(framedSize);
        if (researchDisplay_) researchDisplay_->setFixedSize(framedSize);
        if (workspaceDisplay_) {
            workspaceDisplay_->setFixedSize(std::max(1, int(visualWidth_ - 6.0)),
                                            std::max(1, int(visualHeight_ - 6.0)));
        }
        syncNativeWaterfallGeometry();
    }

    QString id_;
    QString type_;
    QString title_;
    bool customTitle_ = false;
    bool controlActive_ = false;
    QString controlLabel_;
    FineTuneScaleWidget *fineTuneWidget_ = nullptr;
    QGraphicsProxyWidget *fineTuneProxy_ = nullptr;
    MiniVfoDisplay *miniVfoDisplay_ = nullptr;
    QGraphicsProxyWidget *miniVfoProxy_ = nullptr;
    DspResearchWidget *researchDisplay_ = nullptr;
    QGraphicsProxyWidget *researchProxy_ = nullptr;
    WorkspaceSpectrumDisplay *workspaceDisplay_ = nullptr;
    QGraphicsProxyWidget *workspaceProxy_ = nullptr;
    WorkspaceDisplayCallbacks workspaceCallbacks_;
    QPointer<MyWaterfallWidget> nativeWaterfall_;
    QPointer<QGraphicsView> nativeView_;
    QJsonObject workspaceVisualizationSettings_;
    QJsonObject workspaceControllerSettings_;
    bool nativeWaterfall3DActive_ = false;
    bool nativePresentationQueued_ = false;
    bool nativeShowRequested_ = false;
    bool nativeFixedPlaneInitialized_ = false;
    bool nativeAppliedFixedPlane_ = false;
    bool nativeSecondGraph_ = false;
    bool nativeColorSpectrum_ = true;
    float nativeContrast_ = 10.0f;
    float nativeSensitivity_ = 10.0f;
    float nativeMinimumDbfs_ = -140.0f;
    float nativeMaximumDbfs_ = -30.0f;
    int nativeSourceFftLength_ = 0;
    int nativeFftWindowType_ = 0;
    QGraphicsPathItem *resizeHandleOverlay_ = nullptr;
    qreal visualWidth_ = kVfoDisplayBlockWidth;
    qreal visualHeight_ = kVfoDisplayBlockHeight;
    bool resizing_ = false;
    QPointF resizeStartScene_;
    QSizeF resizeStartSize_;
    QJsonObject settings_;
    int vfoIndex_ = -1;
    QList<DspConnectionItem*> connections_;
};

class DspConnectionItem final : public QGraphicsPathItem {
public:
    DspConnectionItem(DspBlockItem *source, DspBlockItem *target, bool ukrainian = false) : source_(source), target_(target), ukrainian_(ukrainian) {
        setZValue(-5.0);
        setFlag(ItemIsSelectable, true);
        if (source_) source_->addConnection(this);
        if (target_) target_->addConnection(this);
        updatePath();
    }
    ~DspConnectionItem() override {
        if (source_) source_->removeConnection(this);
        if (target_) target_->removeConnection(this);
    }
    DspBlockItem *source() const { return source_; }
    DspBlockItem *target() const { return target_; }
    bool isControlConnection() const { return source_ && source_->isVerticalControlSource(); }
    void updatePath() {
        if (!source_ || !target_) return;
        QPainterPath path;
        if (isControlConnection()) {
            const QPointF from = mapFromScene(source_->controlOutputScenePos(target_->sceneBoundingRect().center()));
            const QPointF to = mapFromScene(target_->controlInputScenePos(source_->sceneBoundingRect().center()));
            const qreal bend = std::max<qreal>(32.0, qAbs(to.y() - from.y()) * 0.45);
            path.moveTo(from);
            const qreal direction = to.y() < from.y() ? -bend : bend;
            path.cubicTo(from + QPointF(0.0, direction), to - QPointF(0.0, direction), to);
        } else {
            const QPointF from = mapFromScene(source_->outputScenePos());
            const QPointF to = mapFromScene(target_->inputScenePos());
            const qreal bend = std::max<qreal>(45.0, qAbs(to.x() - from.x()) * 0.45);
            path.moveTo(from);
            path.cubicTo(from + QPointF(bend, 0.0), to - QPointF(bend, 0.0), to);
        }
        setPath(path);
        const auto kind = dspLinkKind(source_->blockType(), target_->blockType());
        const QColor normal = kind == DspLinkKind::Control ? QColor(190, 122, 235) :
            kind == DspLinkKind::Settings ? QColor(78, 202, 151) :
            kind == DspLinkKind::Spectrum ? QColor(244, 184, 70) :
            kind == DspLinkKind::Documentation ? QColor(125, 133, 145) : QColor(104, 188, 255);
        if ((kind == DspLinkKind::Iq && target_->blockType() == QStringLiteral("fft")) ||
            (kind == DspLinkKind::Spectrum && source_->blockType() == QStringLiteral("fft"))) {
            setToolTip(ukrainian_
                ? QStringLiteral("Прив'язка до спільного головного FFT; джерело та тракт залишаються штатними")
                : QStringLiteral("Shared main FFT binding; uses the existing active source and pipeline"));
        } else {
            setToolTip(dspLinkDescription(kind, ukrainian_));
        }
        setPen(QPen(isSelected() ? QColor(255, 194, 72) : normal,
                    isSelected() ? 3.0 : 2.2, kind == DspLinkKind::Documentation ? Qt::DashLine : Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    }
    QPainterPath shape() const override {
        QPainterPathStroker stroker;
        stroker.setWidth(12.0);
        return stroker.createStroke(path());
    }
protected:
    QVariant itemChange(GraphicsItemChange change, const QVariant &value) override {
        const QVariant result = QGraphicsPathItem::itemChange(change, value);
        if (change == ItemSelectedHasChanged) updatePath();
        return result;
    }
private:
    DspBlockItem *source_ = nullptr;
    DspBlockItem *target_ = nullptr;
    bool ukrainian_ = false;
};

void DspBlockItem::updateConnections() {
    for (DspConnectionItem *edge : connections_) if (edge) edge->updatePath();
}

QVariant DspBlockItem::itemChange(GraphicsItemChange change, const QVariant &value) {
    const QVariant result = QGraphicsItem::itemChange(change, value);
    if (change == ItemPositionHasChanged) {
        const auto snapshot = connections_;
        for (DspConnectionItem *edge : snapshot) if (edge) edge->updatePath();
    }
    if (change == ItemPositionHasChanged ||
        change == ItemScaleHasChanged ||
        change == ItemTransformHasChanged ||
        change == ItemParentHasChanged ||
        change == ItemVisibleHasChanged ||
        change == ItemSceneHasChanged) {
        syncNativeWaterfallGeometry();
    }
    return result;
}

class DspFlowView final : public QGraphicsView {
public:
    explicit DspFlowView(QGraphicsScene *scene, QWidget *parent = nullptr) : QGraphicsView(scene, parent) {
        setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
        setViewportUpdateMode(QGraphicsView::BoundingRectViewportUpdate);
        setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
        setResizeAnchor(QGraphicsView::AnchorViewCenter);
        setDragMode(QGraphicsView::RubberBandDrag);
        setBackgroundBrush(QColor(20, 23, 28));
        setFocusPolicy(Qt::StrongFocus);
    }

    void setWorkspaceMode(bool enabled) {
        workspaceMode_ = enabled;
        resetTransform();
        setAlignment(Qt::AlignLeft | Qt::AlignTop);
        setHorizontalScrollBarPolicy(enabled ? Qt::ScrollBarAlwaysOff : Qt::ScrollBarAsNeeded);
        setVerticalScrollBarPolicy(enabled ? Qt::ScrollBarAlwaysOff : Qt::ScrollBarAsNeeded);
    }

    std::function<bool(const QPointF &, int)> workspaceWheelHandler;
    std::function<bool(const QPointF &, const QPointF &, bool)> workspacePanHandler;
    std::function<void()> nativeWidgetsGeometryChanged;

protected:
    void wheelEvent(QWheelEvent *event) override {
        for (QGraphicsItem *item = itemAt(event->position().toPoint()); item; item = item->parentItem()) {
            if (dynamic_cast<QGraphicsProxyWidget *>(item)) {
                QGraphicsView::wheelEvent(event);
                return;
            }
            auto *block = dynamic_cast<DspBlockItem*>(item);
            if (block && (block->blockType() == QStringLiteral("fine_tune_control") ||
                          block->isWorkspaceDisplayBlock())) {
                QGraphicsView::wheelEvent(event);
                return;
            }
        }
        if (workspaceMode_) {
            const int direction = event->angleDelta().y() >= 0 ? 1 : -1;
            if (workspaceWheelHandler &&
                workspaceWheelHandler(mapToScene(event->position().toPoint()), direction)) {
                event->accept();
                return;
            }
            event->accept();
            return;
        }
        const qreal factor = event->angleDelta().y() > 0 ? 1.15 : (1.0 / 1.15);
        const qreal next = transform().m11() * factor;
        if (next >= 0.22 && next <= 4.5) {
            scale(factor, factor);
            if (nativeWidgetsGeometryChanged) nativeWidgetsGeometryChanged();
        }
        event->accept();
    }

    void keyPressEvent(QKeyEvent *event) override {
        if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
            spacePressed_ = true;
            if (!panning_) setCursor(Qt::OpenHandCursor);
            event->accept();
            return;
        }
        QGraphicsView::keyPressEvent(event);
    }

    void keyReleaseEvent(QKeyEvent *event) override {
        if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
            spacePressed_ = false;
            if (!panning_) unsetCursor();
            event->accept();
            return;
        }
        QGraphicsView::keyReleaseEvent(event);
    }

    void mousePressEvent(QMouseEvent *event) override {
        const bool workspacePanGesture =
            workspaceMode_ &&
            (event->button() == Qt::MiddleButton ||
             (event->button() == Qt::LeftButton && spacePressed_));
        if (workspacePanGesture && !blockAtPosition(event->pos())) {
            panning_ = true;
            panButton_ = event->button();
            panStart_ = event->pos();
            workspacePanAnchor_ = mapToScene(event->pos());
            setCursor(Qt::ClosedHandCursor);
            event->accept();
            return;
        }
        if (!workspaceMode_ && event->button() == Qt::MiddleButton) {
            panning_ = true;
            panButton_ = event->button();
            panStart_ = event->pos();
            setCursor(Qt::ClosedHandCursor);
            event->accept();
            return;
        }
        QGraphicsView::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override {
        if (panning_) {
            if (workspaceMode_) {
                const QPointF oldScene = mapToScene(panStart_);
                const QPointF newScene = mapToScene(event->pos());
                if (workspacePanHandler)
                    workspacePanHandler(workspacePanAnchor_, newScene - oldScene, false);
            } else {
                const QPoint delta = event->pos() - panStart_;
                horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta.x());
                verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
            }
            panStart_ = event->pos();
            event->accept();
            return;
        }
        QGraphicsView::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override {
        if (panning_ && event->button() == panButton_) {
            if (workspaceMode_ && workspacePanHandler)
                workspacePanHandler(workspacePanAnchor_, QPointF(), true);
            panning_ = false;
            panButton_ = Qt::NoButton;
            if (spacePressed_) setCursor(Qt::OpenHandCursor);
            else unsetCursor();
            event->accept();
            return;
        }
        QGraphicsView::mouseReleaseEvent(event);
    }

    void showEvent(QShowEvent *event) override {
        QGraphicsView::showEvent(event);
        if (nativeWidgetsGeometryChanged) nativeWidgetsGeometryChanged();
    }
    void resizeEvent(QResizeEvent *event) override {
        QGraphicsView::resizeEvent(event);
        if (nativeWidgetsGeometryChanged) nativeWidgetsGeometryChanged();
    }

    void scrollContentsBy(int dx, int dy) override {
        QGraphicsView::scrollContentsBy(dx, dy);
        if (nativeWidgetsGeometryChanged) nativeWidgetsGeometryChanged();
    }
private:
    bool blockAtPosition(const QPoint &position) const {
        for (QGraphicsItem *item = itemAt(position); item; item = item->parentItem()) {
            if (dynamic_cast<DspBlockItem *>(item)) return true;
        }
        return false;
    }

    bool panning_ = false;
    bool workspaceMode_ = false;
    bool spacePressed_ = false;
    Qt::MouseButton panButton_ = Qt::NoButton;
    QPoint panStart_;
    QPointF workspacePanAnchor_;
};

class DockedSettingsHeader final : public QFrame {
public:
    explicit DockedSettingsHeader(const QString &title, QWidget *parent = nullptr)
        : QFrame(parent) {
        setCursor(Qt::OpenHandCursor);
        setObjectName(QStringLiteral("dockedSettingsHeader"));
        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(8, 4, 4, 4);
        layout->setSpacing(5);
        arrow_ = new QLabel(QStringLiteral("\u25be"), this);
        title_ = new QLabel(title, this);
        title_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        undock_ = new QToolButton(this);
        undock_->setText(QStringLiteral("↗"));
        undock_->setToolTip(QStringLiteral("Відкріпити / Undock"));
        close_ = new QToolButton(this);
        close_->setText(QStringLiteral("x"));
        close_->setToolTip(QStringLiteral("Remove"));
        layout->addWidget(arrow_);
        layout->addWidget(title_, 1);
        layout->addWidget(undock_);
        layout->addWidget(close_);
        connect(undock_, &QToolButton::clicked, this, [this]() {
            if (undockRequested) undockRequested();
        });
        connect(close_, &QToolButton::clicked, this, [this]() {
            if (closeRequested) closeRequested();
        });
    }

    void setExpanded(bool expanded) {
        expanded_ = expanded;
        arrow_->setText(expanded ? QStringLiteral("\u25be") : QStringLiteral("\u25b8"));
    }

    std::function<void(const QPoint &)> dragDelta;
    std::function<void()> dragFinished;
    std::function<void()> toggleRequested;
    std::function<void()> undockRequested;
    std::function<void()> closeRequested;

protected:
    void mousePressEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton) {
            lastGlobal_ = event->globalPos();
            moved_ = false;
            setCursor(Qt::ClosedHandCursor);
            event->accept();
            return;
        }
        QFrame::mousePressEvent(event);
    }
    void mouseMoveEvent(QMouseEvent *event) override {
        if (event->buttons().testFlag(Qt::LeftButton)) {
            const QPoint current = event->globalPos();
            const QPoint delta = current - lastGlobal_;
            if (!delta.isNull()) {
                if (delta.manhattanLength() > 1) moved_ = true;
                lastGlobal_ = current;
                if (dragDelta) dragDelta(delta);
            }
            event->accept();
            return;
        }
        QFrame::mouseMoveEvent(event);
    }
    void mouseReleaseEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton) {
            setCursor(Qt::OpenHandCursor);
            if (moved_) {
                if (dragFinished) dragFinished();
            } else if (toggleRequested) {
                toggleRequested();
            }
            event->accept();
            return;
        }
        QFrame::mouseReleaseEvent(event);
    }

private:
    QLabel *arrow_ = nullptr;
    QLabel *title_ = nullptr;
    QToolButton *undock_ = nullptr;
    QToolButton *close_ = nullptr;
    QPoint lastGlobal_;
    bool moved_ = false;
    bool expanded_ = true;
};

class DockedSettingsProxy final : public QGraphicsProxyWidget {
public:
    QString blockId;
    int sectorIndex = -1;
    DockedSettingsHeader *header = nullptr;
    QWidget *content = nullptr;
    bool expanded = true;
};
} // namespace

class DspFlowScene final : public QGraphicsScene {
public:
    explicit DspFlowScene(QObject *parent = nullptr) : QGraphicsScene(parent) {
        setSceneRect(-1800.0, -1200.0, 3600.0, 2400.0);
        resetSectorCuts();
        rebuildSectorItems();
    }
    ~DspFlowScene() override {
        // QGraphicsScene does not guarantee edges are deleted before their nodes.
        clearGraph(false);
    }

    void setNativeWorkspaceView(QGraphicsView *view) {
        nativeWorkspaceView_ = view;
        for (QGraphicsItem *item : items()) {
            if (auto *block = dynamic_cast<DspBlockItem *>(item))
                block->attachNativeWorkspaceView(view);
        }
        syncNativeWorkspaceWidgets();
    }

    void syncNativeWorkspaceWidgets() {
        for (QGraphicsItem *item : items()) {
            if (auto *block = dynamic_cast<DspBlockItem *>(item))
                block->syncNativeWaterfallGeometry();
        }
    }

    std::function<void()> changed;
    std::function<void(const QString&, const QPointF&)> addRequested;
    std::function<void(const QString&, const QString&)> activated;
    std::function<void(const QString&, const QString&, const QString&, const QString&)> controlTriggered;
    std::function<void(double)> fineTuneDelta;
    std::function<void(int)> workspaceScale;
    std::function<void(int, int)> workspacePan;
    std::function<void(double, const QPoint &)> workspaceTuneContext;
    std::function<void(double)> workspaceAutoTune;
    std::function<void(double)> workspaceScienceMarker;
    std::function<void(double, double)> workspaceMultiVfoSelection;
    std::function<void(double)> workspaceListeningFrequency;
    std::function<void(double)> workspaceCenterFrequency;
    std::function<void(double, double)> workspaceTuning;
    std::function<void(bool)> workspaceAnalogMeterToggled;
    std::function<void(int)> workspaceAnalogMeterStyleChanged;
    std::function<QString(const QString&)> standardTitle;
    std::function<void(const QString&)> statusChanged;
    bool ukrainian = false;

    DspBlockItem *addBlock(const QString &type, const QString &title, const QPointF &position,
                           const QString &id = QString(), bool customTitle = false, bool notify = true,
                           int requestedVfoIndex = -1) {
        int vfoIndex = requestedVfoIndex;
        if (type == QStringLiteral("vfo_channel") && vfoIndex < 0) {
            QSet<int> used;
            for (QGraphicsItem *item : items()) {
                if (auto *existing = dynamic_cast<DspBlockItem *>(item);
                    existing && existing->blockType() == QStringLiteral("vfo_channel")) {
                    used.insert(existing->vfoIndex());
                }
            }
            vfoIndex = 0;
            while (used.contains(vfoIndex)) ++vfoIndex;
        }
        QString resolvedTitle = title;
        if (type == QStringLiteral("vfo_channel") && vfoIndex >= 0 && !customTitle) {
            resolvedTitle += QStringLiteral(" %1").arg(vfoIndex + 1);
        }
        const QString resolvedId = id.isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces) : id;
        WorkspaceDisplayCallbacks workspaceCallbacks;
        workspaceCallbacks.scale = [this](int direction) { if (workspaceScale) workspaceScale(direction); };
        workspaceCallbacks.pan = [this](int deltaPixels, int widthPixels) {
            if (workspacePan) workspacePan(deltaPixels, widthPixels);
        };
        workspaceCallbacks.tuneContext = [this](double frequency, const QPoint &globalPos) {
            if (workspaceTuneContext) workspaceTuneContext(frequency, globalPos);
        };
        workspaceCallbacks.autoTune = [this](double frequency) {
            if (workspaceAutoTune) workspaceAutoTune(frequency);
        };
        workspaceCallbacks.scienceMarker = [this](double frequency) {
            if (workspaceScienceMarker) workspaceScienceMarker(frequency);
        };
        workspaceCallbacks.multiVfoSelection = [this](double lowHz, double highHz) {
            if (workspaceMultiVfoSelection) workspaceMultiVfoSelection(lowHz, highHz);
        };
        workspaceCallbacks.listeningFrequency = [this](double frequency) {
            if (workspaceListeningFrequency) workspaceListeningFrequency(frequency);
        };
        workspaceCallbacks.centerFrequency = [this](double frequency) {
            if (workspaceCenterFrequency) workspaceCenterFrequency(frequency);
        };
        workspaceCallbacks.tuning = [this](double listening, double center) {
            if (workspaceTuning) workspaceTuning(listening, center);
        };
        workspaceCallbacks.fillGroup = [this](const QString &blockId) {
            toggleWorkspaceGroup(blockById(blockId));
        };
        workspaceCallbacks.openSettings = [this](const QString &blockId) {
            if (DspBlockItem *block = blockById(blockId); block && activated)
                activated(block->blockType(), block->id());
        };
        workspaceCallbacks.pauseToggled = [this](const QString &blockId, bool paused) {
            if (DspBlockItem *block = blockById(blockId)) {
                QJsonObject settings = block->settings();
                settings.insert(QStringLiteral("paused"), paused);
                block->setSettings(settings);
                notifyChanged();
            }
        };
        workspaceCallbacks.analogMeterToggled = [this](bool enabled) {
            if (workspaceAnalogMeterToggled) workspaceAnalogMeterToggled(enabled);
        };
        workspaceCallbacks.analogMeterStyleChanged = [this](int style) {
            if (workspaceAnalogMeterStyleChanged) workspaceAnalogMeterStyleChanged(style);
        };
        workspaceCallbacks.ukrainian = [this]() { return ukrainian; };
        auto *block = new DspBlockItem(resolvedId, type, resolvedTitle, customTitle,
                                       [this](double deltaHz) { if (fineTuneDelta) fineTuneDelta(deltaHz); },
                                       vfoIndex, workspaceCallbacks);
        addItem(block);
        frameRoutesDirty_ = true;
        if (block->isWorkspaceDisplayBlock()) {
            block->setWorkspaceAnalogPeakMeterEnabled(workspaceAnalogPeakMeterEnabled_);
            block->setWorkspaceAnalogPeakMeterStyle(workspaceAnalogPeakMeterStyle_);
            block->setWorkspaceAnalogPeakMeterTarget(workspaceAnalogPeakMeterTargetHz_,
                                                     workspaceAnalogPeakMeterTargetValid_);
        }
        if (nativeWorkspaceView_) block->attachNativeWorkspaceView(nativeWorkspaceView_);
        block->setPos(position);
        if (notify) notifyChanged();
        return block;
    }

    bool addConnection(DspBlockItem *source, DspBlockItem *target, bool notify = true) {
        if (!source || !target || source == target || !source->hasOutput()) return false;
        const auto kind = dspLinkKind(source->blockType(), target->blockType());
        if (kind == DspLinkKind::Invalid) {
            setStatus(dspLinkDescription(DspLinkKind::Invalid, ukrainian));
            return false;
        }
        if (isWorkspaceDisplaySettingsType(source->blockType()) &&
            !workspaceSettingsMatchView(source->blockType(), target->blockType())) {
            setStatus(ukrainian
                ? QStringLiteral("Цей блок налаштувань можна під'єднати лише до відповідного робочого віджета")
                : QStringLiteral("This settings block can only connect to its matching workspace display"));
            return false;
        }
        const bool control = source->isVerticalControlSource();
        if (control ? !acceptsControlType(target->blockType(), source->blockType()) : !target->hasInput()) return false;
        for (QGraphicsItem *item : items()) {
            auto *edge = dynamic_cast<DspConnectionItem*>(item);
            if (edge && edge->source() == source && edge->target() == target) return false;
        }
        QList<DspConnectionItem*> oldInputs;
        for (QGraphicsItem *item : items()) {
            auto *edge = dynamic_cast<DspConnectionItem*>(item);
            if (!edge) continue;
            if (control) {
                if (edge->source() == source ||
                    (edge->isControlConnection() && edge->target() == target &&
                     edge->source()->blockType() == source->blockType())) oldInputs.append(edge);
            } else if (!edge->isControlConnection() && edge->target() == target) {
                const auto existingKind = dspLinkKind(edge->source()->blockType(), target->blockType());
                const bool workspaceView = target->blockType() == QStringLiteral("workspace_spectrum") ||
                                           target->blockType() == QStringLiteral("workspace_waterfall");
                // Widget settings and main spectrum frames occupy different input roles.
                if (workspaceView && ((kind == DspLinkKind::Settings && existingKind == DspLinkKind::Spectrum) ||
                                      (kind == DspLinkKind::Spectrum && existingKind == DspLinkKind::Settings))) continue;
                oldInputs.append(edge);
            }
        }
        for (DspConnectionItem *edge : oldInputs) delete edge;
        auto *edge = new DspConnectionItem(source, target, ukrainian);
        addItem(edge);
        frameRoutesDirty_ = true;
        updateConnectionContainer(edge);
        refreshResearchDisplays(); refreshWorkspaceDisplayBindings();
        if (notify) notifyChanged();
        return true;
    }

    void resetDefault(bool notify = true) {
        clearGraph(false);
        DspBlockItem *run = addBlock(QStringLiteral("run_control"), titleFor(QStringLiteral("run_control")),
                                     QPointF(-396.0, -125.0), QString(), false, false);
        DspBlockItem *source = addBlock(QStringLiteral("receiver_source"), titleFor(QStringLiteral("receiver_source")),
                                        QPointF(-430.0, -34.0), QString(), false, false);
        DspBlockItem *frequency = addBlock(QStringLiteral("frequency_control"), titleFor(QStringLiteral("frequency_control")),
                                           QPointF(-430.0, 78.0), QString(), false, false);
        DspBlockItem *filter = addBlock(QStringLiteral("channel_filter"), titleFor(QStringLiteral("channel_filter")),
                                        QPointF(-210.0, -34.0), QString(), false, false);
        DspBlockItem *demod = addBlock(QStringLiteral("demodulator"), titleFor(QStringLiteral("demodulator")),
                                       QPointF(10.0, -34.0), QString(), false, false);
        DspBlockItem *audio = addBlock(QStringLiteral("audio_filter"), titleFor(QStringLiteral("audio_filter")),
                                       QPointF(230.0, -34.0), QString(), false, false);
        DspBlockItem *output = addBlock(QStringLiteral("audio_output"), titleFor(QStringLiteral("audio_output")),
                                        QPointF(450.0, -34.0), QString(), false, false);
        DspBlockItem *fft = addBlock(QStringLiteral("fft"), titleFor(QStringLiteral("fft")),
                                     QPointF(-210.0, -155.0), QString(), false, false);
        addConnection(run, source, false);
        addConnection(frequency, source, false);
        addConnection(source, filter, false);
        addConnection(source, fft, false);
        addConnection(filter, demod, false);
        addConnection(demod, audio, false);
        addConnection(audio, output, false);
        if (notify) notifyChanged();
    }

    void releaseDockedSettingsPanel(DockedSettingsProxy *proxy) {
        if (!proxy) return;
        QWidget *panel = proxy->widget();
        removeItem(proxy);
        proxy->hide();
        proxy->setWidget(nullptr);
        proxy->deleteLater();
        if (panel) {
            panel->hide();
            panel->deleteLater();
        }
    }

    void clearGraph(bool notify = true) {
        frameRoutesDirty_ = true;
        workspaceFrameBlocks_.clear();
        vfoFrameBlocks_.clear();
        vfoFrameChannels_.clear();
        cancelGraphInteraction();
        const auto dockedPanels = dockedSettingsPanels_;
        dockedSettingsPanels_.clear();
        for (DockedSettingsProxy *proxy : dockedPanels) {
            if (!proxy) continue;
            releaseDockedSettingsPanel(proxy);
        }
        QList<DspConnectionItem *> edges;
        QList<DspBlockItem *> blocks;
        for (QGraphicsItem *item : items()) {
            if (auto *edge = dynamic_cast<DspConnectionItem *>(item)) edges.append(edge);
            else if (auto *block = dynamic_cast<DspBlockItem *>(item)) blocks.append(block);
        }
        for (DspConnectionItem *edge : edges) delete edge;
        for (DspBlockItem *block : blocks) delete block;
        if (notify) notifyChanged();
    }

    void deleteSelection() {
        QSet<DspConnectionItem*> edges;
        QList<DspBlockItem*> blocks;
        for (QGraphicsItem *item : selectedItems()) {
            if (auto *edge = dynamic_cast<DspConnectionItem*>(item)) {
                edges.insert(edge);
            } else if (auto *block = dynamic_cast<DspBlockItem*>(item)) {
                blocks.append(block);
                for (DspConnectionItem *edge : block->connections()) edges.insert(edge);
            }
        }
        if (edges.isEmpty() && blocks.isEmpty()) return;
        cancelGraphInteraction();
        frameRoutesDirty_ = true;
        workspaceFrameBlocks_.clear();
        vfoFrameBlocks_.clear();
        vfoFrameChannels_.clear();
        // A selected edge can also belong to several selected blocks. Delete it once,
        // while both endpoints are still alive to detach it from their connection lists.
        for (DspConnectionItem *edge : edges) delete edge;
        for (DspBlockItem *block : blocks) {
            if (DockedSettingsProxy *proxy = dockedSettingsPanels_.take(block->id())) {
                releaseDockedSettingsPanel(proxy);
            }
            delete block;
        }
        refreshResearchDisplays(); refreshWorkspaceDisplayBindings();
        notifyChanged();
    }

    void renameSelection() {
        DspBlockItem *block = selectedBlock();
        if (!block) return;
        bool ok = false;
        const QString title = QInputDialog::getText(nullptr,
                                                    ukrainian ? QStringLiteral("Назва блока") : QStringLiteral("Block name"),
                                                    ukrainian ? QStringLiteral("Назва:") : QStringLiteral("Name:"),
                                                    QLineEdit::Normal, block->title(), &ok).trimmed();
        if (ok && !title.isEmpty() && title != block->title()) {
            block->setTitle(title, true);
            notifyChanged();
        }
    }

    void editBindingId(DspBlockItem *block) {
        if (!block) return;
        bool ok = false;
        const QString current = block->settings()
                                    .value(QStringLiteral("bindingId"))
                                    .toString()
                                    .trimmed();
        QString value = QInputDialog::getText(
                            nullptr,
                            ukrainian ? QStringLiteral("Ідентифікатор прив'язки")
                                      : QStringLiteral("Binding ID"),
                            ukrainian ? QStringLiteral("Однаковий ID пов'язує блок налаштувань із віджетом:\n(порожнє поле повертає прив'язку стрілкою)")
                                      : QStringLiteral("The same ID binds a settings block to a view:\n(an empty value restores arrow binding)"),
                            QLineEdit::Normal,
                            current,
                            &ok)
                            .trimmed();
        if (!ok) return;
        if (value.size() > 24) value.truncate(24);

        QJsonObject settings = block->settings();
        if (value.isEmpty()) settings.remove(QStringLiteral("bindingId"));
        else settings.insert(QStringLiteral("bindingId"), value);
        block->setSettings(settings);
        refreshResearchDisplays(); refreshWorkspaceDisplayBindings();
        notifyChanged();
    }

    void activateSelection() {
        if (DspBlockItem *block = selectedBlock()) {
            if (block->isMiniControlBlock()) triggerControl(block);
            else if (block->blockType() == QStringLiteral("fine_tune_control"))
                setStatus(ukrainian ? QStringLiteral("Точне налаштування працює безпосередньо на шкалі")
                                    : QStringLiteral("Fine tune is controlled directly on the scale"));
            else if (activated) activated(block->blockType(), block->id());
        }
    }

    void setControlStates(bool receiverRunning, const QHash<QString, bool> &enabledByBlockType) {
        for (QGraphicsItem *item : items()) {
            auto *block = dynamic_cast<DspBlockItem*>(item);
            if (!block) continue;
            if (block->isWorkspaceDisplayBlock()) block->setWorkspaceReceiverRunning(receiverRunning);
            if (!block->isMiniControlBlock()) continue;
            bool active = false;
            if (block->blockType() == QStringLiteral("run_control")) {
                active = receiverRunning;
            } else if (DspBlockItem *target = controlTargetFor(block)) {
                active = target->blockType() == QStringLiteral("ddc_channelizer")
                    ? target->settings().value(QStringLiteral("enabled")).toBool(false)
                    : enabledByBlockType.value(target->blockType(), false);
            }
            block->setControlState(active, ukrainian);
        }
    }

    void setFineTuneRangeHz(double rangeHz) {
        for (QGraphicsItem *item : items()) {
            if (auto *block = dynamic_cast<DspBlockItem*>(item)) block->setFineTuneRangeHz(rangeHz);
        }
    }

    bool hasWorkspaceDisplayBlocks() const {
        for (QGraphicsItem *item : items()) {
            const auto *block = dynamic_cast<const DspBlockItem *>(item);
            if (block && block->blockType().startsWith(QStringLiteral("workspace_"))) return true;
        }
        return false;
    }

    void ensureWorkspaceDisplays() {
        if (hasWorkspaceDisplayBlocks()) {
            for (QGraphicsItem *item : items()) {
                auto *block = dynamic_cast<DspBlockItem *>(item);
                if (!block || !block->blockType().startsWith(QStringLiteral("workspace_"))) continue;
                QJsonObject settings = block->settings();
                settings.remove(QStringLiteral("workspaceStackRole"));
                settings.insert(QStringLiteral("workspaceAutoFit"), false);
                if (!settings.contains(QStringLiteral("workspaceAutoWidth"))) {
                    settings.insert(QStringLiteral("workspaceAutoWidth"), true);
                }
                block->setSettings(settings);
            }
            assignUnassignedBlocksToSector(0);
            relayoutSectorWidgets();
            notifyChanged();
            return;
        }

        setSectorLayout(2, 1, false);
        DspBlockItem *spectrum = addBlock(QStringLiteral("workspace_spectrum"),
                                          titleFor(QStringLiteral("workspace_spectrum")),
                                          QPointF(), QString(), false, false);
        DspBlockItem *ruler = addBlock(QStringLiteral("workspace_ruler"),
                                       titleFor(QStringLiteral("workspace_ruler")),
                                       QPointF(), QString(), false, false);
        DspBlockItem *waterfall = addBlock(QStringLiteral("workspace_waterfall"),
                                           titleFor(QStringLiteral("workspace_waterfall")),
                                           QPointF(), QString(), false, false);
        const auto placeInitialVisual = [this](DspBlockItem *block, const QRectF &target) {
            QJsonObject settings = block->settings();
            settings.insert(QStringLiteral("workspaceSector"), 1);
            settings.insert(QStringLiteral("workspaceAutoFit"), false);
            settings.insert(QStringLiteral("workspaceAutoWidth"), true);
            settings.remove(QStringLiteral("workspaceStackRole"));
            block->setSettings(settings);
            assignBlockToSector(block, 1, false);
            block->setVisualSize(target.width(), target.height());
            block->setPos(target.topLeft());
        };

        if (DspSectorItem *visualSector = sectorItem(1)) {
            const QRectF available = visualSector->rect().adjusted(12.0, 12.0, -12.0, -12.0);
            constexpr qreal gap = 7.0;
            constexpr qreal rulerHeight = 58.0;
            const qreal spectrumHeight = (std::max)(kVisualBlockMinHeight, available.height() * 0.32);
            QRectF spectrumTarget = available;
            spectrumTarget.setHeight(spectrumHeight);
            QRectF rulerTarget = available;
            rulerTarget.setTop(spectrumTarget.bottom() + gap);
            rulerTarget.setHeight(rulerHeight);
            QRectF waterfallTarget = available;
            waterfallTarget.setTop(rulerTarget.bottom() + gap);
            waterfallTarget.setHeight((std::max)(kVisualBlockMinHeight,
                                                  available.bottom() - waterfallTarget.top()));
            placeInitialVisual(spectrum, spectrumTarget);
            placeInitialVisual(ruler, rulerTarget);
            placeInitialVisual(waterfall, waterfallTarget);
        }
        assignUnassignedBlocksToSector(0);
        relayoutSectorWidgets();
        notifyChanged();
    }
    bool hasWorkspaceDisplayFrames() const {
        ensureFrameRoutes();
        return !workspaceFrameBlocks_.isEmpty();
    }
    int workspaceSpectrumTargetBins() const {
        ensureFrameRoutes();
        int width = 512;
        for (DspBlockItem *block : workspaceFrameBlocks_) {
            const int blockWidth = nativeWorkspaceView_
                ? nativeWorkspaceView_->mapFromScene(block->sceneBoundingRect()).boundingRect().width()
                : int(block->visualWidth());
            width = (std::max)(width, blockWidth);
        }
        return std::clamp(width, 512, 16384);
    }

    void updateWorkspaceSpectrum(const std::vector<float> &frequencies,
                                 const std::vector<float> &levels,
                                 double centerHz,
                                 double listeningHz,
                                 double sampleRate,
                                 double bandwidthHz,
                                 int modulationType) {
        ensureFrameRoutes();
        for (DspBlockItem *block : workspaceFrameBlocks_) {
            block->setWorkspaceSpectrum(frequencies, levels, centerHz, listeningHz, sampleRate,
                                        bandwidthHz, modulationType);
        }
    }

    void setWorkspaceAnalogPeakMeterEnabled(bool enabled) {
        workspaceAnalogPeakMeterEnabled_ = enabled;
        for (QGraphicsItem *item : items()) {
            if (auto *block = dynamic_cast<DspBlockItem *>(item); block && block->isWorkspaceDisplayBlock())
                block->setWorkspaceAnalogPeakMeterEnabled(enabled);
        }
    }
    void setWorkspaceAnalogPeakMeterStyle(int style) {
        workspaceAnalogPeakMeterStyle_ = std::clamp(style, 0, 1);
        for (QGraphicsItem *item : items()) {
            if (auto *block = dynamic_cast<DspBlockItem *>(item); block && block->isWorkspaceDisplayBlock())
                block->setWorkspaceAnalogPeakMeterStyle(workspaceAnalogPeakMeterStyle_);
        }
    }
    void setWorkspaceAnalogPeakMeterTarget(double frequencyHz, bool valid) {
        workspaceAnalogPeakMeterTargetValid_ = valid && std::isfinite(frequencyHz);
        workspaceAnalogPeakMeterTargetHz_ = workspaceAnalogPeakMeterTargetValid_ ? frequencyHz : 0.0;
        for (QGraphicsItem *item : items()) {
            if (auto *block = dynamic_cast<DspBlockItem *>(item); block && block->isWorkspaceDisplayBlock())
                block->setWorkspaceAnalogPeakMeterTarget(workspaceAnalogPeakMeterTargetHz_,
                                                         workspaceAnalogPeakMeterTargetValid_);
        }
    }

    void setWorkspaceVisualizationSettings(const QJsonObject &settings) {
        for (QGraphicsItem *item : items()) {
            if (auto *block = dynamic_cast<DspBlockItem *>(item); block && block->isWorkspaceDisplayBlock())
                block->setWorkspaceVisualizationSettings(settings);
        }
    }

    int sectorColumns() const { return sectorColumns_; }
    int sectorRows() const {
        int maximum = 1;
        for (int count : columnRowCounts_) maximum = (std::max)(maximum, count);
        return maximum;
    }
    int sectorCount() const {
        int total = 0;
        for (int count : columnRowCounts_) total += count;
        return total;
    }
    bool hasUniformSectorRows() const {
        if (columnRowCounts_.isEmpty()) return true;
        const int first = columnRowCounts_.front();
        for (int count : columnRowCounts_) if (count != first) return false;
        return true;
    }

    void setWorkspaceViewportSize(const QSize &size) {
        if (size.width() < 2 || size.height() < 2) return;
        workspaceRect_ = QRectF(0.0, 0.0, qreal(size.width()), qreal(size.height()));
        setSceneRect(workspaceRect_);
        updateSectorItemsGeometry();
        relayoutSectorWidgets();
        update();
    }

    void setSectorLayout(int columns, int rows, bool notify = true) {
        sectorColumns_ = (std::clamp)(columns, 1, 8);
        const int rowCount = (std::clamp)(rows, 1, 4);
        columnRowCounts_.fill(rowCount, sectorColumns_);
        resetSectorCuts();
        rebuildSectorItems();
        update();
        if (notify) notifyChanged();
    }

    void setSectorColumns(int columns, bool notify = true) {
        setSectorLayout(columns, 1, notify);
    }

    void prepareNewBlockForWorkspace(DspBlockItem *block, const QPointF &scenePosition) {
        if (!block) return;
        const int index = sectorIndexAt(scenePosition);
        DspSectorItem *sector = sectorItem(index);
        if (!sector) return;
        qreal inheritedScale = 1.0;
        for (QGraphicsItem *item : items()) {
            auto *existing = dynamic_cast<DspBlockItem *>(item);
            if (!existing || existing == block || existing->parentItem() != sector) continue;
            if (existing->isWorkspaceDisplayBlock() &&
                existing->settings().value(QStringLiteral("workspaceGroupLayout")).toBool(false)) continue;
            inheritedScale = std::clamp(existing->scale(), 0.35, 2.5);
            break;
        }
        QJsonObject settings = block->settings();
        settings.insert(QStringLiteral("workspaceSector"), index);
        settings.insert(QStringLiteral("workspaceAutoFit"), false);
        settings.insert(QStringLiteral("workspaceGroupLayout"), false);
        settings.remove(QStringLiteral("workspaceGroupId"));
        settings.remove(QStringLiteral("workspaceGroupOrder"));
        settings.remove(QStringLiteral("workspaceGroupCuts"));
        settings.insert(QStringLiteral("workspaceCanvasScale"), inheritedScale);
        block->setSettings(settings);
        assignBlockToSector(block, index, true);
        notifyChanged();
    }
    bool placeSelectionInSector(int index) {
        DspBlockItem *block = selectedBlock();
        if (!block || index < 0 || index >= sectorCount()) return false;
        QJsonObject settings = block->settings();
        settings.insert(QStringLiteral("workspaceSector"), index);
        settings.remove(QStringLiteral("workspaceStackRole"));
        settings.insert(QStringLiteral("workspaceAutoFit"), false);
        settings.insert(QStringLiteral("workspaceGroupLayout"), false);
        settings.remove(QStringLiteral("workspaceGroupId"));
        settings.remove(QStringLiteral("workspaceGroupOrder"));
        settings.remove(QStringLiteral("workspaceGroupCuts"));
        settings.insert(QStringLiteral("workspaceCanvasScale"), canvasScaleForSector(index, block));
        block->setSettings(settings);
        assignBlockToSector(block, index, false);
        if (block->isVisualBlock()) fitBlockToSector(block, index);
        notifyChanged();
        return true;
    }

    bool dockSettingsDialog(const QString &blockId, QDialog *dialog, int requestedSector = -1) {
        DspBlockItem *block = blockById(blockId);
        if (!block || !dialog) return false;
        if (DockedSettingsProxy *existing = dockedSettingsPanels_.value(blockId, nullptr)) {
            existing->show();
            existing->setSelected(true);
            return true;
        }
        int sectorIndex = requestedSector >= 0
            ? requestedSector
            : block->settings().value(QStringLiteral("dockedSettingsSector")).toInt(
                  block->settings().value(QStringLiteral("workspaceSector")).toInt(0));
        sectorIndex = std::clamp(sectorIndex, 0, (std::max)(0, sectorCount() - 1));
        DspSectorItem *sector = sectorItem(sectorIndex);
        if (!sector) return false;
        const QPointer<QWidget> originalParent(dialog->parentWidget());

        dialog->hide();
        dialog->setAttribute(Qt::WA_DeleteOnClose, false);
        dialog->setWindowFlag(Qt::Tool, false);
        dialog->setWindowFlag(Qt::WindowStaysOnTopHint, false);
        dialog->setWindowFlags(Qt::Widget);
        dialog->setMinimumWidth(0);
        for (QDialogButtonBox *box : dialog->findChildren<QDialogButtonBox *>()) box->hide();
        for (QCheckBox *box : dialog->findChildren<QCheckBox *>()) {
            if (box->text().contains(QStringLiteral("поверх"), Qt::CaseInsensitive) ||
                box->text().contains(QStringLiteral("stay on top"), Qt::CaseInsensitive)) box->hide();
        }

        auto *container = new QFrame();
        container->setFocusPolicy(Qt::StrongFocus);
        container->setObjectName(QStringLiteral("dockedSettingsPanel"));
        container->setStyleSheet(QStringLiteral(
            "QFrame#dockedSettingsPanel { background:#090c11; border:1px solid #29313b; }"
            "QFrame#dockedSettingsHeader { background:#111720; border:0; border-bottom:1px solid #303a46; }"
            "QFrame#dockedSettingsHeader QLabel { color:#d8e1eb; font-weight:600; }"
            "QFrame#dockedSettingsPanel QLabel, QFrame#dockedSettingsPanel QCheckBox { color:#c8d1dc; }"
            "QFrame#dockedSettingsPanel QToolButton, QFrame#dockedSettingsPanel QPushButton {"
            " background:#171e27; color:#d8e1eb; border:1px solid #354151; padding:3px 7px; }"
            "QFrame#dockedSettingsPanel QComboBox, QFrame#dockedSettingsPanel QSpinBox,"
            "QFrame#dockedSettingsPanel QDoubleSpinBox, QFrame#dockedSettingsPanel QLineEdit {"
            " background:#0e141c; color:#e1e8ef; border:1px solid #354151; }"));
        auto *layout = new QVBoxLayout(container);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        auto *header = new DockedSettingsHeader(dialog->windowTitle(), container);
        auto *content = new QWidget(container);
        auto *contentLayout = new QVBoxLayout(content);
        contentLayout->setContentsMargins(7, 6, 7, 7);
        contentLayout->setSizeConstraint(QLayout::SetMinimumSize);
        dialog->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
        dialog->ensurePolished();
        if (QLayout *dialogLayout = dialog->layout()) {
            dialogLayout->setSizeConstraint(QLayout::SetMinimumSize);
            dialogLayout->invalidate();
            dialogLayout->activate();
        }
        contentLayout->addWidget(dialog);
        layout->addWidget(header);
        layout->addWidget(content);
        dialog->show();
        dialog->adjustSize();
        dialog->setMinimumHeight((std::max)(120, dialog->sizeHint().height()));

        auto *proxy = new DockedSettingsProxy();
        proxy->blockId = blockId;
        proxy->sectorIndex = sectorIndex;
        proxy->header = header;
        proxy->content = content;
        proxy->setWidget(container);
        proxy->setFocusPolicy(Qt::StrongFocus);
        proxy->setZValue(35.0);
        proxy->setFlag(QGraphicsItem::ItemIsSelectable, true);
        addItem(proxy);
        dockedSettingsPanels_.insert(blockId, proxy);

        const auto placePanel = [this, proxy](int requestedSector, bool preserveVertical) {
            DspSectorItem *target = sectorItem(requestedSector);
            if (!target || !proxy || !proxy->widget()) return;
            const QPointF oldSceneTopLeft = proxy->scenePos();
            if (proxy->parentItem() != target) proxy->setParentItem(target);
            const QRectF available = target->rect().adjusted(10.0, 10.0, -10.0, -10.0);
            proxy->widget()->setFixedWidth((std::max)(180, int(available.width())));
            proxy->widget()->adjustSize();
            qreal y = preserveVertical ? target->mapFromScene(oldSceneTopLeft).y() : available.top();
            y = std::clamp(y, available.top(),
                           (std::max)(available.top(), available.bottom() - proxy->boundingRect().height()));
            proxy->setPos(available.left(), y);
            proxy->sectorIndex = requestedSector;
            if (DspBlockItem *owner = blockById(proxy->blockId)) {
                QJsonObject settings = owner->settings();
                settings.insert(QStringLiteral("dockedSettingsSector"), requestedSector);
                settings.insert(QStringLiteral("dockedSettingsExpanded"), proxy->expanded);
                owner->setSettings(settings);
            }
        };
        header->dragDelta = [proxy](const QPoint &delta) {
            proxy->setPos(proxy->pos() + QPointF(delta));
        };
        header->dragFinished = [this, proxy, placePanel]() {
            int target = sectorIndexAt(proxy->sceneBoundingRect().center());
            if (target < 0) target = proxy->sectorIndex;
            placePanel(target, true);
            notifyChanged();
        };
        header->toggleRequested = [this, proxy, container, content, placePanel]() {
            proxy->expanded = !proxy->expanded;
            proxy->header->setExpanded(proxy->expanded);
            content->setVisible(proxy->expanded);
            container->adjustSize();
            placePanel(proxy->sectorIndex, true);
            notifyChanged();
        };
        header->undockRequested = [this, proxy, dialog, originalParent]() {
            const QString id = proxy->blockId;
            dockedSettingsPanels_.remove(id);
            if (DspBlockItem *owner = blockById(id)) {
                QJsonObject settings = owner->settings();
                settings.remove(QStringLiteral("dockedSettingsSector"));
                settings.remove(QStringLiteral("dockedSettingsExpanded"));
                owner->setSettings(settings);
            }
            dialog->hide();
            dialog->setParent(originalParent.data(), Qt::Tool);
            dialog->setAttribute(Qt::WA_DeleteOnClose, true);
            if (QCheckBox *stay = dialog->findChild<QCheckBox *>(
                    QStringLiteral("dspStayOnTopCheckBox"))) {
                stay->show();
                dialog->setWindowFlag(Qt::WindowStaysOnTopHint, stay->isChecked());
            }
            if (QPushButton *dock = dialog->findChild<QPushButton *>(
                    QStringLiteral("dspDockToWorkspaceButton"))) dock->show();
            if (QComboBox *sectors = dialog->findChild<QComboBox *>(
                    QStringLiteral("dspDockSectorCombo"))) sectors->show();
            for (QDialogButtonBox *box : dialog->findChildren<QDialogButtonBox *>()) box->show();
            releaseDockedSettingsPanel(proxy);
            dialog->show();
            dialog->raise();
            dialog->activateWindow();
            notifyChanged();
        };
        header->closeRequested = [this, proxy, dialog]() {
            const QString id = proxy->blockId;
            dockedSettingsPanels_.remove(id);
            if (DspBlockItem *owner = blockById(id)) {
                QJsonObject settings = owner->settings();
                settings.remove(QStringLiteral("dockedSettingsSector"));
                settings.remove(QStringLiteral("dockedSettingsExpanded"));
                owner->setSettings(settings);
            }
            releaseDockedSettingsPanel(proxy);
            notifyChanged();
        };
        const bool expanded = block->settings().value(QStringLiteral("dockedSettingsExpanded")).toBool(true);
        proxy->expanded = expanded;
        header->setExpanded(expanded);
        content->setVisible(expanded);
        container->adjustSize();
        placePanel(sectorIndex, false);
        notifyChanged();
        return true;
    }
    QJsonObject blockSettings(const QString &blockId) const {
        DspBlockItem *block = blockById(blockId);
        return block ? block->settings() : QJsonObject();
    }

    QString blockTypeForId(const QString &blockId) const {
        DspBlockItem *block = blockById(blockId);
        return block ? block->blockType() : QString();
    }

    QString boundWorkspaceSettingsBlockId(const QString &viewBlockId) const {
        DspBlockItem *view = blockById(viewBlockId);
        if (!view || (view->blockType() != QStringLiteral("workspace_spectrum") &&
                      view->blockType() != QStringLiteral("workspace_waterfall")))
            return QString();
        const QString bindingId =
            view->settings().value(QStringLiteral("bindingId")).toString().trimmed();
        if (!bindingId.isEmpty()) {
            for (QGraphicsItem *item : items()) {
                auto *candidate = dynamic_cast<DspBlockItem *>(item);
                if (candidate && workspaceSettingsMatchView(candidate->blockType(), view->blockType()) &&
                    candidate->settings().value(QStringLiteral("bindingId")).toString().trimmed() == bindingId)
                    return candidate->id();
            }
        } else {
            for (QGraphicsItem *item : items()) {
                auto *edge = dynamic_cast<DspConnectionItem *>(item);
                if (edge && !edge->isControlConnection() && edge->target() == view && edge->source() &&
                    workspaceSettingsMatchView(edge->source()->blockType(), view->blockType()))
                    return edge->source()->id();
            }
        }
        return QString();
    }

    QJsonArray workspaceDisplayTargets(const QString &settingsBlockId) const {
        QJsonArray result;
        DspBlockItem *settingsBlock = blockById(settingsBlockId);
        if (!settingsBlock || !isWorkspaceDisplaySettingsType(settingsBlock->blockType())) return result;
        QList<DspBlockItem *> targets;
        for (QGraphicsItem *item : items(Qt::AscendingOrder)) {
            auto *candidate = dynamic_cast<DspBlockItem *>(item);
            if (candidate && workspaceSettingsMatchView(settingsBlock->blockType(), candidate->blockType()))
                targets.append(candidate);
        }
        std::sort(targets.begin(), targets.end(), [](const DspBlockItem *left, const DspBlockItem *right) {
            const int byTitle = QString::localeAwareCompare(left->title(), right->title());
            return byTitle == 0 ? left->id() < right->id() : byTitle < 0;
        });
        for (DspBlockItem *target : targets) {
            QJsonObject entry;
            entry.insert(QStringLiteral("id"), target->id());
            entry.insert(QStringLiteral("title"), target->title());
            entry.insert(QStringLiteral("bound"),
                         boundWorkspaceSettingsBlockId(target->id()) == settingsBlockId);
            result.append(entry);
        }
        return result;
    }

    bool bindWorkspaceSettingsBlock(const QString &settingsBlockId, const QString &viewBlockId) {
        DspBlockItem *settingsBlock = blockById(settingsBlockId);
        if (!settingsBlock || !isWorkspaceDisplaySettingsType(settingsBlock->blockType())) return false;
        DspBlockItem *target = viewBlockId.isEmpty() ? nullptr : blockById(viewBlockId);
        if (target && !workspaceSettingsMatchView(settingsBlock->blockType(), target->blockType())) return false;

        const QString previousBinding =
            settingsBlock->settings().value(QStringLiteral("bindingId")).toString().trimmed();
        QJsonObject sourceSettings = settingsBlock->settings();
        sourceSettings.remove(QStringLiteral("bindingId"));
        settingsBlock->setSettings(sourceSettings);

        QList<DspConnectionItem *> obsolete;
        for (QGraphicsItem *item : items()) {
            auto *edge = dynamic_cast<DspConnectionItem *>(item);
            if (edge && !edge->isControlConnection() && edge->source() == settingsBlock)
                obsolete.append(edge);
        }
        for (DspConnectionItem *edge : obsolete) delete edge;

        if (!previousBinding.isEmpty()) {
            for (QGraphicsItem *item : items()) {
                auto *view = dynamic_cast<DspBlockItem *>(item);
                if (!view || !workspaceSettingsMatchView(settingsBlock->blockType(), view->blockType())) continue;
                QJsonObject viewSettings = view->settings();
                if (viewSettings.value(QStringLiteral("bindingId")).toString().trimmed() == previousBinding) {
                    viewSettings.remove(QStringLiteral("bindingId"));
                    view->setSettings(viewSettings);
                }
            }
        }
        if (target) {
            QJsonObject targetSettings = target->settings();
            targetSettings.remove(QStringLiteral("bindingId"));
            target->setSettings(targetSettings);
            if (!addConnection(settingsBlock, target, false)) return false;
        }
        refreshWorkspaceDisplayBindings();
        notifyChanged();
        return true;
    }
    bool setBlockSettings(const QString &blockId, const QJsonObject &settings) {
        DspBlockItem *block = blockById(blockId);
        if (!block ||
            (!isResearchSettingsType(block->blockType()) &&
             !isWorkspaceDisplaySettingsType(block->blockType()) &&
             block->blockType() != QStringLiteral("vfo_spectrum") &&
             block->blockType() != QStringLiteral("vfo_waterfall") &&
             !dspIsBranchType(block->blockType()) &&
             !block->blockType().startsWith(QStringLiteral("workspace_")))) {
            return false;
        }
        block->setSettings(settings);
        if (isResearchSettingsType(block->blockType())) refreshResearchDisplays();
        if (isWorkspaceDisplaySettingsType(block->blockType()) ||
            block->blockType() == QStringLiteral("workspace_spectrum") ||
            block->blockType() == QStringLiteral("workspace_waterfall"))
            refreshWorkspaceDisplayBindings();
        notifyChanged();
        return true;
    }

    bool hasMultiVfoDisplayBlocks() const {
        ensureFrameRoutes();
        return !vfoFrameBlocks_.isEmpty();
    }

    DspGraphPlan visibleBranchPlan(const DspGraphPlan &plan) const {
        DspGraphPlan visible = plan;
        QSet<QString> presented;
        for (QGraphicsItem *item : items()) {
            auto *block = dynamic_cast<DspBlockItem *>(item);
            if (!block || !block->isVisualBlock() || !block->isVisible()) continue;
            for (const auto *view : views()) {
                if (view->isVisible() && !view->window()->isMinimized() &&
                    view->viewport()->rect().intersects(view->mapFromScene(block->sceneBoundingRect()).boundingRect())) {
                    presented.insert(block->id()); break;
                }
            }
        }
        QStringList liveSinks;
        for (auto &branch : visible.branches) {
            branch.iqConsumer = false;
            for (auto route = researchControllers_.cbegin(); route != researchControllers_.cend(); ++route) {
                if (presented.contains(route.key()) && plan.researchRoutes.value(route.value()) == branch.id) {
                    branch.iqConsumer = true; liveSinks.append(route.key());
                }
            }
            for (int i = branch.ffts.size()-1; i >= 0; --i) {
                bool shown = false;
                for (auto route = plan.spectrumRoutes.cbegin(); route != plan.spectrumRoutes.cend(); ++route) {
                    if (route.value() == branch.ffts[i].id && presented.contains(route.key())) {
                        shown = true; liveSinks.append(route.key());
                    }
                }
                if (!shown) branch.ffts.removeAt(i);
            }
        }
        std::sort(liveSinks.begin(), liveSinks.end());
        for (int i = visible.branches.size()-1; i >= 0; --i)
            if (!visible.branches[i].iqConsumer && visible.branches[i].ffts.isEmpty()) visible.branches.removeAt(i);
        visible.executionKey += liveSinks.join(QLatin1Char('|')).toUtf8();
        return visible;
    }

    void applyBranchFrame(const DspGraphPlan &plan, const DspBranchFrame &frame) {
        const QString info = frame.error.isEmpty()
            ? QStringLiteral("Fs %1 ksps | D %2 (%3) | gaps %4 | %5 ms")
                .arg(frame.sampleRateHz/1000.0, 0, 'f', 3).arg(frame.decimation).arg(frame.frontDecimation)
                .arg(qulonglong(frame.gaps)).arg(frame.processingMs, 0, 'f', 1)
            : (ukrainian ? QStringLiteral("Помилка параметрів/джерела DDC: %1").arg(frame.error) : frame.error);
        branchStatuses_.insert(frame.id, frame.recordingStatus.isEmpty() ? info : info+QStringLiteral(" | ")+frame.recordingStatus);
        if (auto *block = blockById(frame.id)) block->setToolTip(info);
        for (auto route = plan.spectrumRoutes.cbegin(); route != plan.spectrumRoutes.cend(); ++route) {
            auto *view = blockById(route.key());
            if (!view) continue;
            if (route.value() == frame.id) {
                if (frame.error.isEmpty()) view->setBranchSpectrum(frame,
                    QStringLiteral("%1 ksps | bin %2 Hz | RBW %3 Hz").arg(frame.sampleRateHz/1000.0, 0, 'f', 1)
                        .arg(frame.binWidthHz, 0, 'f', 2).arg(frame.rbwHz, 0, 'f', 2));
                else view->setMiniVfoStatus(info);
            } else if (!frame.error.isEmpty()) {
                for (const auto &branch : plan.branches) if (branch.id == frame.id)
                    for (const auto &fft : branch.ffts) if (fft.id == route.value()) view->setMiniVfoStatus(info);
            }
        }
        for (auto view = researchControllers_.cbegin(); view != researchControllers_.cend(); ++view) {
            if (plan.researchRoutes.value(view.value()) == frame.id)
                if (auto *block = blockById(view.key())) block->setResearchFrame(frame);
        }
    }

    QString branchStatus(const QString &id) const {
        if (auto *block = blockById(id)) {
            if (block->blockType() == QStringLiteral("ddc_channelizer") && !block->settings().value(QStringLiteral("enabled")).toBool(false))
                return ukrainian ? QStringLiteral("Гілку вимкнено") : QStringLiteral("Branch disabled");
        }
        return branchStatuses_.value(id, ukrainian ? QStringLiteral("Очікування IQ / увімкнення гілки") : QStringLiteral("Waiting for IQ / branch enable"));
    }

    void resetBranchRouteStatus(const DspGraphPlan &plan) {
        branchStatuses_.clear();
        for (QGraphicsItem *item : items()) {
            auto *block = dynamic_cast<DspBlockItem *>(item);
            if (!block || (block->blockType() != QStringLiteral("branch_spectrum") && block->blockType() != QStringLiteral("branch_waterfall"))) continue;
            const QString fftId = plan.spectrumRoutes.value(block->id());
            bool enabled = false;
            for (const auto &b : plan.branches) for (const auto &f : b.ffts) if (f.id == fftId) enabled = b.enabled;
            block->setMiniVfoStatus(fftId.isEmpty() ?
                (ukrainian ? QStringLiteral("Під'єднайте FFT гілки") : QStringLiteral("Connect branch FFT")) :
                enabled ? (ukrainian ? QStringLiteral("Очікування IQ") : QStringLiteral("Waiting for IQ")) :
                (ukrainian ? QStringLiteral("Гілку вимкнено") : QStringLiteral("Branch disabled")));
        }
    }

    void addDdcBranch(int sectorIndex = -1) {
        DspBlockItem *source = nullptr;
        for (QGraphicsItem *item : items()) {
            auto *b = dynamic_cast<DspBlockItem *>(item);
            if (b && (b->blockType() == QStringLiteral("receiver_source") || b->blockType() == QStringLiteral("iq_source"))) { source = b; break; }
        }
        const QRectF bounds = itemsBoundingRect();
        const QPointF origin(bounds.isEmpty() ? 0.0 : bounds.left(), bounds.isEmpty() ? 0.0 : bounds.bottom()+100.0);
        if (!source) source = addBlock(QStringLiteral("iq_source"), titleFor(QStringLiteral("iq_source")), origin-QPointF(220, 0));
        auto *ddc = addBlock(QStringLiteral("ddc_channelizer"), titleFor(QStringLiteral("ddc_channelizer")), origin);
        auto *fft = addBlock(QStringLiteral("branch_fft"), titleFor(QStringLiteral("branch_fft")), origin+QPointF(230, 0));
        auto *spectrum = addBlock(QStringLiteral("branch_spectrum"), titleFor(QStringLiteral("branch_spectrum")), origin+QPointF(460, 0));
        auto *waterfall = addBlock(QStringLiteral("branch_waterfall"), titleFor(QStringLiteral("branch_waterfall")), origin+QPointF(460, 230));
        if (sectorIndex >= 0 && sectorIndex < sectorCount()) {
            const qreal scale = canvasScaleForSector(sectorIndex);
            const QList<DspBlockItem *> added{ddc, fft, spectrum, waterfall};
            const QList<QPointF> positions{QPointF(10, 10), QPointF(205, 10), QPointF(10, 100), QPointF(10, 315)};
            for (int i = 0; i < added.size(); ++i) {
                auto settings = added[i]->settings(); settings.insert(QStringLiteral("workspaceSector"), sectorIndex);
                settings.insert(QStringLiteral("workspaceCanvasScale"), scale);
                added[i]->setSettings(settings); assignBlockToSector(added[i], sectorIndex, false);
                added[i]->setScale(scale); added[i]->setPos(positions[i]*scale);
            }
        }
        addConnection(source, ddc); addConnection(ddc, fft); addConnection(fft, spectrum); addConnection(fft, waterfall);
    }

    void updateMultiVfoSpectrum(const QString &configurationJson,
                                const std::vector<float> &frequencies,
                                const std::vector<float> &levels) {
        if (configurationJson != cachedVfoConfiguration_) {
            cachedVfoConfiguration_ = configurationJson;
            cachedVfoChannels_ = QJsonDocument::fromJson(configurationJson.toUtf8())
                                     .object().value(QStringLiteral("channels")).toArray();
        }
        const QJsonArray &channels = cachedVfoChannels_;
        if (channels.isEmpty()) return;
        ensureFrameRoutes();
        for (DspBlockItem *display : vfoFrameBlocks_) {
            const int channelIndex = vfoFrameChannels_.value(display, display->vfoIndex());
            if (channelIndex < 0) {
                display->setMiniVfoStatus(ukrainian
                    ? QStringLiteral("Під'єднайте блок Канал VFO")
                    : QStringLiteral("Connect a VFO channel block"));
                continue;
            }
            if (channelIndex >= channels.size()) {
                display->setMiniVfoStatus(ukrainian
                    ? QStringLiteral("Для цього номера VFO немає каналу")
                    : QStringLiteral("No channel for this VFO index"));
                continue;
            }
            const QJsonObject channel = channels.at(channelIndex).toObject();
            if (!channel.value(QStringLiteral("enabled")).toBool(true)) {
                display->setMiniVfoStatus(ukrainian
                    ? QStringLiteral("Канал VFO вимкнений")
                    : QStringLiteral("VFO channel is disabled"));
                continue;
            }
            display->setMiniVfoSpectrum(frequencies, levels,
                                        channel.value(QStringLiteral("frequencyHz")).toDouble(),
                                        channel.value(QStringLiteral("bandwidthHz")).toDouble(12500.0),
                                        channel.value(QStringLiteral("name")).toString(QStringLiteral("VFO %1").arg(channelIndex + 1)));
        }
    }

    DspBlockItem *blockById(const QString &blockId) const {
        for (QGraphicsItem *item : items()) {
            auto *block = dynamic_cast<DspBlockItem *>(item);
            if (block && block->id() == blockId) return block;
        }
        return nullptr;
    }

    DspBlockItem *vfoChannelForBlock(const QString &blockId) const {
        DspBlockItem *block = blockById(blockId);
        if (!block) return nullptr;
        if (block->blockType() == QStringLiteral("vfo_channel")) return block;
        if (block->blockType() != QStringLiteral("vfo_spectrum") &&
            block->blockType() != QStringLiteral("vfo_waterfall")) return nullptr;
        for (QGraphicsItem *item : items()) {
            auto *edge = dynamic_cast<DspConnectionItem *>(item);
            if (edge && !edge->isControlConnection() && edge->target() == block && edge->source() &&
                edge->source()->blockType() == QStringLiteral("vfo_channel")) {
                return edge->source();
            }
        }
        return nullptr;
    }

    int vfoIndexForBlock(const QString &blockId) const {
        DspBlockItem *block = blockById(blockId);
        if (!block) return -1;
        DspBlockItem *channel = vfoChannelForBlock(blockId);
        if (channel) return channel->vfoIndex();
        return block->vfoIndex();
    }

    bool assignVfoIndexToBlock(const QString &blockId, int channelIndex) {
        if (channelIndex < 0) return false;
        DspBlockItem *block = blockById(blockId);
        if (!block) return false;
        if (block->blockType() == QStringLiteral("vfo_spectrum") ||
            block->blockType() == QStringLiteral("vfo_waterfall")) {
            block->setVfoIndex(channelIndex);
            notifyChanged();
            return true;
        }
        if (block->blockType() != QStringLiteral("vfo_channel")) return false;
        block->setVfoIndex(channelIndex);
        if (!block->customTitle()) {
            QString title = standardTitle
                ? standardTitle(QStringLiteral("vfo_channel"))
                : QStringLiteral("VFO channel");
            title += QStringLiteral(" %1").arg(channelIndex + 1);
            block->setTitle(title, false);
        }
        notifyChanged();
        return true;
    }

    void addMultiVfoBranch(const QString &channelTitle,
                           const QString &spectrumTitle,
                           const QString &waterfallTitle,
                           int channelIndex) {
        const QRectF visible = itemsBoundingRect();
        const QPointF origin = visible.isEmpty() ? QPointF(0.0, 0.0)
                                                  : QPointF(visible.right() + 90.0, visible.top());
        DspBlockItem *channel = addBlock(QStringLiteral("vfo_channel"), channelTitle, origin,
                                         QString(), false, true, channelIndex);
        DspBlockItem *spectrum = addBlock(QStringLiteral("vfo_spectrum"), spectrumTitle,
                                          origin + QPointF(250.0, -70.0));
        DspBlockItem *waterfall = addBlock(QStringLiteral("vfo_waterfall"), waterfallTitle,
                                           origin + QPointF(250.0, 160.0));
        addConnection(channel, spectrum);
        addConnection(channel, waterfall);
        clearSelection();
        channel->setSelected(true);
        notifyChanged();
    }

    void refreshStandardTitles() {
        if (!standardTitle) return;
        for (QGraphicsItem *item : items()) {
            auto *block = dynamic_cast<DspBlockItem*>(item);
            if (block && !block->customTitle()) {
                QString title = standardTitle(block->blockType());
                if (block->blockType() == QStringLiteral("vfo_channel") && block->vfoIndex() >= 0) {
                    title += QStringLiteral(" %1").arg(block->vfoIndex() + 1);
                }
                block->setTitle(title, false);
            }
        }
        setControlStates(false, {});
    }

    QString configurationJson() const {
        QJsonArray blocks;
        QJsonArray connections;
        QList<DspBlockItem*> orderedBlocks;
        QList<DspConnectionItem*> orderedConnections;
        for (QGraphicsItem *item : items(Qt::AscendingOrder)) {
            if (auto *block = dynamic_cast<DspBlockItem*>(item)) orderedBlocks.append(block);
            else if (auto *edge = dynamic_cast<DspConnectionItem*>(item)) orderedConnections.append(edge);
        }
        for (DspBlockItem *block : orderedBlocks) {
            QJsonObject object;
            object.insert(QStringLiteral("id"), block->id());
            object.insert(QStringLiteral("type"), block->blockType());
            object.insert(QStringLiteral("title"), block->title());
            object.insert(QStringLiteral("customTitle"), block->customTitle());
            object.insert(QStringLiteral("x"), block->scenePos().x());
            object.insert(QStringLiteral("y"), block->scenePos().y());
            if (auto *sector = dynamic_cast<DspSectorItem *>(block->parentItem())) {
                object.insert(QStringLiteral("sector"), sector->index());
                object.insert(QStringLiteral("localX"), block->pos().x());
                object.insert(QStringLiteral("localY"), block->pos().y());
            }
            if (block->vfoIndex() >= 0) object.insert(QStringLiteral("vfoIndex"), block->vfoIndex());
            if (block->isVisualBlock()) {
                object.insert(QStringLiteral("width"), block->visualWidth());
                object.insert(QStringLiteral("height"), block->visualHeight());
            }
            if (!block->settings().isEmpty()) object.insert(QStringLiteral("settings"), block->settings());
            blocks.append(object);
        }
        for (DspConnectionItem *edge : orderedConnections) {
            if (!edge->source() || !edge->target()) continue;
            QJsonObject object;
            object.insert(QStringLiteral("source"), edge->source()->id());
            object.insert(QStringLiteral("target"), edge->target()->id());
            connections.append(object);
        }
        QJsonObject root;
        root.insert(QStringLiteral("version"), 8);
        root.insert(QStringLiteral("sectorColumns"), sectorColumns_);
        root.insert(QStringLiteral("sectorRows"), sectorRows());
        root.insert(QStringLiteral("sectorColumnCuts"), sectorCutsToJson(columnCuts_));
        QJsonArray rowCounts;
        QJsonArray rowCutsByColumn;
        for (int column = 0; column < sectorColumns_; ++column) {
            rowCounts.append(columnRowCounts_.value(column, 1));
            rowCutsByColumn.append(sectorCutsToJson(columnRowCuts_.value(column)));
        }
        root.insert(QStringLiteral("sectorColumnRows"), rowCounts);
        root.insert(QStringLiteral("sectorRowCutsByColumn"), rowCutsByColumn);
        root.insert(QStringLiteral("blocks"), blocks);
        root.insert(QStringLiteral("connections"), connections);
        return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
    }

    bool loadConfiguration(const QString &json) {
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(json.toUtf8(), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) return false;
        const QJsonObject root = document.object();
        const int version = root.value(QStringLiteral("version")).toInt(1);
        const QJsonArray blockArray = root.value(QStringLiteral("blocks")).toArray();
        const QJsonArray connectionArray = root.value(QStringLiteral("connections")).toArray();
        if (blockArray.isEmpty() || blockArray.size() > 128 || connectionArray.size() > 256) return false;

        clearGraph(false);
        setSectorLayout(root.value(QStringLiteral("sectorColumns")).toInt(1),
                        root.value(QStringLiteral("sectorRows")).toInt(1), false);
        if (version >= 6) {
            restoreIndependentSectorCuts(root.value(QStringLiteral("sectorColumnCuts")).toArray(),
                                         root.value(QStringLiteral("sectorColumnRows")).toArray(),
                                         root.value(QStringLiteral("sectorRowCutsByColumn")).toArray());
        } else if (version >= 5) {
            restoreSectorCuts(root.value(QStringLiteral("sectorColumnCuts")).toArray(),
                              root.value(QStringLiteral("sectorRowCuts")).toArray());
        }
        QHash<QString, DspBlockItem*> byId;
        for (const QJsonValue &value : blockArray) {
            const QJsonObject object = value.toObject();
            const QString id = object.value(QStringLiteral("id")).toString();
            const QString type = object.value(QStringLiteral("type")).toString();
            if (id.isEmpty() || type.isEmpty() || byId.contains(id)) { clearGraph(false); return false; }
            const qreal x = object.value(QStringLiteral("x")).toDouble();
            const qreal y = object.value(QStringLiteral("y")).toDouble();
            if (!qIsFinite(x) || !qIsFinite(y) || qAbs(x) > 100000.0 || qAbs(y) > 100000.0) {
                clearGraph(false); return false;
            }
            const bool custom = object.value(QStringLiteral("customTitle")).toBool(false);
            QString title = object.value(QStringLiteral("title")).toString().trimmed();
            if (title.isEmpty() || !custom) title = titleFor(type);
            DspBlockItem *block = addBlock(type, title, QPointF(x, y), id, custom, false,
                                           object.value(QStringLiteral("vfoIndex")).toInt(-1));
            block->setSettings(object.value(QStringLiteral("settings")).toObject());
            if (block->isVisualBlock()) {
                block->setVisualSize(object.value(QStringLiteral("width")).toDouble(kVfoDisplayBlockWidth),
                                     object.value(QStringLiteral("height")).toDouble(kVfoDisplayBlockHeight));
            }
            if (version >= 7 && object.contains(QStringLiteral("localX")) &&
                object.contains(QStringLiteral("localY"))) {
                const int sectorIndex = object.value(QStringLiteral("sector")).toInt(
                    block->settings().value(QStringLiteral("workspaceSector")).toInt(-1));
                const qreal localX = object.value(QStringLiteral("localX")).toDouble();
                const qreal localY = object.value(QStringLiteral("localY")).toDouble();
                if (sectorIndex >= 0 && sectorIndex < sectorCount() && qIsFinite(localX) && qIsFinite(localY)) {
                    QJsonObject settings = block->settings();
                    settings.insert(QStringLiteral("workspaceSector"), sectorIndex);
                    block->setSettings(settings);
                    assignBlockToSector(block, sectorIndex, false);
                    DspSectorItem *sector = sectorItem(sectorIndex);
                    block->setPos(localX, localY);

                }
            }
            byId.insert(id, block);
        }
        DspBlockItem *receiver = nullptr;
        DspBlockItem *frequencyController = nullptr;
        DspBlockItem *runController = nullptr;
        for (DspBlockItem *block : byId) {
            if (!receiver && (block->blockType() == QStringLiteral("receiver_source") ||
                              block->blockType() == QStringLiteral("iq_source") ||
                              block->blockType() == QStringLiteral("network_input") ||
                              block->blockType() == QStringLiteral("playback"))) receiver = block;
            if (!frequencyController && block->blockType() == QStringLiteral("frequency_control"))
                frequencyController = block;
            if (!runController && block->blockType() == QStringLiteral("run_control"))
                runController = block;
        }
        for (const QJsonValue &value : connectionArray) {
            const QJsonObject object = value.toObject();
            DspBlockItem *source = byId.value(object.value(QStringLiteral("source")).toString());
            DspBlockItem *target = byId.value(object.value(QStringLiteral("target")).toString());
            if (source && target && frequencyController && receiver) {
                if (source == receiver && target == frequencyController) {
                    addConnection(frequencyController, receiver, false);
                    continue;
                }
                if (source == frequencyController && target != receiver) {
                    addConnection(receiver, target, false);
                    continue;
                }
            }
            addConnection(source, target, false);
        }
        if (version < 2 && receiver && !runController) {
            runController = addBlock(QStringLiteral("run_control"), titleFor(QStringLiteral("run_control")),
                                     receiver->pos() + QPointF((kBlockWidth - kMiniBlockWidth) * 0.5, -72.0),
                                     QString(), false, false);
            addConnection(runController, receiver, false);
        }
        refreshResearchDisplays(); refreshWorkspaceDisplayBindings();
        relayoutSectorWidgets();
        if (version < 7) spreadOverlappingBlocks();
        return !byId.isEmpty();
    }

    void refreshResearchDisplayLanguage() { refreshResearchDisplays(); refreshWorkspaceDisplayBindings(); }

    bool zoomSectorContentsAt(const QPointF &scenePosition, int direction) {
        const int sectorIndex = sectorIndexAt(scenePosition);
        DspSectorItem *sector = sectorItem(sectorIndex);
        if (!sector || direction == 0) return false;
        const qreal factor = direction > 0 ? 1.15 : (1.0 / 1.15);
        const QPointF anchor = sector->mapFromScene(scenePosition);
        bool changedScale = false;
        for (QGraphicsItem *item : items()) {
            auto *block = dynamic_cast<DspBlockItem *>(item);
            if (!block || block->parentItem() != sector) continue;
            if (block->isWorkspaceDisplayBlock() &&
                block->settings().value(QStringLiteral("workspaceGroupLayout")).toBool(false)) continue;
            const qreal oldScale = block->scale();
            const qreal nextScale = std::clamp(oldScale * factor, 0.35, 2.5);
            if (qFuzzyCompare(oldScale + 1.0, nextScale + 1.0)) continue;
            const qreal ratio = nextScale / oldScale;
            block->setPos(anchor + (block->pos() - anchor) * ratio);
            QJsonObject settings = block->settings();
            settings.insert(QStringLiteral("workspaceCanvasScale"), nextScale);
            block->setSettings(settings);
            updateBlockConnections(block);
            changedScale = true;
        }
        if (changedScale) notifyChanged();
        return changedScale;
    }

    bool panSectorContentsAt(const QPointF &scenePosition, const QPointF &delta, bool finalize) {
        const int index = sectorIndexAt(scenePosition);
        DspSectorItem *sector = sectorItem(index);
        if (!sector) return false;

        QList<DspBlockItem *> movableBlocks;
        for (QGraphicsItem *item : items()) {
            auto *block = dynamic_cast<DspBlockItem *>(item);
            if (!block || block->parentItem() != sector) continue;
            if (block->isWorkspaceDisplayBlock() &&
                block->settings().value(QStringLiteral("workspaceGroupLayout")).toBool(false)) continue;
            movableBlocks.append(block);
        }
        if (movableBlocks.isEmpty()) return false;
        if (!finalize && !delta.isNull()) {
            for (DspBlockItem *block : movableBlocks) {
                block->setPos(block->pos() + delta);
                updateBlockConnections(block);
            }
            update();
        } else if (finalize) {
            notifyChanged();
        }
        return true;
    }

protected:
    void drawBackground(QPainter *painter, const QRectF &rect) override {
        QGraphicsScene::drawBackground(painter, rect);
        constexpr int minor = 20;
        constexpr int major = 100;
        QVarLengthArray<QLineF, 256> minorLines;
        QVarLengthArray<QLineF, 128> majorLines;
        const int left = int(qFloor(rect.left() / minor)) * minor;
        const int top = int(qFloor(rect.top() / minor)) * minor;
        for (int x = left; x < rect.right(); x += minor) {
            if (x % major == 0) majorLines.append(QLineF(x, rect.top(), x, rect.bottom()));
            else minorLines.append(QLineF(x, rect.top(), x, rect.bottom()));
        }
        for (int y = top; y < rect.bottom(); y += minor) {
            if (y % major == 0) majorLines.append(QLineF(rect.left(), y, rect.right(), y));
            else minorLines.append(QLineF(rect.left(), y, rect.right(), y));
        }
        painter->setPen(QPen(QColor(32, 37, 44), 1.0));
        painter->drawLines(minorLines.constData(), minorLines.size());
        painter->setPen(QPen(QColor(43, 49, 58), 1.0));
        painter->drawLines(majorLines.constData(), majorLines.size());
    }
    void mousePressEvent(QGraphicsSceneMouseEvent *event) override {
        if (event->modifiers().testFlag(Qt::ControlModifier)) {
            for (QGraphicsItem *item = itemAt(event->scenePos(), QTransform()); item; item = item->parentItem()) {
                auto *block = dynamic_cast<DspBlockItem *>(item);
                if (block && block->researchCameraAt(event->scenePos())) {
                    draggedBlock_ = nullptr;
                    dragSelection_.clear();
                    QGraphicsScene::mousePressEvent(event);
                    return;
                }
            }
        }
        if (event->button() == Qt::LeftButton && beginWorkspaceGroupDividerDrag(event->scenePos())) {
            clearSelection();
            event->accept();
            return;
        }
        if (event->button() == Qt::LeftButton) {
            if (DspBlockItem *resizeBlock = resizeBlockAt(event->scenePos())) {
                resizingBlock_ = resizeBlock;
                draggedBlock_ = nullptr;
                clearSelection();
                resizeBlock->setSelected(true);
                resizeBlock->beginSceneResize(event->scenePos());
                event->accept();
                return;
            }
        }
        if (event->button() == Qt::LeftButton && beginSectorDividerDrag(event->scenePos())) {
            clearSelection();
            event->accept();
            return;
        }
        if (event->button() == Qt::LeftButton) {
            draggedBlock_ = blockAtBody(event->scenePos());
            if (draggedBlock_ && draggedBlock_->isWorkspaceGroupLocked()) draggedBlock_ = nullptr;
            dragSelection_.clear();
            if (draggedBlock_ && draggedBlock_->isSelected()) {
                for (QGraphicsItem *selected : selectedItems()) {
                    if (auto *selectedBlock = dynamic_cast<DspBlockItem *>(selected))
                        dragSelection_.append(selectedBlock);
                }
            }
            if (draggedBlock_ && !dragSelection_.contains(draggedBlock_))
                dragSelection_.append(draggedBlock_);
            blockPressPosition_ = event->scenePos();
            if (DspBlockItem *source = blockAtOutput(event->scenePos())) {
                pendingSource_ = source;
                temporaryConnection_ = addPath(QPainterPath(),
                                               QPen(QColor(255, 172, 72), 2.0, Qt::DashLine));
                temporaryConnection_->setZValue(60.0);
                updateTemporaryConnection(event->scenePos());
                setStatus(ukrainian ? QStringLiteral("Оберіть вхід блока")
                                    : QStringLiteral("Select a block input"));
                event->accept();
                return;
            }
            if (auto *block = dynamic_cast<DspBlockItem*>(itemAt(event->scenePos(), QTransform()));
                block && block->isMiniControlBlock()) {
                pressedControl_ = block;
                controlPressPosition_ = event->scenePos();
            }
        }
        QGraphicsScene::mousePressEvent(event);
    }

    void mouseMoveEvent(QGraphicsSceneMouseEvent *event) override {
        if (workspaceGroupResizeBoundary_ >= 0) {
            dragWorkspaceGroupDivider(event->scenePos());
            event->accept();
            return;
        }
        if (resizingBlock_) {
            resizingBlock_->continueSceneResize(event->scenePos());
            event->accept();
            return;
        }
        if (dividerAxis_ != 0) {
            dragSectorDivider(event->scenePos());
            event->accept();
            return;
        }
        if (pendingSource_) {
            updateTemporaryConnection(event->scenePos());
            event->accept();
            return;
        }
        QGraphicsScene::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QGraphicsSceneMouseEvent *event) override {
        if (workspaceGroupResizeBoundary_ >= 0) {
            draggedBlock_ = nullptr;
            finishWorkspaceGroupDividerDrag();
            notifyChanged();
            event->accept();
            return;
        }
        if (resizingBlock_) {
            resizingBlock_->continueSceneResize(event->scenePos());
            resizingBlock_->finishSceneResize();
            resizingBlock_ = nullptr;
            notifyChanged();
            event->accept();
            return;
        }
        if (dividerAxis_ != 0) {
            draggedBlock_ = nullptr;
            finishSectorDividerDrag();
            notifyChanged();
            event->accept();
            return;
        }
        if (pendingSource_) {
            DspBlockItem *target = blockAtInput(event->scenePos(), pendingSource_);
            DspBlockItem *source = pendingSource_;
            draggedBlock_ = nullptr;
            cancelPendingConnection();
            if (target && addConnection(source, target))
                setStatus(ukrainian ? QStringLiteral("З'єднання створено") : QStringLiteral("Connection created"));
            else
                setStatus(ukrainian ? QStringLiteral("Готово") : QStringLiteral("Ready"));
            event->accept();
            return;
        }
        QGraphicsScene::mouseReleaseEvent(event);
        if (event->button() == Qt::LeftButton && draggedBlock_) {
            DspBlockItem *block = draggedBlock_;
            draggedBlock_ = nullptr;
            if (QLineF(blockPressPosition_, event->scenePos()).length() >= 8.0) {
                const int targetSector = sectorIndexAt(event->scenePos());
                const bool moved = dragSelection_.size() > 1
                                       ? moveBlocksToSector(dragSelection_, targetSector)
                                       : moveBlockToSector(block, targetSector, event->scenePos());
                if (targetSector >= 0 && moved) {
                    setStatus(ukrainian ? QStringLiteral("Виділену групу перенесено до іншого сектора")
                                        : QStringLiteral("Selected group moved to another sector"));
                }
            }
            dragSelection_.clear();
        }
        if (event->button() == Qt::LeftButton && pressedControl_) {
            DspBlockItem *block = pressedControl_;
            pressedControl_ = nullptr;
            if (QLineF(controlPressPosition_, event->scenePos()).length() < 5.0)
                triggerControl(block);
        }
        notifyChanged();
    }

    void mouseDoubleClickEvent(QGraphicsSceneMouseEvent *event) override {
        if (auto *block = blockAtBody(event->scenePos())) {
            if (event->modifiers().testFlag(Qt::ControlModifier) && block->researchCameraAt(event->scenePos())) {
                QGraphicsScene::mouseDoubleClickEvent(event);
                return;
            }
            if (block->isMiniControlBlock()) {
                event->accept();
                return;
            }
            clearSelection();
            block->setSelected(true);
            QGraphicsScene::mouseDoubleClickEvent(event);
            activateSelection();
            return;
        }
        QGraphicsScene::mouseDoubleClickEvent(event);
    }

    void keyPressEvent(QKeyEvent *event) override {
        if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
            deleteSelection();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Escape) {
            cancelPendingConnection();
            setStatus(ukrainian ? QStringLiteral("Готово") : QStringLiteral("Ready"));
            event->accept();
            return;
        }
        QGraphicsScene::keyPressEvent(event);
    }

    void contextMenuEvent(QGraphicsSceneContextMenuEvent *event) override {
        QGraphicsItem *item = itemAt(event->scenePos(), QTransform());
        if (auto *block = blockAtBody(event->scenePos())) {
            clearSelection();
            block->setSelected(true);
            QMenu menu;
            QAction *open = menu.addAction(ukrainian ? QStringLiteral("Відкрити налаштування")
                                                      : QStringLiteral("Open settings"));
            open->setEnabled(block->blockType() != QStringLiteral("fine_tune_control"));
            QAction *fillGroup = nullptr;
            if (block->isWorkspaceDisplayBlock()) {
                fillGroup = menu.addAction(block->isWorkspaceGroupLocked()
                    ? (ukrainian ? QStringLiteral("Відкріпити групу") : QStringLiteral("Release group"))
                    : (ukrainian ? QStringLiteral("Зафіксувати групу") : QStringLiteral("Lock group")));
            }
            QAction *binding = menu.addAction(ukrainian ? QStringLiteral("Призначити ID прив'язки...")
                                                        : QStringLiteral("Assign binding ID..."));
            binding->setEnabled(isResearchSettingsType(block->blockType()) ||
                                isResearchViewType(block->blockType()) ||
                                isWorkspaceDisplaySettingsType(block->blockType()) ||
                                block->blockType() == QStringLiteral("workspace_spectrum") ||
                                block->blockType() == QStringLiteral("workspace_waterfall"));
            binding->setToolTip(ukrainian
                ? QStringLiteral("Для пар параметри/графік; VFO-віджети обирають канал у власних налаштуваннях")
                : QStringLiteral("For settings/view pairs; VFO views select their channel in their own settings"));
            menu.addSeparator();
            QAction *rename = menu.addAction(ukrainian ? QStringLiteral("Перейменувати") : QStringLiteral("Rename"));
            QAction *remove = menu.addAction(ukrainian ? QStringLiteral("Видалити") : QStringLiteral("Delete"));
            QAction *chosen = menu.exec(event->screenPos());
            if (chosen == open) activateSelection();
            else if (fillGroup && chosen == fillGroup) toggleWorkspaceGroup(block);
            else if (chosen == binding) editBindingId(block);
            else if (chosen == rename) renameSelection();
            else if (chosen == remove) deleteSelection();
            return;
        }
        if (auto *edge = dynamic_cast<DspConnectionItem*>(item)) {
            clearSelection();
            edge->setSelected(true);
            QMenu menu;
            QAction *remove = menu.addAction(ukrainian ? QStringLiteral("Видалити з'єднання")
                                                        : QStringLiteral("Delete connection"));
            if (menu.exec(event->screenPos()) == remove) deleteSelection();
            return;
        }

        QMenu menu;
        const int clickedSector = sectorIndexAt(event->scenePos());
        int clickedColumn = -1;
        int clickedRow = -1;
        sectorCoordinates(clickedSector, &clickedColumn, &clickedRow);
        QHash<QAction *, int> rowCountActions;
        QAction *addColumnRight = nullptr;
        QAction *removeSector = nullptr;
        QAction *removeColumn = nullptr;
        QAction *moveColumnLeft = nullptr;
        QAction *moveColumnRight = nullptr;
        if (clickedColumn >= 0) {
            QMenu *split = menu.addMenu(
                ukrainian ? QStringLiteral("Поділити цей стовпець")
                          : QStringLiteral("Split this column"));
            const int currentRows = columnRowCounts_.value(clickedColumn, 1);
            for (int rows = 1; rows <= 4; ++rows) {
                QAction *action = split->addAction(
                    ukrainian ? QStringLiteral("%1 секц.").arg(rows)
                              : QStringLiteral("%1 section(s)").arg(rows));
                action->setCheckable(true);
                action->setChecked(rows == currentRows);
                rowCountActions.insert(action, rows);
            }
            menu.addSeparator();
            addColumnRight = menu.addAction(ukrainian ? QStringLiteral("Додати стовпець праворуч")
                                                       : QStringLiteral("Add column to the right"));
            addColumnRight->setEnabled(sectorColumns_ < 8);
            removeSector = menu.addAction(ukrainian ? QStringLiteral("Видалити цей сектор")
                                                     : QStringLiteral("Remove this sector"));
            removeSector->setEnabled(currentRows > 1 && clickedRow >= 0);
            removeColumn = menu.addAction(ukrainian ? QStringLiteral("Видалити цей стовпець")
                                                     : QStringLiteral("Remove this column"));
            removeColumn->setEnabled(sectorColumns_ > 1);
            moveColumnLeft = menu.addAction(ukrainian ? QStringLiteral("Перемістити стовпець ліворуч")
                                                       : QStringLiteral("Move column left"));
            moveColumnLeft->setEnabled(clickedColumn > 0);
            moveColumnRight = menu.addAction(ukrainian ? QStringLiteral("Перемістити стовпець праворуч")
                                                        : QStringLiteral("Move column right"));
            moveColumnRight->setEnabled(clickedColumn + 1 < sectorColumns_);
            menu.addSeparator();
        }
        QMenu *add = menu.addMenu(ukrainian ? QStringLiteral("Додати блок") : QStringLiteral("Add block"));
        const QStringList categories = {QStringLiteral("signal_path"), QStringLiteral("filters"),
                                        QStringLiteral("scanning"), QStringLiteral("visualization"),
                                        QStringLiteral("measurement"), QStringLiteral("digital"),
                                        QStringLiteral("navigation"), QStringLiteral("io_tools")};
        QHash<QString, QMenu*> categoryMenus;
        for (const QString &category : categories)
            categoryMenus.insert(category, add->addMenu(categoryTitle(category, ukrainian)));
        for (const QString &type : allBlockTypes()) {
            QAction *action = categoryMenus.value(blockCategory(type), add)->addAction(titleFor(type));
            action->setData(type);
        }
        QAction *chosen = menu.exec(event->screenPos());
        if (rowCountActions.contains(chosen)) {
            setColumnRowCount(clickedColumn, rowCountActions.value(chosen));
        } else if (chosen == addColumnRight) {
            insertWorkspaceColumn(clickedColumn + 1);
        } else if (chosen == removeSector) {
            removeWorkspaceRow(clickedColumn, clickedRow);
        } else if (chosen == removeColumn) {
            removeWorkspaceColumn(clickedColumn);
        } else if (chosen == moveColumnLeft) {
            swapWorkspaceColumns(clickedColumn, clickedColumn - 1);
        } else if (chosen == moveColumnRight) {
            swapWorkspaceColumns(clickedColumn, clickedColumn + 1);
        } else if (chosen && addRequested && !chosen->data().toString().isEmpty()) {
            addRequested(chosen->data().toString(), event->scenePos());
        }
    }

private:
    DspSectorItem *sectorItem(int index) const {
        return index >= 0 && index < sectorItems_.size() ? sectorItems_[index] : nullptr;
    }

    DspBlockItem *blockAtBody(const QPointF &position) const {
        for (QGraphicsItem *item = itemAt(position, QTransform()); item; item = item->parentItem()) {
            if (auto *block = dynamic_cast<DspBlockItem *>(item)) return block;
        }
        return nullptr;
    }

    DspBlockItem *resizeBlockAt(const QPointF &position) const {
        const QList<QGraphicsItem *> hitItems = items(position, Qt::IntersectsItemShape,
                                                       Qt::DescendingOrder, QTransform());
        for (QGraphicsItem *item : hitItems) {
            for (QGraphicsItem *candidate = item; candidate; candidate = candidate->parentItem()) {
                auto *block = dynamic_cast<DspBlockItem *>(candidate);
                if (block && !block->isWorkspaceGroupLocked() &&
                    block->resizeHandleContainsScenePoint(position)) return block;
            }
        }
        return nullptr;
    }

    qreal canvasScaleForSector(int index, const DspBlockItem *ignore = nullptr) const {
        DspSectorItem *sector = sectorItem(index);
        if (!sector) return 1.0;
        for (QGraphicsItem *item : items()) {
            auto *existing = dynamic_cast<DspBlockItem *>(item);
            if (!existing || existing == ignore || existing->parentItem() != sector) continue;
            if (existing->isWorkspaceDisplayBlock() &&
                existing->settings().value(QStringLiteral("workspaceGroupLayout")).toBool(false)) continue;
            return std::clamp(existing->scale(), 0.35, 2.5);
        }
        return 1.0;
    }
    bool moveBlocksToSector(const QList<DspBlockItem *> &blocks, int index) {
        DspSectorItem *sector = sectorItem(index);
        if (!sector || blocks.isEmpty()) return false;
        for (DspBlockItem *block : blocks)
            if (block && block->isWorkspaceGroupLocked()) return false;
        bool needsMove = false;
        QSet<QString> selectedIds;
        for (DspBlockItem *block : blocks) {
            if (!block || block->scene() != this) continue;
            selectedIds.insert(block->id());
            if (block->parentItem() != sector) needsMove = true;
        }
        if (!needsMove) return false;

        QSet<QString> completeGroupIds;
        for (DspBlockItem *block : blocks) {
            if (!block) continue;
            const QString groupId = block->settings().value(QStringLiteral("workspaceGroupId")).toString();
            if (groupId.isEmpty() || completeGroupIds.contains(groupId)) continue;
            bool complete = true;
            for (QGraphicsItem *item : items()) {
                auto *member = dynamic_cast<DspBlockItem *>(item);
                if (!member || member->settings().value(QStringLiteral("workspaceGroupId")).toString() != groupId)
                    continue;
                if (!selectedIds.contains(member->id())) { complete = false; break; }
            }
            if (complete) completeGroupIds.insert(groupId);
        }

        bool preservedWorkspaceGroup = false;
        for (DspBlockItem *block : blocks) {
            if (!block || block->scene() != this) continue;
            QJsonObject settings = block->settings();
            const QString groupId = settings.value(QStringLiteral("workspaceGroupId")).toString();
            const bool preserveGroup = !groupId.isEmpty() && completeGroupIds.contains(groupId);
            settings.insert(QStringLiteral("workspaceSector"), index);
            settings.insert(QStringLiteral("workspaceAutoFit"), false);
            if (!preserveGroup) {
                settings.insert(QStringLiteral("workspaceGroupLayout"), false);
                settings.remove(QStringLiteral("workspaceGroupId"));
                settings.remove(QStringLiteral("workspaceGroupOrder"));
                settings.remove(QStringLiteral("workspaceGroupCuts"));
                settings.insert(QStringLiteral("workspaceCanvasScale"), canvasScaleForSector(index, block));
            } else {
                preservedWorkspaceGroup = true;
            }
            block->setSettings(settings);
            assignBlockToSector(block, index, true);
        }
        if (preservedWorkspaceGroup) layoutWorkspaceGroup(index);
        for (DspBlockItem *block : blocks) updateBlockConnections(block);
        notifyChanged();
        return true;
    }
    bool moveBlockToSector(DspBlockItem *block, int index, const QPointF &dropPosition) {
        DspSectorItem *sector = sectorItem(index);
        if (!block || block->isWorkspaceGroupLocked() || !sector || block->parentItem() == sector) return false;
        const QRectF oldBounds = block->sceneBoundingRect().adjusted(-6.0, -6.0, 6.0, 6.0);
        QJsonObject settings = block->settings();
        settings.insert(QStringLiteral("workspaceSector"), index);
        settings.remove(QStringLiteral("workspaceStackRole"));
        settings.insert(QStringLiteral("workspaceAutoFit"), false);
        settings.insert(QStringLiteral("workspaceGroupLayout"), false);
        settings.remove(QStringLiteral("workspaceGroupId"));
        settings.remove(QStringLiteral("workspaceGroupOrder"));
        settings.remove(QStringLiteral("workspaceGroupCuts"));
        settings.insert(QStringLiteral("workspaceCanvasScale"), canvasScaleForSector(index, block));
        block->setSettings(settings);
        assignBlockToSector(block, index, false);
        if (block->isVisualBlock()) {
            const QRectF available = sector->rect().adjusted(12.0, 12.0, -12.0, -12.0);
            block->setVisualSize((std::min)(block->visualWidth(), available.width()),
                                 (std::min)(block->visualHeight(), available.height()));
        }
        const QPointF local = sector->mapFromScene(dropPosition) -
                              QPointF(block->blockWidth() * 0.5, block->blockHeight() * 0.5);
        block->setPos(local);
        updateBlockConnections(block);
        const QRectF newBounds = block->sceneBoundingRect().adjusted(-6.0, -6.0, 6.0, 6.0);
        invalidate(oldBounds, QGraphicsScene::AllLayers);
        invalidate(newBounds, QGraphicsScene::AllLayers);
        update(oldBounds.united(newBounds));
        for (QGraphicsView *view : views()) view->viewport()->update();
        return true;
    }

    struct WorkspaceColumnBlockLocation {
        DspBlockItem *block = nullptr;
        int column = -1;
        int row = -1;
        QPointF localPosition;
    };

    QVector<WorkspaceColumnBlockLocation> workspaceColumnLocations() const {
        QVector<WorkspaceColumnBlockLocation> locations;
        for (QGraphicsItem *item : items()) {
            auto *block = dynamic_cast<DspBlockItem *>(item);
            if (!block) continue;
            int column = -1;
            int row = -1;
            const int sectorIndex = block->settings().value(QStringLiteral("workspaceSector")).toInt(-1);
            if (!sectorCoordinates(sectorIndex, &column, &row)) continue;
            locations.append({block, column, row, block->pos()});
        }
        return locations;
    }

    void restoreWorkspaceColumnLocations(const QVector<WorkspaceColumnBlockLocation> &locations,
                                         const std::function<int(int)> &mapColumn) {
        for (const WorkspaceColumnBlockLocation &location : locations) {
            if (!location.block || location.block->scene() != this) continue;
            const int mappedColumn = mapColumn(location.column);
            const int mappedRow = std::clamp(location.row, 0,
                                             columnRowCounts_.value(mappedColumn, 1) - 1);
            const int targetIndex = sectorIndexForColumnRow(mappedColumn, mappedRow);
            if (targetIndex < 0) continue;
            QJsonObject settings = location.block->settings();
            settings.insert(QStringLiteral("workspaceSector"), targetIndex);
            if (settings.contains(QStringLiteral("dockedSettingsSector")))
                settings.insert(QStringLiteral("dockedSettingsSector"), targetIndex);
            location.block->setSettings(settings);
        }
        rebuildSectorItems();
        for (const WorkspaceColumnBlockLocation &location : locations) {
            if (!location.block || location.block->scene() != this) continue;
            const int mappedColumn = mapColumn(location.column);
            const int mappedRow = std::clamp(location.row, 0,
                                             columnRowCounts_.value(mappedColumn, 1) - 1);
            const int targetIndex = sectorIndexForColumnRow(mappedColumn, mappedRow);
            DspSectorItem *target = sectorItem(targetIndex);
            if (!target) continue;
            assignBlockToSector(location.block, targetIndex, false);
            if (!location.block->settings().value(QStringLiteral("workspaceGroupLayout")).toBool(false))
                location.block->setPos(location.localPosition);
            updateBlockConnections(location.block);
        }
        for (int index = 0; index < sectorCount(); ++index) layoutWorkspaceGroup(index);
        notifyChanged();
    }

    bool insertWorkspaceColumn(int insertIndex) {
        if (sectorColumns_ >= 8) return false;
        insertIndex = std::clamp(insertIndex, 0, sectorColumns_);
        const QVector<WorkspaceColumnBlockLocation> locations = workspaceColumnLocations();
        ++sectorColumns_;
        columnRowCounts_.insert(insertIndex, 1);
        columnRowCuts_.insert(insertIndex, QVector<qreal>());
        columnCuts_.clear();
        for (int index = 1; index < sectorColumns_; ++index)
            columnCuts_.append(qreal(index) / qreal(sectorColumns_));
        restoreWorkspaceColumnLocations(locations, [insertIndex](int oldColumn) {
            return oldColumn >= insertIndex ? oldColumn + 1 : oldColumn;
        });
        return true;
    }

    bool removeWorkspaceColumn(int column) {
        if (sectorColumns_ <= 1 || column < 0 || column >= sectorColumns_) return false;
        const QVector<WorkspaceColumnBlockLocation> locations = workspaceColumnLocations();
        --sectorColumns_;
        columnRowCounts_.removeAt(column);
        columnRowCuts_.removeAt(column);
        columnCuts_.clear();
        for (int index = 1; index < sectorColumns_; ++index)
            columnCuts_.append(qreal(index) / qreal(sectorColumns_));
        restoreWorkspaceColumnLocations(locations, [this, column](int oldColumn) {
            if (oldColumn < column) return oldColumn;
            if (oldColumn == column) return (std::min)(column, sectorColumns_ - 1);
            return oldColumn - 1;
        });
        return true;
    }

    bool removeWorkspaceRow(int column, int row) {
        if (column < 0 || column >= sectorColumns_) return false;
        const int oldRows = columnRowCounts_.value(column, 1);
        if (oldRows <= 1 || row < 0 || row >= oldRows) return false;
        const QVector<WorkspaceColumnBlockLocation> locations = workspaceColumnLocations();
        const int newRows = oldRows - 1;
        columnRowCounts_[column] = newRows;
        columnRowCuts_[column].clear();
        for (int index = 1; index < newRows; ++index)
            columnRowCuts_[column].append(qreal(index) / qreal(newRows));

        for (const WorkspaceColumnBlockLocation &location : locations) {
            if (!location.block || location.block->scene() != this) continue;
            int mappedRow = location.row;
            if (location.column == column) {
                if (mappedRow > row) --mappedRow;
                else if (mappedRow == row) mappedRow = (std::min)(row, newRows - 1);
            }
            mappedRow = std::clamp(mappedRow, 0,
                                   columnRowCounts_.value(location.column, 1) - 1);
            const int targetIndex = sectorIndexForColumnRow(location.column, mappedRow);
            if (targetIndex < 0) continue;
            QJsonObject settings = location.block->settings();
            settings.insert(QStringLiteral("workspaceSector"), targetIndex);
            if (settings.contains(QStringLiteral("dockedSettingsSector")))
                settings.insert(QStringLiteral("dockedSettingsSector"), targetIndex);
            location.block->setSettings(settings);
        }
        rebuildSectorItems();
        for (const WorkspaceColumnBlockLocation &location : locations) {
            if (!location.block || location.block->scene() != this) continue;
            const int targetIndex = location.block->settings()
                                        .value(QStringLiteral("workspaceSector")).toInt(0);
            assignBlockToSector(location.block, targetIndex, false);
            if (!location.block->settings().value(QStringLiteral("workspaceGroupLayout")).toBool(false))
                location.block->setPos(location.localPosition);
            updateBlockConnections(location.block);
        }
        relayoutSectorWidgets();
        notifyChanged();
        return true;
    }
    bool swapWorkspaceColumns(int first, int second) {
        if (first < 0 || second < 0 || first >= sectorColumns_ || second >= sectorColumns_ ||
            first == second) return false;
        const QVector<WorkspaceColumnBlockLocation> locations = workspaceColumnLocations();
        std::swap(columnRowCounts_[first], columnRowCounts_[second]);
        std::swap(columnRowCuts_[first], columnRowCuts_[second]);
        restoreWorkspaceColumnLocations(locations, [first, second](int oldColumn) {
            if (oldColumn == first) return second;
            if (oldColumn == second) return first;
            return oldColumn;
        });
        return true;
    }
    bool setColumnRowCount(int column, int requestedRows) {
        if (column < 0 || column >= sectorColumns_) return false;
        const int rows = (std::clamp)(requestedRows, 1, 4);
        if (columnRowCounts_.value(column, 1) == rows) return false;

        struct BlockLocation {
            DspBlockItem *block = nullptr;
            QPointF center;
            int column = -1;
        };
        QVector<BlockLocation> locations;
        for (QGraphicsItem *item : items()) {
            if (auto *block = dynamic_cast<DspBlockItem *>(item)) {
                const QPointF center = block->sceneBoundingRect().center();
                locations.push_back({block, center, columnAtPosition(center)});
            }
        }

        columnRowCounts_[column] = rows;
        columnRowCuts_[column].clear();
        for (int index = 1; index < rows; ++index)
            columnRowCuts_[column].append(qreal(index) / qreal(rows));
        rebuildSectorItems();

        for (const BlockLocation &location : locations) {
            DspBlockItem *block = location.block;
            if (!block) continue;
            int targetIndex = sectorIndexAt(location.center);
            if (targetIndex < 0) targetIndex = 0;
            QJsonObject settings = block->settings();
            settings.insert(QStringLiteral("workspaceSector"), targetIndex);
            if (location.column == column) {
                settings.remove(QStringLiteral("workspaceStackRole"));
                settings.insert(QStringLiteral("workspaceAutoFit"), false);
            }
            block->setSettings(settings);
            assignBlockToSector(block, targetIndex, false);
            if (DspSectorItem *target = sectorItem(targetIndex)) {
                if (block->isVisualBlock()) {
                    const QRectF available = target->rect().adjusted(12.0, 12.0, -12.0, -12.0);
                    block->setVisualSize((std::min)(block->visualWidth(), available.width()),
                                         (std::min)(block->visualHeight(), available.height()));
                }
                block->setPos(target->mapFromScene(location.center) -
                              QPointF(block->blockWidth() * 0.5, block->blockHeight() * 0.5));
            }
            updateBlockConnections(block);
        }
        notifyChanged();
        return true;
    }

    void updateConnectionContainer(DspConnectionItem *edge) {
        if (!edge || !edge->source() || !edge->target()) return;
        auto *sourceSector = dynamic_cast<DspSectorItem *>(edge->source()->parentItem());
        auto *targetSector = dynamic_cast<DspSectorItem *>(edge->target()->parentItem());
        DspSectorItem *commonSector = sourceSector && sourceSector == targetSector ? sourceSector : nullptr;
        if (edge->parentItem() != commonSector) {
            edge->setParentItem(commonSector);
            edge->setPos(0.0, 0.0);
        }
        edge->setZValue(commonSector ? 2.0 : 50.0);
        edge->updatePath();
    }

    void updateBlockConnections(DspBlockItem *block) {
        if (!block) return;
        for (DspConnectionItem *edge : block->connections()) updateConnectionContainer(edge);
    }

    void spreadOverlappingBlocks() {
        for (int sectorIndex = 0; sectorIndex < sectorCount(); ++sectorIndex) {
            DspSectorItem *sector = sectorItem(sectorIndex);
            if (!sector) continue;
            const QRectF available = sector->rect().adjusted(12.0, 12.0, -12.0, -12.0);
            QSet<QString> occupied;
            for (QGraphicsItem *item : items(Qt::AscendingOrder)) {
                auto *block = dynamic_cast<DspBlockItem *>(item);
                if (!block || block->parentItem() != sector ||
                    block->settings().value(QStringLiteral("workspaceGroupLayout")).toBool(false)) continue;
                const auto keyFor = [](const QPointF &point) {
                    return QStringLiteral("%1:%2").arg(qRound(point.x() / 4.0)).arg(qRound(point.y() / 4.0));
                };
                QString key = keyFor(block->pos());
                if (!occupied.contains(key)) { occupied.insert(key); continue; }
                const qreal stepX = (std::max)(96.0, block->blockWidth() * block->scale() + 16.0);
                const qreal stepY = (std::max)(64.0, block->blockHeight() * block->scale() + 16.0);
                const int columns = (std::max)(1, int(available.width() / stepX));
                for (int slot = 0; slot < 256; ++slot) {
                    QPointF candidate(available.left() + (slot % columns) * stepX,
                                      available.top() + (slot / columns) * stepY);
                    if (candidate.y() + block->blockHeight() * block->scale() > available.bottom()) break;
                    key = keyFor(candidate);
                    if (occupied.contains(key)) continue;
                    block->setPos(candidate);
                    occupied.insert(key);
                    break;
                }
            }
        }
    }

    void assignBlockToSector(DspBlockItem *block, int index, bool preserveScenePosition = true) {
        DspSectorItem *sector = sectorItem(index);
        if (!block || !sector) return;
        const QPointF oldScenePosition = block->scenePos();
        if (block->parentItem() != sector) block->setParentItem(sector);
        QPointF localPosition = preserveScenePosition
                                    ? sector->mapFromScene(oldScenePosition)
                                    : QPointF(54.0, 42.0);
        const QRectF available = sector->rect().adjusted(10.0, 10.0, -10.0, -10.0);
        const qreal maximumX = (std::max)(available.left(), available.right() - block->blockWidth());
        const qreal maximumY = (std::max)(available.top(), available.bottom() - block->blockHeight());
        localPosition.setX(std::clamp(localPosition.x(), available.left(), maximumX));
        localPosition.setY(std::clamp(localPosition.y(), available.top(), maximumY));
        block->setPos(localPosition);
        updateBlockConnections(block);
    }

    void updateSectorItemsGeometry() {
        for (int index = 0; index < sectorItems_.size(); ++index) {
            if (sectorItems_[index]) sectorItems_[index]->setSectorRect(sectorRect(index));
        }
    }

    void rebuildSectorItems() {
        const QList<QGraphicsItem *> snapshot = items();
        for (QGraphicsItem *item : snapshot) {
            auto *edge = dynamic_cast<DspConnectionItem *>(item);
            if (edge && dynamic_cast<DspSectorItem *>(edge->parentItem())) {
                edge->setParentItem(nullptr);
                edge->setPos(0.0, 0.0);
            }
        }
        for (QGraphicsItem *item : snapshot) {
            auto *block = dynamic_cast<DspBlockItem *>(item);
            if (block && dynamic_cast<DspSectorItem *>(block->parentItem())) {
                const QPointF scenePosition = block->scenePos();
                block->setParentItem(nullptr);
                block->setPos(scenePosition);
            }
        }
        for (DockedSettingsProxy *proxy : dockedSettingsPanels_) {
            if (!proxy || !dynamic_cast<DspSectorItem *>(proxy->parentItem())) continue;
            const QPointF scenePosition = proxy->scenePos();
            proxy->setParentItem(nullptr);
            proxy->setPos(scenePosition);
        }
        for (DspSectorItem *sector : sectorItems_) delete sector;
        sectorItems_.clear();
        sectorItems_.reserve(sectorCount());
        for (int index = 0; index < sectorCount(); ++index) {
            auto *sector = new DspSectorItem(index);
            addItem(sector);
            sectorItems_.append(sector);
        }
        updateSectorItemsGeometry();
        relayoutSectorWidgets();
        for (QGraphicsItem *item : items()) {
            if (auto *edge = dynamic_cast<DspConnectionItem *>(item)) updateConnectionContainer(edge);
        }
    }

    void assignUnassignedBlocksToSector(int index) {
        DspSectorItem *sector = sectorItem(index);
        if (!sector) return;
        QList<DspBlockItem *> blocks;
        QRectF bounds;
        for (QGraphicsItem *item : items()) {
            auto *block = dynamic_cast<DspBlockItem *>(item);
            if (!block || block->settings().value(QStringLiteral("workspaceSector")).toInt(-1) >= 0) continue;
            blocks.append(block);
            bounds = bounds.isNull() ? block->sceneBoundingRect() : bounds.united(block->sceneBoundingRect());
        }
        if (blocks.isEmpty()) return;
        const QRectF target = sectorRect(index).adjusted(55.0, 42.0, -18.0, -18.0);
        const qreal scaleX = bounds.width() > 1.0 ? target.width() / bounds.width() : 1.0;
        const qreal scaleY = bounds.height() > 1.0 ? target.height() / bounds.height() : 1.0;
        const qreal positionScale = (std::min)(1.0, (std::min)(scaleX, scaleY));
        for (DspBlockItem *block : blocks) {
            const QPointF relative = block->scenePos() - bounds.topLeft();
            const QPointF desiredScene = target.topLeft() + relative * positionScale;
            QJsonObject settings = block->settings();
            settings.insert(QStringLiteral("workspaceSector"), index);
            block->setSettings(settings);
            block->setParentItem(sector);
            block->setPos(sector->mapFromScene(desiredScene));
        }
        for (DspBlockItem *block : blocks) updateBlockConnections(block);
    }

    void resetSectorCuts() {
        columnCuts_.clear();
        for (int index = 1; index < sectorColumns_; ++index)
            columnCuts_.append(qreal(index) / qreal(sectorColumns_));
        if (columnRowCounts_.size() != sectorColumns_) columnRowCounts_.fill(1, sectorColumns_);
        columnRowCuts_.clear();
        columnRowCuts_.resize(sectorColumns_);
        for (int column = 0; column < sectorColumns_; ++column) {
            const int rows = (std::clamp)(columnRowCounts_.value(column, 1), 1, 4);
            columnRowCounts_[column] = rows;
            for (int index = 1; index < rows; ++index)
                columnRowCuts_[column].append(qreal(index) / qreal(rows));
        }
    }
    QJsonArray sectorCutsToJson(const QVector<qreal> &cuts) const {
        QJsonArray values;
        for (qreal cut : cuts) values.append(double(cut));
        return values;
    }
    bool validatedSectorCuts(const QJsonArray &source, int expected, QVector<qreal> *target) const {
        if (!target || source.size() != expected) return false;
        QVector<qreal> parsed;
        qreal previous = 0.0;
        for (const QJsonValue &value : source) {
            const qreal cut = value.toDouble(-1.0);
            if (!qIsFinite(cut) || cut <= previous + 0.04 || cut >= 0.96) return false;
            parsed.append(cut);
            previous = cut;
        }
        *target = parsed;
        return true;
    }
    bool restoreSectorCuts(const QJsonArray &columns, const QJsonArray &rows) {
        QVector<qreal> columnsParsed;
        QVector<qreal> rowsParsed;
        const int uniformRows = sectorRows();
        if (!validatedSectorCuts(columns, sectorColumns_ - 1, &columnsParsed) ||
            !validatedSectorCuts(rows, uniformRows - 1, &rowsParsed)) return false;
        columnCuts_ = columnsParsed;
        columnRowCounts_.fill(uniformRows, sectorColumns_);
        columnRowCuts_.fill(rowsParsed, sectorColumns_);
        updateSectorItemsGeometry();
        relayoutSectorWidgets();
        update();
        return true;
    }
    bool restoreIndependentSectorCuts(const QJsonArray &columns,
                                      const QJsonArray &rowCounts,
                                      const QJsonArray &rowsByColumn) {
        QVector<qreal> columnsParsed;
        if (!validatedSectorCuts(columns, sectorColumns_ - 1, &columnsParsed) ||
            rowCounts.size() != sectorColumns_ || rowsByColumn.size() != sectorColumns_) return false;
        QVector<int> parsedCounts;
        QVector<QVector<qreal>> parsedCuts;
        parsedCounts.reserve(sectorColumns_);
        parsedCuts.reserve(sectorColumns_);
        for (int column = 0; column < sectorColumns_; ++column) {
            const int count = rowCounts.at(column).toInt(0);
            QVector<qreal> cuts;
            if (count < 1 || count > 4 ||
                !validatedSectorCuts(rowsByColumn.at(column).toArray(), count - 1, &cuts)) return false;
            parsedCounts.append(count);
            parsedCuts.append(cuts);
        }
        columnCuts_ = columnsParsed;
        columnRowCounts_ = parsedCounts;
        columnRowCuts_ = parsedCuts;
        rebuildSectorItems();
        update();
        return true;
    }
    QList<DspBlockItem *> workspaceGroupBlocks(int sectorIndex) const {
        QList<DspBlockItem *> blocks;
        for (QGraphicsItem *item : items()) {
            auto *block = dynamic_cast<DspBlockItem *>(item);
            if (!block || !block->isWorkspaceDisplayBlock()) continue;
            const QJsonObject settings = block->settings();
            if (!settings.value(QStringLiteral("workspaceGroupLayout")).toBool(false) ||
                settings.value(QStringLiteral("workspaceSector")).toInt(-1) != sectorIndex) continue;
            blocks.append(block);
        }
        std::sort(blocks.begin(), blocks.end(), [](const DspBlockItem *left, const DspBlockItem *right) {
            const QJsonObject leftSettings = left->settings();
            const QJsonObject rightSettings = right->settings();
            const bool leftHasOrder = leftSettings.contains(QStringLiteral("workspaceGroupOrder"));
            const bool rightHasOrder = rightSettings.contains(QStringLiteral("workspaceGroupOrder"));
            if (leftHasOrder && rightHasOrder) {
                const int leftOrder = leftSettings.value(QStringLiteral("workspaceGroupOrder")).toInt();
                const int rightOrder = rightSettings.value(QStringLiteral("workspaceGroupOrder")).toInt();
                if (leftOrder != rightOrder) return leftOrder < rightOrder;
            } else if (leftHasOrder != rightHasOrder) {
                return leftHasOrder;
            }
            const qreal leftY = left->sceneBoundingRect().center().y();
            const qreal rightY = right->sceneBoundingRect().center().y();
            if (!qFuzzyCompare(leftY + 1.0, rightY + 1.0)) return leftY < rightY;
            return left->id() < right->id();
        });
        return blocks;
    }

    QVector<qreal> defaultWorkspaceGroupCuts(const QList<DspBlockItem *> &blocks) const {
        QVector<qreal> cuts;
        if (blocks.size() < 2) return cuts;
        QVector<qreal> weights;
        qreal total = 0.0;
        for (const DspBlockItem *block : blocks) {
            const qreal weight = block->blockType() == QStringLiteral("workspace_ruler")
                                     ? 0.38
                                     : (block->blockType() == QStringLiteral("workspace_waterfall") ? 1.35 : 1.0);
            weights.append(weight);
            total += weight;
        }
        qreal accumulated = 0.0;
        for (int index = 0; index + 1 < weights.size(); ++index) {
            accumulated += weights[index];
            cuts.append(accumulated / total);
        }
        return cuts;
    }

    QVector<qreal> workspaceGroupCuts(const QList<DspBlockItem *> &blocks) const {
        if (blocks.size() < 2) return {};
        const QJsonArray stored = blocks.front()->settings()
                                      .value(QStringLiteral("workspaceGroupCuts"))
                                      .toArray();
        QVector<qreal> cuts;
        qreal previous = 0.0;
        for (const QJsonValue &value : stored) {
            const qreal cut = value.toDouble(-1.0);
            if (cut <= previous || cut >= 1.0) return defaultWorkspaceGroupCuts(blocks);
            cuts.append(cut);
            previous = cut;
        }
        if (cuts.size() != blocks.size() - 1) return defaultWorkspaceGroupCuts(blocks);
        return cuts;
    }

    void applyWorkspaceGroupCuts(const QList<DspBlockItem *> &blocks,
                                 const QVector<qreal> &cuts) {
        if (blocks.isEmpty()) return;
        QString groupId = blocks.front()->settings()
                              .value(QStringLiteral("workspaceGroupId"))
                              .toString();
        if (groupId.isEmpty()) groupId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QJsonArray encodedCuts;
        for (qreal cut : cuts) encodedCuts.append(cut);
        for (int index = 0; index < blocks.size(); ++index) {
            DspBlockItem *block = blocks[index];
            QJsonObject settings = block->settings();
            settings.insert(QStringLiteral("workspaceGroupLayout"), true);
            settings.insert(QStringLiteral("workspaceGroupId"), groupId);
            settings.insert(QStringLiteral("workspaceGroupOrder"), index);
            settings.insert(QStringLiteral("workspaceGroupCuts"), encodedCuts);
            settings.remove(QStringLiteral("workspaceGroupCut1"));
            settings.remove(QStringLiteral("workspaceGroupCut2"));
            settings.insert(QStringLiteral("workspaceAutoFit"), false);
            settings.insert(QStringLiteral("workspaceAutoWidth"), true);
            block->setSettings(settings);
        }
    }

    void layoutWorkspaceGroup(int sectorIndex) {
        const QList<DspBlockItem *> blocks = workspaceGroupBlocks(sectorIndex);
        DspSectorItem *sector = sectorItem(sectorIndex);
        if (blocks.isEmpty() || !sector) return;
        const QRectF available = sector->rect().adjusted(12.0, 12.0, -12.0, -12.0);
        if (available.width() < 1.0 || available.height() < 1.0) return;
        QVector<qreal> cuts = workspaceGroupCuts(blocks);
        applyWorkspaceGroupCuts(blocks, cuts);

        constexpr qreal gap = 7.0;
        qreal previousCut = 0.0;
        for (int index = 0; index < blocks.size(); ++index) {
            const qreal nextCut = index < cuts.size() ? cuts[index] : 1.0;
            const qreal top = available.top() + available.height() * previousCut +
                              (index > 0 ? gap * 0.5 : 0.0);
            const qreal bottom = available.top() + available.height() * nextCut -
                                 (index + 1 < blocks.size() ? gap * 0.5 : 0.0);
            DspBlockItem *block = blocks[index];
            if (block->parentItem() != sector) assignBlockToSector(block, sectorIndex, false);
            block->setVisualSize(available.width(), (std::max)(24.0, bottom - top));
            block->setPos(available.left(), top);
            previousCut = nextCut;
        }
    }

    void releaseWorkspaceGroup(DspBlockItem *anchor) {
        if (!anchor || !anchor->isWorkspaceDisplayBlock()) return;
        const QString groupId = anchor->settings().value(QStringLiteral("workspaceGroupId")).toString();
        QList<DspBlockItem *> members;
        for (QGraphicsItem *item : items()) {
            auto *candidate = dynamic_cast<DspBlockItem *>(item);
            if (!candidate || !candidate->isWorkspaceDisplayBlock()) continue;
            if ((groupId.isEmpty() && candidate == anchor) ||
                (!groupId.isEmpty() && candidate->settings().value(QStringLiteral("workspaceGroupId")).toString() == groupId))
                members.append(candidate);
        }
        for (DspBlockItem *member : members) {
            QJsonObject settings = member->settings();
            settings.insert(QStringLiteral("workspaceGroupLayout"), false);
            settings.remove(QStringLiteral("workspaceGroupLocked"));
            settings.remove(QStringLiteral("workspaceGroupId"));
            settings.remove(QStringLiteral("workspaceGroupOrder"));
            settings.remove(QStringLiteral("workspaceGroupCuts"));
            settings.insert(QStringLiteral("workspaceAutoFit"), false);
            settings.insert(QStringLiteral("workspaceAutoWidth"), false);
            member->setSettings(settings);
            updateBlockConnections(member);
        }
        notifyChanged();
    }

    void toggleWorkspaceGroup(DspBlockItem *anchor) {
        if (!anchor || !anchor->isWorkspaceDisplayBlock()) return;
        if (anchor->isWorkspaceGroupLocked()) releaseWorkspaceGroup(anchor);
        else fillWorkspaceGroup(anchor);
    }
    void fillWorkspaceGroup(DspBlockItem *anchor) {
        if (!anchor || !anchor->isWorkspaceDisplayBlock()) return;
        int sectorIndex = anchor->settings().value(QStringLiteral("workspaceSector")).toInt(-1);
        if (sectorIndex < 0) sectorIndex = sectorIndexAt(anchor->sceneBoundingRect().center());
        DspSectorItem *sector = sectorItem(sectorIndex);
        if (!sector) return;

        QList<DspBlockItem *> blocks;
        for (QGraphicsItem *item : items()) {
            auto *candidate = dynamic_cast<DspBlockItem *>(item);
            if (!candidate || !candidate->isWorkspaceDisplayBlock()) continue;
            const int candidateSector = candidate->settings()
                                            .value(QStringLiteral("workspaceSector"))
                                            .toInt(-1);
            if (candidate->parentItem() == sector && candidateSector == sectorIndex)
                blocks.append(candidate);
        }
        if (blocks.isEmpty()) return;
        std::sort(blocks.begin(), blocks.end(), [](const DspBlockItem *left, const DspBlockItem *right) {
            const qreal leftY = left->sceneBoundingRect().center().y();
            const qreal rightY = right->sceneBoundingRect().center().y();
            if (!qFuzzyCompare(leftY + 1.0, rightY + 1.0)) return leftY < rightY;
            const auto priority = [](const QString &type) {
                if (type == QStringLiteral("workspace_spectrum")) return 0;
                if (type == QStringLiteral("workspace_ruler")) return 1;
                return 2;
            };
            const int leftPriority = priority(left->blockType());
            const int rightPriority = priority(right->blockType());
            return leftPriority != rightPriority ? leftPriority < rightPriority : left->id() < right->id();
        });
        const QString groupId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        for (int index = 0; index < blocks.size(); ++index) {
            QJsonObject settings = blocks[index]->settings();
            settings.insert(QStringLiteral("workspaceSector"), sectorIndex);
            settings.insert(QStringLiteral("workspaceGroupLayout"), true);
            settings.insert(QStringLiteral("workspaceGroupLocked"), true);
            settings.insert(QStringLiteral("workspaceGroupId"), groupId);
            settings.insert(QStringLiteral("workspaceGroupOrder"), index);
            settings.insert(QStringLiteral("workspaceAutoFit"), false);
            settings.insert(QStringLiteral("workspaceAutoWidth"), true);
            blocks[index]->setSettings(settings);
        }
        applyWorkspaceGroupCuts(blocks, defaultWorkspaceGroupCuts(blocks));
        layoutWorkspaceGroup(sectorIndex);
        notifyChanged();
    }
    bool beginWorkspaceGroupDividerDrag(const QPointF &position) {
        constexpr qreal tolerance = 8.0;
        qreal nearest = tolerance + 1.0;
        workspaceGroupResizeSector_ = -1;
        workspaceGroupResizeBoundary_ = -1;
        for (int sectorIndex = 0; sectorIndex < sectorCount(); ++sectorIndex) {
            const QList<DspBlockItem *> blocks = workspaceGroupBlocks(sectorIndex);
            DspSectorItem *sector = sectorItem(sectorIndex);
            if (blocks.size() < 2 || !sector) continue;
            const QRectF sceneAvailable = sector->mapRectToScene(
                sector->rect().adjusted(12.0, 12.0, -12.0, -12.0));
            if (position.x() < sceneAvailable.left() || position.x() > sceneAvailable.right()) continue;
            const QVector<qreal> cuts = workspaceGroupCuts(blocks);
            for (int boundary = 0; boundary < cuts.size(); ++boundary) {
                const qreal y = sceneAvailable.top() + sceneAvailable.height() * cuts[boundary];
                const qreal distance = qAbs(position.y() - y);
                if (distance < nearest) {
                    nearest = distance;
                    workspaceGroupResizeSector_ = sectorIndex;
                    workspaceGroupResizeBoundary_ = boundary;
                }
            }
        }
        if (workspaceGroupResizeBoundary_ < 0) return false;
        for (QGraphicsView *view : views()) view->viewport()->setCursor(Qt::SplitVCursor);
        return true;
    }

    void dragWorkspaceGroupDivider(const QPointF &position) {
        const QList<DspBlockItem *> blocks = workspaceGroupBlocks(workspaceGroupResizeSector_);
        DspSectorItem *sector = sectorItem(workspaceGroupResizeSector_);
        if (blocks.size() < 2 || !sector ||
            workspaceGroupResizeBoundary_ >= blocks.size() - 1) return;
        const QRectF available = sector->mapRectToScene(
            sector->rect().adjusted(12.0, 12.0, -12.0, -12.0));
        if (available.height() <= 1.0) return;
        QVector<qreal> cuts = workspaceGroupCuts(blocks);
        const qreal minimumSpan = (std::min)(0.12, 32.0 / available.height());
        const int boundary = workspaceGroupResizeBoundary_;
        const qreal lower = boundary == 0 ? minimumSpan : cuts[boundary - 1] + minimumSpan;
        const qreal upper = boundary + 1 >= cuts.size()
                                ? 1.0 - minimumSpan
                                : cuts[boundary + 1] - minimumSpan;
        if (lower >= upper) return;
        const qreal raw = (position.y() - available.top()) / available.height();
        cuts[boundary] = std::clamp(raw, lower, upper);
        applyWorkspaceGroupCuts(blocks, cuts);
        layoutWorkspaceGroup(workspaceGroupResizeSector_);
        update();
    }
    void finishWorkspaceGroupDividerDrag() {
        workspaceGroupResizeSector_ = -1;
        workspaceGroupResizeBoundary_ = -1;
        for (QGraphicsView *view : views()) view->viewport()->unsetCursor();
    }

    void fitBlockToSector(DspBlockItem *block, int index) {
        DspSectorItem *sector = sectorItem(index);
        if (!block || !block->isVisualBlock() || !sector) return;
        if (block->parentItem() != sector) assignBlockToSector(block, index, false);
        QJsonObject settings = block->settings();
        settings.insert(QStringLiteral("workspaceSector"), index);
        settings.insert(QStringLiteral("workspaceAutoFit"), false);
        settings.insert(QStringLiteral("workspaceAutoWidth"),
                        block->blockType().startsWith(QStringLiteral("workspace_")));
        settings.insert(QStringLiteral("workspaceGroupLayout"), false);
        settings.remove(QStringLiteral("workspaceGroupId"));
        settings.remove(QStringLiteral("workspaceGroupOrder"));
        settings.remove(QStringLiteral("workspaceGroupCuts"));
        settings.remove(QStringLiteral("workspaceStackRole"));
        block->setSettings(settings);
        const QRectF target = sector->rect().adjusted(12.0, 12.0, -12.0, -12.0);
        block->setVisualSize(target.width(), target.height());
        block->setPos(target.topLeft());
    }
    void relayoutSectorWidgets() {
        const QList<QGraphicsItem *> snapshot = items();
        for (QGraphicsItem *item : snapshot) {
            auto *block = dynamic_cast<DspBlockItem *>(item);
            if (!block) continue;
            int index = block->settings().value(QStringLiteral("workspaceSector")).toInt(-1);
            if (index < 0) continue;
            if (index >= sectorCount()) {
                index = sectorCount() - 1;
                QJsonObject settings = block->settings();
                settings.insert(QStringLiteral("workspaceSector"), index);
                settings.insert(QStringLiteral("workspaceAutoFit"), false);
                settings.remove(QStringLiteral("workspaceStackRole"));
                block->setSettings(settings);
            }
            DspSectorItem *targetSector = sectorItem(index);
            if (block->parentItem() != targetSector) assignBlockToSector(block, index, true);
            const QJsonObject blockSettings = block->settings();
            if (block->isWorkspaceDisplayBlock() &&
                blockSettings.value(QStringLiteral("workspaceGroupLayout")).toBool(false)) {
                continue;
            }
            if (block->isVisualBlock() && targetSector) {
                const QRectF available = targetSector->rect().adjusted(12.0, 12.0, -12.0, -12.0);
                const bool autoWidth = block->blockType().startsWith(QStringLiteral("workspace_")) &&
                                       (!blockSettings.contains(QStringLiteral("workspaceAutoWidth")) ||
                                        blockSettings.value(QStringLiteral("workspaceAutoWidth")).toBool());
                const qreal width = autoWidth
                                        ? available.width()
                                        : (std::min)(block->visualWidth(), available.width());
                block->setVisualSize(width, (std::min)(block->visualHeight(), available.height()));
                QPointF position = block->pos();
                if (autoWidth) position.setX(available.left());
                block->setPos(position);
            } else if (targetSector) {
                block->setPos(block->pos());
            }
        }
        for (int index = 0; index < sectorCount(); ++index) layoutWorkspaceGroup(index);
        for (DockedSettingsProxy *proxy : dockedSettingsPanels_) {
            if (!proxy || !proxy->widget()) continue;
            if (DspBlockItem *owner = blockById(proxy->blockId)) {
                proxy->sectorIndex = owner->settings()
                                         .value(QStringLiteral("dockedSettingsSector"))
                                         .toInt(proxy->sectorIndex);
            }
            DspSectorItem *sector = sectorItem(proxy->sectorIndex);
            if (!sector) continue;
            if (proxy->parentItem() != sector) proxy->setParentItem(sector);
            const QRectF available = sector->rect().adjusted(10.0, 10.0, -10.0, -10.0);
            proxy->widget()->setFixedWidth((std::max)(180, int(available.width())));
            proxy->widget()->adjustSize();
            const qreal y = std::clamp(proxy->pos().y(), available.top(),
                (std::max)(available.top(), available.bottom() - proxy->boundingRect().height()));
            proxy->setPos(available.left(), y);
        }
        for (QGraphicsItem *item : items()) {
            if (auto *edge = dynamic_cast<DspConnectionItem *>(item)) updateConnectionContainer(edge);
        }
        syncNativeWorkspaceWidgets();
    }
    int columnAtPosition(const QPointF &position) const {
        if (!workspaceRect_.contains(position)) return -1;
        const qreal normalized = (position.x() - workspaceRect_.left()) / workspaceRect_.width();
        for (int column = 0; column < sectorColumns_; ++column) {
            const qreal right = column + 1 == sectorColumns_ ? 1.0 : columnCuts_.value(column, 1.0);
            if (normalized <= right || column + 1 == sectorColumns_) return column;
        }
        return -1;
    }
    bool sectorCoordinates(int index, int *columnOut, int *rowOut) const {
        int current = 0;
        const int maximumRows = sectorRows();
        for (int row = 0; row < maximumRows; ++row) {
            for (int column = 0; column < sectorColumns_; ++column) {
                if (row >= columnRowCounts_.value(column, 1)) continue;
                if (current++ == index) {
                    if (columnOut) *columnOut = column;
                    if (rowOut) *rowOut = row;
                    return true;
                }
            }
        }
        return false;
    }
    int sectorIndexForColumnRow(int wantedColumn, int wantedRow) const {
        int current = 0;
        const int maximumRows = sectorRows();
        for (int row = 0; row < maximumRows; ++row) {
            for (int column = 0; column < sectorColumns_; ++column) {
                if (row >= columnRowCounts_.value(column, 1)) continue;
                if (column == wantedColumn && row == wantedRow) return current;
                ++current;
            }
        }
        return -1;
    }
    int sectorIndexAt(const QPointF &position) const {
        const int column = columnAtPosition(position);
        if (column < 0) return -1;
        const qreal normalizedY = (position.y() - workspaceRect_.top()) / workspaceRect_.height();
        const QVector<qreal> cuts = columnRowCuts_.value(column);
        int row = 0;
        while (row < cuts.size() && normalizedY > cuts[row]) ++row;
        return sectorIndexForColumnRow(column, row);
    }
    bool beginSectorDividerDrag(const QPointF &position) {
        if (!workspaceRect_.adjusted(-10.0, -10.0, 10.0, 10.0).contains(position)) return false;
        constexpr qreal tolerance = 10.0;
        qreal bestDistance = tolerance + 1.0;
        dividerAxis_ = 0;
        dividerIndex_ = -1;
        dividerColumn_ = -1;
        for (int index = 0; index < columnCuts_.size(); ++index) {
            const qreal x = workspaceRect_.left() + workspaceRect_.width() * columnCuts_[index];
            const qreal distance = qAbs(position.x() - x);
            if (distance <= bestDistance) {
                bestDistance = distance;
                dividerAxis_ = 1;
                dividerIndex_ = index;
            }
        }
        const int column = columnAtPosition(position);
        if (column >= 0) {
            const QVector<qreal> &rowCuts = columnRowCuts_[column];
            for (int index = 0; index < rowCuts.size(); ++index) {
                const qreal y = workspaceRect_.top() + workspaceRect_.height() * rowCuts[index];
                const qreal distance = qAbs(position.y() - y);
                if (distance < bestDistance) {
                    bestDistance = distance;
                    dividerAxis_ = 2;
                    dividerIndex_ = index;
                    dividerColumn_ = column;
                }
            }
        }
        if (dividerAxis_ == 0) return false;
        for (QGraphicsView *view : views())
            view->viewport()->setCursor(dividerAxis_ == 1 ? Qt::SplitHCursor : Qt::SplitVCursor);
        return true;
    }
    void dragSectorDivider(const QPointF &position) {
        constexpr qreal minimumSpan = 0.08;
        QVector<qreal> *cuts = nullptr;
        if (dividerAxis_ == 1) cuts = &columnCuts_;
        else if (dividerAxis_ == 2 && dividerColumn_ >= 0 &&
                 dividerColumn_ < columnRowCuts_.size()) cuts = &columnRowCuts_[dividerColumn_];
        if (!cuts || dividerIndex_ < 0 || dividerIndex_ >= cuts->size()) return;
        const qreal raw = dividerAxis_ == 1
                              ? (position.x() - workspaceRect_.left()) / workspaceRect_.width()
                              : (position.y() - workspaceRect_.top()) / workspaceRect_.height();
        const qreal lower = dividerIndex_ == 0 ? minimumSpan : cuts->at(dividerIndex_ - 1) + minimumSpan;
        const qreal upper = dividerIndex_ + 1 >= cuts->size()
                                ? 1.0 - minimumSpan
                                : cuts->at(dividerIndex_ + 1) - minimumSpan;
        (*cuts)[dividerIndex_] = std::clamp(raw, lower, upper);
        updateSectorItemsGeometry();
        relayoutSectorWidgets();
        update();
    }
    void finishSectorDividerDrag() {
        dividerAxis_ = 0;
        dividerIndex_ = -1;
        dividerColumn_ = -1;
        for (QGraphicsView *view : views()) view->viewport()->unsetCursor();
    }
    QRectF sectorRect(int index) const {
        int column = 0;
        int row = 0;
        if (!sectorCoordinates(index, &column, &row)) return QRectF();
        const qreal left = column == 0 ? 0.0 : columnCuts_.value(column - 1, qreal(column) / sectorColumns_);
        const qreal right = column + 1 == sectorColumns_
                                ? 1.0
                                : columnCuts_.value(column, qreal(column + 1) / sectorColumns_);
        const QVector<qreal> &rowCuts = columnRowCuts_[column];
        const int rows = columnRowCounts_.value(column, 1);
        const qreal top = row == 0 ? 0.0 : rowCuts.value(row - 1, qreal(row) / rows);
        const qreal bottom = row + 1 == rows ? 1.0 : rowCuts.value(row, qreal(row + 1) / rows);
        return QRectF(workspaceRect_.left() + workspaceRect_.width() * left,
                      workspaceRect_.top() + workspaceRect_.height() * top,
                      workspaceRect_.width() * (right - left),
                      workspaceRect_.height() * (bottom - top));
    }
    QString titleFor(const QString &type) const { return standardTitle ? standardTitle(type) : type; }
    DspBlockItem *selectedBlock() const {
        for (QGraphicsItem *item : selectedItems())
            if (auto *block = dynamic_cast<DspBlockItem*>(item)) return block;
        return nullptr;
    }
    void refreshResearchDisplays() {
        const QList<QGraphicsItem *> sceneItems = items();
        researchControllers_.clear();
        for (QGraphicsItem *item : sceneItems) {
            auto *view = dynamic_cast<DspBlockItem *>(item);
            if (!view || !isResearchViewType(view->blockType())) continue;
            DspBlockItem *settingsBlock = nullptr;
            const QString expectedType = settingsTypeForResearchView(view->blockType());
            const QString bindingId = view->settings()
                                          .value(QStringLiteral("bindingId"))
                                          .toString()
                                          .trimmed();
            if (!bindingId.isEmpty()) {
                for (QGraphicsItem *candidateItem : sceneItems) {
                    auto *candidate = dynamic_cast<DspBlockItem *>(candidateItem);
                    if (candidate && candidate->blockType() == expectedType &&
                        candidate->settings().value(QStringLiteral("bindingId")).toString().trimmed() == bindingId) {
                        settingsBlock = candidate;
                        break;
                    }
                }
            } else {
                for (QGraphicsItem *edgeItem : sceneItems) {
                    auto *edge = dynamic_cast<DspConnectionItem *>(edgeItem);
                    if (edge && !edge->isControlConnection() && edge->target() == view && edge->source() &&
                        edge->source()->blockType() == expectedType) {
                        settingsBlock = edge->source();
                        break;
                    }
                }
            }
            view->setResearchConnection(settingsBlock != nullptr,
                                        settingsBlock ? settingsBlock->settings() : QJsonObject(),
                                        ukrainian);
            bool branchInput = false;
            if (settingsBlock) {
                researchControllers_.insert(view->id(), settingsBlock->id());
                for (QGraphicsItem *edgeItem : sceneItems) {
                    auto *edge = dynamic_cast<DspConnectionItem *>(edgeItem);
                    if (edge && edge->target() == settingsBlock && edge->source()->blockType() == QStringLiteral("ddc_channelizer")) branchInput = true;
                }
            }
            view->setResearchExternalInput(branchInput);
        }
    }
    void refreshWorkspaceDisplayBindings() {
        const QList<QGraphicsItem *> sceneItems = items();
        for (QGraphicsItem *item : sceneItems) {
            auto *view = dynamic_cast<DspBlockItem *>(item);
            if (!view || (view->blockType() != QStringLiteral("workspace_spectrum") &&
                          view->blockType() != QStringLiteral("workspace_waterfall"))) continue;
            DspBlockItem *settingsBlock = nullptr;
            const QString bindingId =
                view->settings().value(QStringLiteral("bindingId")).toString().trimmed();
            if (!bindingId.isEmpty()) {
                for (QGraphicsItem *candidateItem : sceneItems) {
                    auto *candidate = dynamic_cast<DspBlockItem *>(candidateItem);
                    if (candidate && workspaceSettingsMatchView(candidate->blockType(), view->blockType()) &&
                        candidate->settings().value(QStringLiteral("bindingId")).toString().trimmed() == bindingId) {
                        settingsBlock = candidate;
                        break;
                    }
                }
            } else {
                for (QGraphicsItem *edgeItem : sceneItems) {
                    auto *edge = dynamic_cast<DspConnectionItem *>(edgeItem);
                    if (edge && !edge->isControlConnection() && edge->target() == view &&
                        edge->source() &&
                        workspaceSettingsMatchView(edge->source()->blockType(), view->blockType())) {
                        settingsBlock = edge->source();
                        break;
                    }
                }
            }
            QJsonObject controller;
            if (settingsBlock) {
                controller = settingsBlock->settings();
                if (settingsBlock->blockType() == QStringLiteral("waterfall_2d"))
                    controller.insert(QStringLiteral("displayMode"), 0);
                else if (settingsBlock->blockType() == QStringLiteral("waterfall_3d") &&
                         !controller.contains(QStringLiteral("displayMode")))
                    controller.insert(QStringLiteral("displayMode"), 1);
            }
            view->setWorkspaceControllerSettings(controller);
        }
    }

    DspBlockItem *blockAtInput(const QPointF &pos, DspBlockItem *source) const {
        const QList<QGraphicsItem *> sceneItems = items(Qt::DescendingOrder);
        for (QGraphicsItem *item : sceneItems) {
            auto *block = dynamic_cast<DspBlockItem *>(item);
            if (!block || block == source) continue;
            if (source && source->isVerticalControlSource()) {
                if (block->controlInputContains(pos, source->blockType()) ||
                    (acceptsControlType(block->blockType(), source->blockType()) &&
                     block->bodyContainsScenePoint(pos))) return block;
            } else if (block->inputContains(pos)) {
                return block;
            }
        }
        return nullptr;
    }
    DspBlockItem *blockAtOutput(const QPointF &pos) const {
        for (QGraphicsItem *item : items(Qt::DescendingOrder))
            if (auto *block = dynamic_cast<DspBlockItem*>(item); block && block->outputContains(pos)) return block;
        return nullptr;
    }
    void updateTemporaryConnection(const QPointF &to) {
        if (!pendingSource_ || !temporaryConnection_) return;
        QPainterPath path;
        if (pendingSource_->isVerticalControlSource()) {
            const QPointF from = pendingSource_->controlOutputScenePos(to);
            const qreal bend = std::max<qreal>(32.0, qAbs(to.y() - from.y()) * 0.45);
            const qreal direction = to.y() < from.y() ? -bend : bend;
            path.moveTo(from);
            path.cubicTo(from + QPointF(0.0, direction), to - QPointF(0.0, direction), to);
        } else {
            const QPointF from = pendingSource_->outputScenePos();
            const qreal bend = std::max<qreal>(45.0, qAbs(to.x() - from.x()) * 0.45);
            path.moveTo(from);
            path.cubicTo(from + QPointF(bend, 0.0), to - QPointF(bend, 0.0), to);
        }
        temporaryConnection_->setPath(path);
    }
    DspBlockItem *controlTargetFor(DspBlockItem *control) const {
        if (!control) return nullptr;
        for (QGraphicsItem *item : items()) {
            auto *edge = dynamic_cast<DspConnectionItem*>(item);
            if (edge && edge->isControlConnection() && edge->source() == control) return edge->target();
        }
        return nullptr;
    }
    void triggerControl(DspBlockItem *control) {
        if (!control || !controlTriggered) return;
        DspBlockItem *target = controlTargetFor(control);
        if (control->blockType() == QStringLiteral("enable_control") && !target) {
            setStatus(ukrainian ? QStringLiteral("Під'єднайте Вкл/Викл до сумісного блока")
                                : QStringLiteral("Connect On/Off to a compatible block"));
            return;
        }
        controlTriggered(control->blockType(), control->id(),
                         target ? target->blockType() : QString(),
                         target ? target->id() : QString());
    }
    void cancelPendingConnection() {
        pendingSource_ = nullptr;
        if (temporaryConnection_) { delete temporaryConnection_; temporaryConnection_ = nullptr; }
    }
    void cancelGraphInteraction() {
        cancelPendingConnection();
        pressedControl_ = nullptr;
        draggedBlock_ = nullptr;
        dragSelection_.clear();
        if (resizingBlock_) resizingBlock_->finishSceneResize();
        resizingBlock_ = nullptr;
        finishWorkspaceGroupDividerDrag();
        finishSectorDividerDrag();
    }
    void ensureFrameRoutes() const {
        if (!frameRoutesDirty_) return;
        workspaceFrameBlocks_.clear();
        vfoFrameBlocks_.clear();
        vfoFrameChannels_.clear();
        const auto snapshot = items();
        const auto plan = compileDspGraph(QJsonDocument::fromJson(configurationJson().toUtf8()).object());
        QSet<QString> explicitMainViews;
        for (QGraphicsItem *item : snapshot) {
            auto *edge = dynamic_cast<DspConnectionItem *>(item);
            if (edge && edge->source() && edge->target() && edge->source()->blockType() == QStringLiteral("fft") &&
                dspLinkKind(edge->source()->blockType(), edge->target()->blockType()) == DspLinkKind::Spectrum)
                explicitMainViews.insert(edge->target()->id());
        }
        for (QGraphicsItem *item : snapshot) {
            auto *block = dynamic_cast<DspBlockItem *>(item);
            if (!block) continue;
            // Unwired older layouts retain the shared-frame path; explicit routes must compile.
            if (block->isWorkspaceDisplayBlock() &&
                (!explicitMainViews.contains(block->id()) || plan.mainSpectrumRoutes.contains(block->id())))
                workspaceFrameBlocks_.append(block);
            if (block->blockType() == QStringLiteral("vfo_spectrum") ||
                block->blockType() == QStringLiteral("vfo_waterfall"))
                vfoFrameBlocks_.append(block);
        }
        for (QGraphicsItem *item : snapshot) {
            auto *edge = dynamic_cast<DspConnectionItem *>(item);
            if (edge && !edge->isControlConnection() && edge->target() && edge->source() &&
                edge->source()->blockType() == QStringLiteral("vfo_channel") &&
                !vfoFrameChannels_.contains(edge->target())) {
                const int index = edge->source()->vfoIndex();
                if (index >= 0) vfoFrameChannels_.insert(edge->target(), index);
            }
        }
        frameRoutesDirty_ = false;
    }
    void notifyChanged() {
        frameRoutesDirty_ = true;
        if (changed) changed();
    }
    void setStatus(const QString &status) { if (statusChanged) statusChanged(status); }

    DspBlockItem *pendingSource_ = nullptr;
    DspBlockItem *pressedControl_ = nullptr;
    QPointF controlPressPosition_;
    QGraphicsPathItem *temporaryConnection_ = nullptr;
    mutable bool frameRoutesDirty_ = true;
    mutable QList<DspBlockItem *> workspaceFrameBlocks_;
    mutable QList<DspBlockItem *> vfoFrameBlocks_;
    mutable QHash<DspBlockItem *, int> vfoFrameChannels_;
    QHash<QString, QString> researchControllers_;
    QHash<QString, QString> branchStatuses_;
    QString cachedVfoConfiguration_;
    QJsonArray cachedVfoChannels_;
    QRectF workspaceRect_{-800.0, -450.0, 1600.0, 900.0};
    int sectorColumns_ = 1;
    QVector<int> columnRowCounts_{1};
    QVector<qreal> columnCuts_;
    QVector<QVector<qreal>> columnRowCuts_{QVector<qreal>()};
    QVector<DspSectorItem *> sectorItems_;
    QPointer<QGraphicsView> nativeWorkspaceView_;
    bool workspaceAnalogPeakMeterEnabled_ = false;
    int workspaceAnalogPeakMeterStyle_ = 0;
    bool workspaceAnalogPeakMeterTargetValid_ = false;
    double workspaceAnalogPeakMeterTargetHz_ = 0.0;
    int dividerAxis_ = 0;
    int dividerIndex_ = -1;
    int dividerColumn_ = -1;
    int workspaceGroupResizeSector_ = -1;
    int workspaceGroupResizeBoundary_ = -1;
    QHash<QString, DockedSettingsProxy *> dockedSettingsPanels_;
    DspBlockItem *draggedBlock_ = nullptr;
    QList<DspBlockItem *> dragSelection_;
    DspBlockItem *resizingBlock_ = nullptr;
    QPointF blockPressPosition_;
};

DspFlowPanel::DspFlowPanel(QWidget *parent) : QDialog(parent, Qt::Window) {
    setModal(false);
    resize(1050, 680);
    setMinimumSize(720, 460);

    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(8, 8, 8, 8);
    rootLayout->setSpacing(6);
    auto *toolbar = new QHBoxLayout();
    toolbar->setContentsMargins(0, 0, 0, 0);
    toolbar->setSpacing(5);
    addButton_ = new QToolButton(this);
    addButton_->setPopupMode(QToolButton::InstantPopup);
    addButton_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    addButton_->setIcon(style()->standardIcon(QStyle::SP_FileDialogNewFolder));
    openButton_ = new QPushButton(this);
    renameButton_ = new QPushButton(this);
    deleteButton_ = new QPushButton(this);
    defaultButton_ = new QPushButton(this);
    fitButton_ = new QPushButton(this);
    sectorLayoutLabel_ = new QLabel(this);
    sectorLayoutCombo_ = new QComboBox(this);
    sectorLayoutCombo_->addItem(QStringLiteral("1 x 1"), QStringLiteral("1x1"));
    sectorLayoutCombo_->addItem(QStringLiteral("2 x 1"), QStringLiteral("2x1"));
    sectorLayoutCombo_->addItem(QStringLiteral("3 x 1"), QStringLiteral("3x1"));
    sectorLayoutCombo_->addItem(QStringLiteral("4 x 1"), QStringLiteral("4x1"));
    sectorLayoutCombo_->addItem(QStringLiteral("5 x 1"), QStringLiteral("5x1"));
    sectorLayoutCombo_->addItem(QStringLiteral("1 x 2"), QStringLiteral("1x2"));
    sectorLayoutCombo_->addItem(QStringLiteral("1 x 3"), QStringLiteral("1x3"));
    sectorLayoutCombo_->addItem(QStringLiteral("2 x 2"), QStringLiteral("2x2"));
    sectorLayoutCombo_->addItem(QStringLiteral("3 x 2"), QStringLiteral("3x2"));
    sectorLayoutCombo_->addItem(QStringLiteral("3 x 3"), QStringLiteral("3x3"));
    sectorLayoutCombo_->addItem(QStringLiteral("Custom"), QStringLiteral("custom"));
    sectorIndexCombo_ = new QComboBox(this);
    sectorIndexCombo_->setMinimumWidth(72);
    placeInSectorButton_ = new QPushButton(this);
    fullScreenButton_ = new QPushButton(this);
    returnToMainButton_ = new QPushButton(this);
    returnToMainButton_->setVisible(false);
    profileLabel_ = new QLabel(this);
    profileCombo_ = new QComboBox(this);
    profileCombo_->setMinimumContentsLength(12);
    profileCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    saveProfileButton_ = new QPushButton(this);
    deleteProfileButton_ = new QPushButton(this);
    statusLabel_ = new QLabel(this);
    statusLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    statusLabel_->setStyleSheet(QStringLiteral("color: #8793a2; padding-right: 4px;"));
    toolbar->addWidget(addButton_);
    auto *ddcButton = new QToolButton(this);
    ddcButton->setObjectName(QStringLiteral("addDdcBranch"));
    ddcButton->setText(QStringLiteral("DDC +"));
    connect(ddcButton, &QToolButton::clicked, this, &DspFlowPanel::addDdcBranch);
    toolbar->addWidget(ddcButton);
    auto *computeButton = new QToolButton(this);
    computeButton->setObjectName(QStringLiteral("dspStreamManager"));
    computeButton->setIcon(style()->standardIcon(QStyle::SP_ComputerIcon));
    computeButton->setToolTip(QStringLiteral("DSP stream manager / Менеджер потоків DSP"));
    toolbar->addWidget(computeButton);
    connect(computeButton, &QToolButton::clicked, this, &DspFlowPanel::showComputeDialog);
    toolbar->addWidget(openButton_);
    toolbar->addWidget(renameButton_);
    toolbar->addWidget(deleteButton_);
    toolbar->addWidget(defaultButton_);
    toolbar->addWidget(fitButton_);
    toolbar->addSpacing(8);
    toolbar->addWidget(profileLabel_);
    toolbar->addWidget(profileCombo_);
    toolbar->addWidget(saveProfileButton_);
    toolbar->addWidget(deleteProfileButton_);
    toolbar->addSpacing(8);
    toolbar->addWidget(sectorLayoutLabel_);
    toolbar->addWidget(sectorLayoutCombo_);
    toolbar->addWidget(sectorIndexCombo_);
    toolbar->addWidget(placeInSectorButton_);
    toolbar->addSpacing(8);
    toolbar->addWidget(fullScreenButton_);
    toolbar->addWidget(returnToMainButton_);
    toolbar->addStretch(1);
    statusLabel_->setMinimumWidth(0);
    toolbar->addWidget(statusLabel_);
    rootLayout->addLayout(toolbar);

    scene_ = new DspFlowScene(this);
    auto *dspView = new DspFlowView(scene_, this);
    view_ = dspView;
    scene_->setNativeWorkspaceView(view_);
    dspView->nativeWidgetsGeometryChanged = [this]() {
        if (scene_) scene_->syncNativeWorkspaceWidgets();
    };
    rootLayout->addWidget(view_, 1);

    saveTimer_ = new QTimer(this);
    saveTimer_->setSingleShot(true);
    saveTimer_->setInterval(450);
    scene_->changed = [this]() {
        refreshSectorControls();
        scheduleConfigurationChanged();
    };
    scene_->standardTitle = [this](const QString &type) { return blockTitle(type); };
    scene_->statusChanged = [this](const QString &status) { statusLabel_->setText(status); };
    scene_->addRequested = [this](const QString &type, const QPointF &position) {
        DspBlockItem *block = scene_->addBlock(type, blockTitle(type), position);
        if (workspaceMode_) scene_->prepareNewBlockForWorkspace(block, position);
        emit blockCreated(type, block->id());
    };
    scene_->activated = [this](const QString &type, const QString &id) { emit blockActivated(type, id); };
    scene_->controlTriggered = [this](const QString &controlType, const QString &controlId,
                                      const QString &targetType, const QString &targetId) {
        emit controlTriggered(controlType, controlId, targetType, targetId);
    };
    scene_->fineTuneDelta = [this](double deltaHz) { emit fineTuneDeltaRequested(deltaHz); };
    scene_->workspaceScale = [this](int direction) { emit workspaceScaleChanged(direction); };
    scene_->workspacePan = [this](int deltaPixels, int widthPixels) {
        emit workspacePanRequested(deltaPixels, widthPixels);
    };
    scene_->workspaceTuneContext = [this](double frequency, const QPoint &globalPos) {
        emit workspaceTuneContextRequested(frequency, globalPos);
    };
    scene_->workspaceAutoTune = [this](double frequency) { emit workspaceAutoTuneRequested(frequency); };
    scene_->workspaceScienceMarker = [this](double frequency) {
        emit workspaceScienceMarkerRequested(frequency);
    };
    scene_->workspaceMultiVfoSelection = [this](double lowHz, double highHz) {
        emit workspaceMultiVfoSelectionRequested(lowHz, highHz);
    };
    scene_->workspaceListeningFrequency = [this](double frequency) {
        emit workspaceListeningFrequencyRequested(frequency);
    };
    scene_->workspaceCenterFrequency = [this](double frequency) {
        emit workspaceCenterFrequencyRequested(frequency);
    };
    scene_->workspaceTuning = [this](double listening, double center) {
        emit workspaceTuningRequested(listening, center);
    };
    scene_->workspaceAnalogMeterToggled = [this](bool enabled) {
        emit workspaceAnalogPeakMeterToggled(enabled);
    };
    scene_->workspaceAnalogMeterStyleChanged = [this](int style) {
        emit workspaceAnalogPeakMeterStyleChanged(style);
    };
    static_cast<DspFlowView *>(view_)->workspaceWheelHandler =
        [this](const QPointF &position, int direction) {
            return scene_ && scene_->zoomSectorContentsAt(position, direction);
        };
    static_cast<DspFlowView *>(view_)->workspacePanHandler =
        [this](const QPointF &position, const QPointF &delta, bool finalize) {
            return scene_ && scene_->panSectorContentsAt(position, delta, finalize);
        };

    auto *controlStateTimer = new QTimer(this);
    controlStateTimer->setInterval(180);
    connect(controlStateTimer, &QTimer::timeout, this, [this]() {
        if (isVisible()) emit controlStateRefreshRequested();
    });
    controlStateTimer->start();
    auto *branchTimer = new QTimer(this);
    branchTimer->setInterval(20);
    connect(branchTimer, &QTimer::timeout, this, &DspFlowPanel::refreshBranches);
    branchTimer->start();

    connect(openButton_, &QPushButton::clicked, this, [this]() { scene_->activateSelection(); });
    connect(renameButton_, &QPushButton::clicked, this, [this]() { scene_->renameSelection(); });
    connect(deleteButton_, &QPushButton::clicked, this, [this]() { scene_->deleteSelection(); });
    connect(defaultButton_, &QPushButton::clicked, this, [this]() {
        scene_->resetDefault();
        if (workspaceMode_) syncWorkspaceViewport();
        else view_->fitInView(scene_->itemsBoundingRect().adjusted(-80.0, -80.0, 80.0, 80.0), Qt::KeepAspectRatio);
    });
    connect(fitButton_, &QPushButton::clicked, this, [this]() {
        if (workspaceMode_) {
            syncWorkspaceViewport();
            return;
        }
        const QRectF bounds = scene_->itemsBoundingRect();
        if (!bounds.isEmpty()) view_->fitInView(bounds.adjusted(-80.0, -80.0, 80.0, 80.0), Qt::KeepAspectRatio);
    });
    connect(sectorLayoutCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) {
                if (loading_ || !scene_) return;
                const QStringList parts = sectorLayoutCombo_->currentData().toString().split(QLatin1Char('x'));
                if (parts.size() != 2) return;
                scene_->setSectorLayout(parts[0].toInt(), parts[1].toInt());
                refreshSectorControls();
            });
    connect(placeInSectorButton_, &QPushButton::clicked, this, [this]() {
        if (!scene_ || !scene_->placeSelectionInSector(sectorIndexCombo_->currentData().toInt())) {
            statusLabel_->setText(ukrainian_
                ? QStringLiteral("Оберіть блок і сектор")
                : QStringLiteral("Select a block and a sector"));
        }
    });
    connect(fullScreenButton_, &QPushButton::clicked, this, [this]() {
        if (isFullScreen()) showMaximized();
        else showFullScreen();
        fullScreenButton_->setText(isFullScreen()
            ? (ukrainian_ ? QStringLiteral("Вікно") : QStringLiteral("Window"))
            : (ukrainian_ ? QStringLiteral("На весь екран") : QStringLiteral("Full screen")));
    });
    auto *fullScreenAction = new QAction(this);
    fullScreenAction->setShortcut(QKeySequence(Qt::Key_F11));
    addAction(fullScreenAction);
    connect(fullScreenAction, &QAction::triggered, fullScreenButton_, &QPushButton::click);
    connect(returnToMainButton_, &QPushButton::clicked,
            this, &DspFlowPanel::workspaceModeExitRequested);
    connect(profileCombo_, QOverload<int>::of(&QComboBox::activated),
            this, &DspFlowPanel::loadWorkspaceProfile);
    connect(saveProfileButton_, &QPushButton::clicked, this, &DspFlowPanel::saveWorkspaceProfile);
    connect(deleteProfileButton_, &QPushButton::clicked, this, &DspFlowPanel::deleteWorkspaceProfile);
    connect(saveTimer_, &QTimer::timeout, this, [this]() {
        if (!loading_) emit configurationChanged(configurationJson());
    });

    setLanguage(false);
    refreshWorkspaceProfiles();
    refreshSectorControls();
    loading_ = true;
    scene_->resetDefault(false);
    loading_ = false;
    QTimer::singleShot(0, this, [this]() {
        view_->fitInView(scene_->itemsBoundingRect().adjusted(-80.0, -80.0, 80.0, 80.0), Qt::KeepAspectRatio);
    });
}

void DspFlowPanel::setLanguage(bool ukrainian) {
    ukrainian_ = ukrainian;
    setWindowTitle(ukrainian_ ? QStringLiteral("Конструктор DSP-шляху") : QStringLiteral("DSP Path Designer"));
    addButton_->setText(ukrainian_ ? QStringLiteral("Додати") : QStringLiteral("Add"));
    openButton_->setText(ukrainian_ ? QStringLiteral("Відкрити") : QStringLiteral("Open"));
    renameButton_->setText(ukrainian_ ? QStringLiteral("Назва") : QStringLiteral("Rename"));
    deleteButton_->setText(ukrainian_ ? QStringLiteral("Видалити") : QStringLiteral("Delete"));
    defaultButton_->setText(ukrainian_ ? QStringLiteral("Типова схема") : QStringLiteral("Default path"));
    fitButton_->setText(ukrainian_ ? QStringLiteral("Вмістити") : QStringLiteral("Fit"));
    sectorLayoutLabel_->setText(ukrainian_ ? QStringLiteral("Сектори:") : QStringLiteral("Sectors:"));
    placeInSectorButton_->setText(ukrainian_ ? QStringLiteral("У сектор") : QStringLiteral("Fill sector"));
    fullScreenButton_->setText(isFullScreen()
        ? (ukrainian_ ? QStringLiteral("Вікно") : QStringLiteral("Window"))
        : (ukrainian_ ? QStringLiteral("На весь екран") : QStringLiteral("Full screen")));
    returnToMainButton_->setText(ukrainian_ ? QStringLiteral("Основне вікно") : QStringLiteral("Main window"));
    profileLabel_->setText(ukrainian_ ? QStringLiteral("Схема:") : QStringLiteral("Layout:"));
    saveProfileButton_->setText(ukrainian_ ? QStringLiteral("Зберегти") : QStringLiteral("Save"));
    deleteProfileButton_->setText(ukrainian_ ? QStringLiteral("Видалити") : QStringLiteral("Delete"));
    addButton_->setToolTip(ukrainian_ ? QStringLiteral("Додати DSP-блок на поле")
                                     : QStringLiteral("Add a DSP block to the canvas"));
    openButton_->setToolTip(ukrainian_ ? QStringLiteral("Відкрити чинні налаштування модуля")
                                      : QStringLiteral("Open the existing module settings"));
    renameButton_->setToolTip(ukrainian_ ? QStringLiteral("Перейменувати обраний блок")
                                        : QStringLiteral("Rename the selected block"));
    deleteButton_->setToolTip(ukrainian_ ? QStringLiteral("Видалити обрані блоки або з'єднання")
                                        : QStringLiteral("Delete selected blocks or connections"));
    defaultButton_->setToolTip(ukrainian_ ? QStringLiteral("Відновити типову схему прийому")
                                         : QStringLiteral("Restore the default receive path"));
    fitButton_->setToolTip(ukrainian_ ? QStringLiteral("Вмістити всю схему у вікно")
                                     : QStringLiteral("Fit the whole graph in the window"));
    const int customLayoutIndex = sectorLayoutCombo_->findData(QStringLiteral("custom"));
    if (customLayoutIndex >= 0)
        sectorLayoutCombo_->setItemText(customLayoutIndex,
            ukrainian_ ? QStringLiteral("Власний") : QStringLiteral("Custom"));
    sectorLayoutCombo_->setToolTip(ukrainian_
        ? QStringLiteral("Готові схеми секторів; незалежний поділ стовпця доступний правою кнопкою")
        : QStringLiteral("Sector presets; right-click a column to split it independently"));
    placeInSectorButton_->setToolTip(ukrainian_
        ? QStringLiteral("Розтягнути обраний візуальний блок на обраний сектор")
        : QStringLiteral("Fit the selected visual block to the whole sector; manual resizing disables auto-fit"));
    fullScreenButton_->setToolTip(ukrainian_
        ? QStringLiteral("Перемкнути повноекранний режим (F11)")
        : QStringLiteral("Toggle full screen (F11)"));
    returnToMainButton_->setToolTip(ukrainian_
        ? QStringLiteral("Вийти з альтернативного робочого місця")
        : QStringLiteral("Leave the alternative workspace"));
    profileCombo_->setToolTip(ukrainian_ ? QStringLiteral("Завантажити збережену схему DSP-дошки")
                                        : QStringLiteral("Load a saved DSP-board layout"));
    saveProfileButton_->setToolTip(ukrainian_ ? QStringLiteral("Зберегти поточні блоки, зв'язки й розміри як іменовану схему")
                                             : QStringLiteral("Save the current blocks, links and sizes as a named layout"));
    deleteProfileButton_->setToolTip(ukrainian_ ? QStringLiteral("Видалити обрану збережену схему")
                                               : QStringLiteral("Delete the selected saved layout"));
    statusLabel_->setText(ukrainian_ ? QStringLiteral("Готово") : QStringLiteral("Ready"));
    scene_->ukrainian = ukrainian_;
    if (auto *button = findChild<QToolButton *>(QStringLiteral("addDdcBranch")))
        button->setToolTip(ukrainian_ ? QStringLiteral("Додати окрему IQ-гілку DDC → FFT → спектр/водоспад") : QStringLiteral("Add an independent DDC -> FFT -> spectrum/waterfall IQ branch"));
    scene_->refreshStandardTitles();
    scene_->refreshResearchDisplayLanguage();
    rebuildAddMenu();
    refreshWorkspaceProfiles();
}

void DspFlowPanel::refreshWorkspaceProfiles(const QString &selectedName) {
    const QString wanted = selectedName.isEmpty() && profileCombo_
                               ? profileCombo_->currentData().toString()
                               : selectedName;
    const QSignalBlocker blocker(profileCombo_);
    profileCombo_->clear();
    profileCombo_->addItem(ukrainian_ ? QStringLiteral("Поточна (автозбереження)")
                                     : QStringLiteral("Current (auto-save)"),
                           QString());
    const QVector<DspWorkspaceProfile> profiles = loadDspWorkspaceProfiles();
    for (const auto &profile : profiles) profileCombo_->addItem(profile.name, profile.name);
    const int selected = profileCombo_->findData(wanted);
    profileCombo_->setCurrentIndex(selected >= 0 ? selected : 0);
    deleteProfileButton_->setEnabled(profileCombo_->currentIndex() > 0);
}

void DspFlowPanel::saveWorkspaceProfile() {
    const QString currentName = profileCombo_->currentData().toString();
    bool accepted = false;
    const QString name = QInputDialog::getText(
        this,
        ukrainian_ ? QStringLiteral("Зберегти схему DSP") : QStringLiteral("Save DSP layout"),
        ukrainian_ ? QStringLiteral("Назва схеми:") : QStringLiteral("Layout name:"),
        QLineEdit::Normal,
        currentName,
        &accepted).trimmed();
    if (!accepted || name.isEmpty()) return;

    QVector<DspWorkspaceProfile> profiles = loadDspWorkspaceProfiles();
    auto existing = std::find_if(profiles.begin(), profiles.end(), [&name](const auto &profile) {
        return profile.name.compare(name, Qt::CaseInsensitive) == 0;
    });
    if (existing != profiles.end()) {
        const auto answer = QMessageBox::question(
            this,
            ukrainian_ ? QStringLiteral("Замінити схему?") : QStringLiteral("Replace layout?"),
            ukrainian_ ? QStringLiteral("Схема з такою назвою вже існує. Замінити її поточним станом?")
                       : QStringLiteral("A layout with this name already exists. Replace it with the current state?"));
        if (answer != QMessageBox::Yes) return;
        existing->name = name;
        existing->configuration = configurationJson();
    } else {
        profiles.push_back({name, configurationJson()});
    }
    storeDspWorkspaceProfiles(profiles);
    refreshWorkspaceProfiles(name);
    statusLabel_->setText(ukrainian_ ? QStringLiteral("Схему збережено") : QStringLiteral("Layout saved"));
}

void DspFlowPanel::loadWorkspaceProfile(int index) {
    deleteProfileButton_->setEnabled(index > 0);
    if (index <= 0) return;
    const QString name = profileCombo_->itemData(index).toString();
    const QVector<DspWorkspaceProfile> profiles = loadDspWorkspaceProfiles();
    const auto profile = std::find_if(profiles.cbegin(), profiles.cend(), [&name](const auto &item) {
        return item.name == name;
    });
    if (profile == profiles.cend() || !setConfigurationJson(profile->configuration)) {
        statusLabel_->setText(ukrainian_ ? QStringLiteral("Не вдалося завантажити схему")
                                        : QStringLiteral("Could not load layout"));
        return;
    }
    statusLabel_->setText(ukrainian_ ? QStringLiteral("Схему завантажено") : QStringLiteral("Layout loaded"));
}

void DspFlowPanel::deleteWorkspaceProfile() {
    const QString name = profileCombo_->currentData().toString();
    if (name.isEmpty()) return;
    const auto answer = QMessageBox::question(
        this,
        ukrainian_ ? QStringLiteral("Видалити схему?") : QStringLiteral("Delete layout?"),
        ukrainian_ ? QStringLiteral("Видалити збережену схему «%1»?").arg(name)
                   : QStringLiteral("Delete the saved layout '%1'?").arg(name));
    if (answer != QMessageBox::Yes) return;
    QVector<DspWorkspaceProfile> profiles = loadDspWorkspaceProfiles();
    profiles.erase(std::remove_if(profiles.begin(), profiles.end(), [&name](const auto &profile) {
        return profile.name == name;
    }), profiles.end());
    storeDspWorkspaceProfiles(profiles);
    refreshWorkspaceProfiles();
}

QString DspFlowPanel::configurationJson() const {
    if (!scene_) return QString();
    auto root = QJsonDocument::fromJson(scene_->configurationJson().toUtf8()).object();
    root.insert(QStringLiteral("spectrumCompute"), QJsonObject{
        {QStringLiteral("mode"), int(spectrumComputeOptions_.mode)},
        {QStringLiteral("backend"), spectrumComputeOptions_.backend},
        {QStringLiteral("uploadLimitMBps"), spectrumComputeOptions_.uploadLimitMBps},
        {QStringLiteral("fallbackToLocal"), spectrumComputeOptions_.fallbackToLocal}});
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

bool DspFlowPanel::setConfigurationJson(const QString &json) {
    if (!json.trimmed().isEmpty()) {
        const auto object = QJsonDocument::fromJson(json.toUtf8()).object();
        if (!object.isEmpty()) {
            const auto plan = compileDspGraph(object);
            if (!plan.errors.isEmpty()) {
                statusLabel_->setText(plan.errors.join(QStringLiteral("; ")));
                return false;
            }
        }
    }
    loading_ = true;
    bool loaded = false;
    if (!json.trimmed().isEmpty()) loaded = scene_->loadConfiguration(json);
    if (!loaded) scene_->resetDefault(false);
    if (loaded) {
        const auto options = QJsonDocument::fromJson(json.toUtf8()).object()
                                 .value(QStringLiteral("spectrumCompute")).toObject();
        SpectrumComputeOptions restored;
        restored.mode = SpectrumComputeMode(options.value(QStringLiteral("mode")).toInt(0));
        restored.backend = options.value(QStringLiteral("backend")).toInt(1);
        restored.uploadLimitMBps = options.value(QStringLiteral("uploadLimitMBps")).toInt(100);
        restored.fallbackToLocal = options.value(QStringLiteral("fallbackToLocal")).toBool(false);
        setSpectrumComputeOptions(restored);
    }
    loading_ = false;
    refreshSectorControls();
    QTimer::singleShot(0, this, [this]() {
        if (workspaceMode_) {
            syncWorkspaceViewport();
            return;
        }
        const QRectF bounds = scene_->itemsBoundingRect();
        if (!bounds.isEmpty()) view_->fitInView(bounds.adjusted(-80.0, -80.0, 80.0, 80.0), Qt::KeepAspectRatio);
    });
    if (loaded) scheduleConfigurationChanged();
    return loaded;
}

DspFlowPanel::~DspFlowPanel() { branchEngine_.reset(); }

void DspFlowPanel::setBranchSourceContext(const RadioSettings &settings, bool running, int updateMs, int overlapPercent) {
    if (!running || settings.simplifiedAudioChannelizer) {
        const auto ids = branchRecordings_.keys();
        for (const auto &id : ids) stopBranchRecording(id);
    }
    branchSource_ = settings;
    branchSourceRunning_ = running;
    mainFftUpdateMs_ = std::clamp(updateMs, 1, 5000);
    mainFftOverlapPercent_ = std::clamp(overlapPercent, 0, 95);
    if (computeDialog_) computeDialog_->setProbeContext(settings.sampleRate, settings.fftLength,
                                                       mainFftUpdateMs_, mainFftOverlapPercent_);
}

QString DspFlowPanel::branchStatus(const QString &id) const {
    if (branchSource_.simplifiedAudioChannelizer)
        return ukrainian_ ? QStringLiteral("Додаткові потоки вимкнено спрощеним режимом") : QStringLiteral("Additional streams disabled by simplified mode");
    return scene_->branchStatus(id);
}

void DspFlowPanel::setExternalEndpoint(const DspStreamEndpoint &endpoint) {
    if (externalEndpoint_ == endpoint) return;
    externalEndpoint_ = endpoint;
    if (branchEngine_) branchEngine_->setExternalEndpoint(endpoint);
    if (computeDialog_) computeDialog_->setEndpoint(endpoint);
    emit externalEndpointChanged();
}

void DspFlowPanel::setSpectrumComputeOptions(const SpectrumComputeOptions &options) {
    auto normalized = options;
    if (int(normalized.mode) < 0 || int(normalized.mode) > 2) normalized.mode = SpectrumComputeMode::Local;
    if (normalized.backend < 0 || normalized.backend > 2) normalized.backend = 1;
    normalized.uploadLimitMBps = std::clamp(normalized.uploadLimitMBps, 1, 2000);
    if (spectrumComputeOptions_ == normalized) return;
    spectrumComputeOptions_ = normalized;
    if (computeDialog_) computeDialog_->setSpectrumComputeOptions(normalized);
    if (!loading_ && saveTimer_) saveTimer_->start();
    emit spectrumComputeOptionsChanged();
}

void DspFlowPanel::setSpectrumComputeStatus(const QString &status) {
    if (spectrumComputeStatus_ == status) return;
    spectrumComputeStatus_ = status;
    if (computeDialog_) computeDialog_->setSpectrumComputeStatus(status);
    emit spectrumComputeStatusChanged(status);
}

void DspFlowPanel::showComputeDialog() {
    if (!computeDialog_) {
        computeDialog_ = new DspComputeDialog(externalEndpoint_, ukrainian_, this);
        computeDialog_->setSpectrumComputeOptions(spectrumComputeOptions_);
        computeDialog_->setSpectrumComputeStatus(spectrumComputeStatus_);
        computeDialog_->setProbeContext(branchSource_.sampleRate, branchSource_.fftLength,
                                         mainFftUpdateMs_, mainFftOverlapPercent_);
        connect(computeDialog_, &DspComputeDialog::endpointChanged, this, [this]() {
            if (computeDialog_) setExternalEndpoint(computeDialog_->endpoint());
        });
        connect(computeDialog_, &DspComputeDialog::spectrumComputeOptionsChanged, this, [this]() {
            if (computeDialog_) setSpectrumComputeOptions(computeDialog_->spectrumComputeOptions());
        });
    }
    computeDialog_->show();
    computeDialog_->raise();
    computeDialog_->activateWindow();
}

bool DspFlowPanel::startBranchRecording(const QString &id, const QString &directory) {
    if (!branchSourceRunning_ || branchSource_.simplifiedAudioChannelizer || directory.isEmpty() ||
        blockTypeForId(id) != QStringLiteral("ddc_channelizer") || !blockSettings(id).value("enabled").toBool() ||
        (branchRecordings_.size() >= 2 && !branchRecordings_.contains(id))) return false;
    branchRecordings_.insert(id, directory);
    if (branchEngine_) branchEngine_->setRecording(id, directory);
    return true;
}
void DspFlowPanel::stopBranchRecording(const QString &id) {
    if (!branchRecordings_.remove(id)) return;
    if (branchEngine_) branchEngine_->setRecording(id, {});
}
bool DspFlowPanel::branchRecordingRequested(const QString &id) const { return branchRecordings_.contains(id); }

void DspFlowPanel::refreshBranches() {
    if (branchSource_.simplifiedAudioChannelizer || !branchSourceRunning_ ||
        ((!isVisible() || isMinimized()) && branchRecordings_.isEmpty())) {
        if (branchEngine_) branchEngine_->configure(branchPlan_, branchSource_, false);
        return;
    }
    if (branchPlanDirty_) {
        auto plan = compileDspGraph(QJsonDocument::fromJson(configurationJson().toUtf8()).object());
        if (branchPlan_.executionKey != plan.executionKey || branchPlan_.spectrumRoutes != plan.spectrumRoutes)
            scene_->resetBranchRouteStatus(plan);
        branchPlan_ = std::move(plan);
        branchPlanDirty_ = false;
        if (!branchPlan_.errors.isEmpty()) statusLabel_->setText(
            (ukrainian_ ? QStringLiteral("Гілки DSP зупинено: ") : QStringLiteral("DSP branches stopped: ")) + branchPlan_.errors.join(QStringLiteral("; ")));
    }
    if (branchPlan_.branches.isEmpty()) {
        const auto ids = branchRecordings_.keys();
        for (const auto &id : ids) stopBranchRecording(id);
        if (branchEngine_) branchEngine_->configure(branchPlan_, branchSource_, false);
        return;
    }
    auto plan = scene_->visibleBranchPlan(branchPlan_);
    const auto recordingIds = branchRecordings_.keys();
    for (const auto &id : recordingIds) {
        const auto original = std::find_if(branchPlan_.branches.begin(), branchPlan_.branches.end(), [&id](const auto &b) { return b.id == id && b.enabled; });
        if (original == branchPlan_.branches.end()) { stopBranchRecording(id); continue; }
        auto activeBranch = std::find_if(plan.branches.begin(), plan.branches.end(), [&id](const auto &b) { return b.id == id; });
        if (activeBranch == plan.branches.end()) { auto copy = *original; copy.ffts.clear(); copy.iqConsumer = true; plan.branches.append(copy); }
        else activeBranch->iqConsumer = true;
        plan.executionKey += QByteArray("|record:")+id.toUtf8();
    }
    bool enabled = false;
    for (const auto &b : plan.branches) if (b.enabled && (b.iqConsumer || !b.ffts.isEmpty())) enabled = true;
    const bool active = enabled && !branchSource_.simplifiedAudioChannelizer && branchSourceRunning_ &&
        ((isVisible() && !isMinimized()) || !branchRecordings_.isEmpty());
    if (!branchEngine_ && active) {
        branchEngine_ = std::make_unique<DspBranchEngine>();
        branchEngine_->setExternalEndpoint(externalEndpoint_);
        for (auto it = branchRecordings_.cbegin(); it != branchRecordings_.cend(); ++it) branchEngine_->setRecording(it.key(), it.value());
    }
    if (!branchEngine_) return;
    branchEngine_->configure(plan, branchSource_, active);
    for (const auto &frame : branchEngine_->takeFrames()) scene_->applyBranchFrame(branchPlan_, frame);
}

void DspFlowPanel::addDdcBranch() {
    scene_->addDdcBranch(workspaceMode_ ? sectorIndexCombo_->currentData().toInt() : -1);
    if (!workspaceMode_) {
        const auto bounds = scene_->itemsBoundingRect();
        view_->fitInView(bounds.adjusted(-40, -40, 40, 40), Qt::KeepAspectRatio);
    }
}

void DspFlowPanel::refreshSectorControls() {
    if (!scene_ || !sectorLayoutCombo_ || !sectorIndexCombo_) return;
    const QString layout = scene_->hasUniformSectorRows()
                               ? QStringLiteral("%1x%2").arg(scene_->sectorColumns()).arg(scene_->sectorRows())
                               : QStringLiteral("custom");
    {
        const QSignalBlocker blocker(sectorLayoutCombo_);
        int index = sectorLayoutCombo_->findData(layout);
        if (index < 0) index = sectorLayoutCombo_->findData(QStringLiteral("custom"));
        if (index >= 0) sectorLayoutCombo_->setCurrentIndex(index);
    }
    const int selected = sectorIndexCombo_->currentData().toInt();
    const QSignalBlocker blocker(sectorIndexCombo_);
    sectorIndexCombo_->clear();
    for (int index = 0; index < scene_->sectorCount(); ++index) {
        sectorIndexCombo_->addItem(ukrainian_
            ? QStringLiteral("Сектор %1").arg(index + 1)
            : QStringLiteral("Sector %1").arg(index + 1), index);
    }
    const int restored = sectorIndexCombo_->findData(selected);
    sectorIndexCombo_->setCurrentIndex(restored >= 0 ? restored : 0);
}

void DspFlowPanel::setAnalogPeakMeterEnabled(bool enabled) {
    analogPeakMeterEnabled_ = enabled;
    if (scene_) scene_->setWorkspaceAnalogPeakMeterEnabled(enabled);
}

void DspFlowPanel::setAnalogPeakMeterStyle(int style) {
    analogPeakMeterStyle_ = std::clamp(style, 0, 1);
    if (scene_) scene_->setWorkspaceAnalogPeakMeterStyle(analogPeakMeterStyle_);
}

void DspFlowPanel::setAnalogPeakMeterTarget(double frequencyHz, bool valid) {
    analogPeakMeterTargetValid_ = valid && std::isfinite(frequencyHz);
    analogPeakMeterTargetHz_ = analogPeakMeterTargetValid_ ? frequencyHz : 0.0;
    if (scene_) scene_->setWorkspaceAnalogPeakMeterTarget(analogPeakMeterTargetHz_, analogPeakMeterTargetValid_);
}

void DspFlowPanel::setWorkspaceVisualizationSettings(const QJsonObject &settings) {
    if (scene_) scene_->setWorkspaceVisualizationSettings(settings);
}

void DspFlowPanel::setWorkspaceMode(bool enabled) {
    workspaceMode_ = enabled;
    returnToMainButton_->setVisible(enabled);
    if (layout()) layout()->setContentsMargins(enabled ? 0 : 8, enabled ? 0 : 8,
                                               enabled ? 0 : 8, enabled ? 0 : 8);
    if (auto *flowView = static_cast<DspFlowView *>(view_)) flowView->setWorkspaceMode(enabled);
    if (enabled && scene_) {
        scene_->ensureWorkspaceDisplays();
        scene_->setWorkspaceAnalogPeakMeterEnabled(analogPeakMeterEnabled_);
        scene_->setWorkspaceAnalogPeakMeterStyle(analogPeakMeterStyle_);
        scene_->setWorkspaceAnalogPeakMeterTarget(analogPeakMeterTargetHz_, analogPeakMeterTargetValid_);
        refreshSectorControls();
        QTimer::singleShot(0, this, [this]() { syncWorkspaceViewport(); });
    } else if (scene_ && view_) {
        scene_->setSceneRect(-1800.0, -1200.0, 3600.0, 2400.0);
        const QRectF bounds = scene_->itemsBoundingRect();
        if (!bounds.isEmpty())
            view_->fitInView(bounds.adjusted(-80.0, -80.0, 80.0, 80.0), Qt::KeepAspectRatio);
    }
    setWindowTitle(enabled
        ? (ukrainian_ ? QStringLiteral("Obrii SDR - робоче місце")
                      : QStringLiteral("Obrii SDR - workspace"))
        : (ukrainian_ ? QStringLiteral("Конструктор DSP-шляху")
                      : QStringLiteral("DSP Path Designer")));
}

void DspFlowPanel::syncWorkspaceViewport() {
    if (!workspaceMode_ || !scene_ || !view_ || !view_->viewport()) return;
    const QSize viewportSize = view_->viewport()->size();
    if (viewportSize.width() < 2 || viewportSize.height() < 2) return;
    view_->resetTransform();
    scene_->setWorkspaceViewportSize(viewportSize);
    view_->setSceneRect(QRectF(QPointF(0.0, 0.0), QSizeF(viewportSize)));
}

void DspFlowPanel::resizeEvent(QResizeEvent *event) {
    QDialog::resizeEvent(event);
    if (workspaceMode_) QTimer::singleShot(0, this, [this]() { syncWorkspaceViewport(); });
}

void DspFlowPanel::closeEvent(QCloseEvent *event) {
    if (saveTimer_ && saveTimer_->isActive()) saveTimer_->stop();
    if (!loading_ && scene_) emit configurationChanged(configurationJson());
    if (workspaceMode_) {
        emit workspaceCloseRequested();
        event->ignore();
        return;
    }
    QDialog::closeEvent(event);
}

void DspFlowPanel::setControlStates(bool receiverRunning, const QHash<QString, bool> &enabledByBlockType) {
    if (scene_) scene_->setControlStates(receiverRunning, enabledByBlockType);
}

void DspFlowPanel::setFineTuneRangeHz(double rangeHz) {
    if (scene_) scene_->setFineTuneRangeHz(rangeHz);
}

bool DspFlowPanel::shouldUpdateMultiVfoSpectrum() {
    if (!scene_ || !isVisible() || isMinimized()) {
        return false;
    }
    constexpr qint64 MinimumUpdateIntervalMs = 33;
    if (multiVfoSpectrumUpdateTimer_.isValid() &&
        multiVfoSpectrumUpdateTimer_.elapsed() < MinimumUpdateIntervalMs) {
        return false;
    }
    multiVfoSpectrumUpdateTimer_.restart();
    return scene_->hasMultiVfoDisplayBlocks();
}

bool DspFlowPanel::shouldUpdateWorkspaceSpectrum() {
    if (!scene_ || !isVisible() || isMinimized()) return false;
    constexpr qint64 MinimumUpdateIntervalMs = 16;
    if (workspaceSpectrumUpdateTimer_.isValid() &&
        workspaceSpectrumUpdateTimer_.elapsed() < MinimumUpdateIntervalMs) return false;
    workspaceSpectrumUpdateTimer_.restart();
    return scene_->hasWorkspaceDisplayFrames();
}

int DspFlowPanel::workspaceSpectrumTargetBins() const {
    return scene_ && isVisible() && !isMinimized() ? scene_->workspaceSpectrumTargetBins() : 0;
}

void DspFlowPanel::updateWorkspaceSpectrum(const std::vector<float> &frequencies,
                                           const std::vector<float> &levels,
                                           double centerHz,
                                           double listeningHz,
                                           double sampleRate,
                                           double bandwidthHz,
                                           int modulationType) {
    if (scene_ && isVisible() && !isMinimized()) {
        scene_->updateWorkspaceSpectrum(frequencies, levels, centerHz, listeningHz, sampleRate,
                                        bandwidthHz, modulationType);
    }
}

void DspFlowPanel::updateMultiVfoSpectrum(const QString &configurationJson,
                                          const std::vector<float> &frequencies,
                                          const std::vector<float> &levels) {
    if (scene_ && isVisible() && !isMinimized())
        scene_->updateMultiVfoSpectrum(configurationJson, frequencies, levels);
}

void DspFlowPanel::addMultiVfoBranch(int channelIndex) {
    if (!scene_) return;
    scene_->addMultiVfoBranch(blockTitle(QStringLiteral("vfo_channel")),
                              blockTitle(QStringLiteral("vfo_spectrum")),
                              blockTitle(QStringLiteral("vfo_waterfall")),
                              channelIndex);
}

int DspFlowPanel::vfoIndexForBlock(const QString &blockId) const {
    return scene_ ? scene_->vfoIndexForBlock(blockId) : -1;
}

bool DspFlowPanel::assignVfoIndexToBlock(const QString &blockId, int channelIndex) {
    return scene_ && scene_->assignVfoIndexToBlock(blockId, channelIndex);
}

QJsonObject DspFlowPanel::blockSettings(const QString &blockId) const {
    return scene_ ? scene_->blockSettings(blockId) : QJsonObject();
}

QString DspFlowPanel::blockTypeForId(const QString &blockId) const {
    return scene_ ? scene_->blockTypeForId(blockId) : QString();
}

QString DspFlowPanel::boundWorkspaceSettingsBlockId(const QString &viewBlockId) const {
    return scene_ ? scene_->boundWorkspaceSettingsBlockId(viewBlockId) : QString();
}

QJsonArray DspFlowPanel::workspaceDisplayTargets(const QString &settingsBlockId) const {
    return scene_ ? scene_->workspaceDisplayTargets(settingsBlockId) : QJsonArray();
}

bool DspFlowPanel::bindWorkspaceSettingsBlock(const QString &settingsBlockId, const QString &viewBlockId) {
    return scene_ && scene_->bindWorkspaceSettingsBlock(settingsBlockId, viewBlockId);
}

bool DspFlowPanel::setBlockSettings(const QString &blockId, const QJsonObject &settings) {
    return scene_ && scene_->setBlockSettings(blockId, settings);
}
int DspFlowPanel::workspaceSectorCount() const {
    return scene_ ? scene_->sectorCount() : 0;
}

bool DspFlowPanel::dockSettingsDialog(const QString &blockId, QDialog *dialog, int sectorIndex) {
    return scene_ && scene_->dockSettingsDialog(blockId, dialog, sectorIndex);
}

void DspFlowPanel::rebuildAddMenu() {
    if (addMenu_) {
        addButton_->setMenu(nullptr);
        delete addMenu_;
    }
    addMenu_ = new QMenu(addButton_);
    const QStringList categories = {QStringLiteral("signal_path"), QStringLiteral("filters"),
                                    QStringLiteral("scanning"), QStringLiteral("visualization"),
                                    QStringLiteral("measurement"), QStringLiteral("digital"),
                                    QStringLiteral("navigation"), QStringLiteral("io_tools")};
    QHash<QString, QMenu*> categoryMenus;
    for (const QString &category : categories)
        categoryMenus.insert(category, addMenu_->addMenu(categoryTitle(category, ukrainian_)));
    for (const QString &type : allBlockTypes()) {
        QAction *action = categoryMenus.value(blockCategory(type), addMenu_)->addAction(blockTitle(type));
        action->setData(type);
        connect(action, &QAction::triggered, this, [this, type]() { addBlock(type); });
    }
    addButton_->setMenu(addMenu_);
}

void DspFlowPanel::scheduleConfigurationChanged() {
    if (loading_) return;
    branchPlanDirty_ = true;
    QHash<QString, QString> filters;
    const auto blocks = QJsonDocument::fromJson(configurationJson().toUtf8()).object().value(QStringLiteral("blocks")).toArray();
    for (const auto &value : blocks) {
        const auto block = value.toObject();
        const auto type = block.value(QStringLiteral("type")).toString();
        if (type.startsWith(QStringLiteral("filter_"))) filters.insert(block.value(QStringLiteral("id")).toString(), type);
    }
    for (auto old = knownFilterBlocks_.cbegin(); old != knownFilterBlocks_.cend(); ++old)
        if (!filters.contains(old.key())) emit blockRemoved(old.value(), old.key());
    knownFilterBlocks_ = filters;
    saveTimer_->start();
}

void DspFlowPanel::addBlock(const QString &type) {
    const QPointF center = view_->mapToScene(view_->viewport()->rect().center());
    const qreal offset = qreal(scene_->items().size() % 6) * 18.0;
    const QPointF position = center + QPointF(offset, offset);
    DspBlockItem *block = scene_->addBlock(type, blockTitle(type), position);
    if (workspaceMode_) scene_->prepareNewBlockForWorkspace(block, position);
    scene_->clearSelection();
    block->setSelected(true);
    emit blockCreated(type, block->id());
}

QString DspFlowPanel::blockTitle(const QString &type) const {
    if (type == QStringLiteral("ddc_channelizer")) return ukrainian_ ? QStringLiteral("DDC каналізатор") : QStringLiteral("DDC channelizer");
    if (type == QStringLiteral("branch_fft")) return ukrainian_ ? QStringLiteral("FFT гілки") : QStringLiteral("Branch FFT");
    if (type == QStringLiteral("branch_spectrum")) return ukrainian_ ? QStringLiteral("Спектр гілки") : QStringLiteral("Branch spectrum");
    if (type == QStringLiteral("branch_waterfall")) return ukrainian_ ? QStringLiteral("Водоспад гілки") : QStringLiteral("Branch waterfall");
    if (type == QStringLiteral("run_control")) return ukrainian_ ? QStringLiteral("СТАРТ") : QStringLiteral("START");
    if (type == QStringLiteral("enable_control")) return ukrainian_ ? QStringLiteral("ВКЛ") : QStringLiteral("ON");
    if (type == QStringLiteral("receiver_source")) return ukrainian_ ? QStringLiteral("Вхід приймача") : QStringLiteral("Receiver input");
    if (type == QStringLiteral("iq_source")) return ukrainian_ ? QStringLiteral("Джерело IQ") : QStringLiteral("IQ source");
    if (type == QStringLiteral("network_input")) return ukrainian_ ? QStringLiteral("Мережевий вхід") : QStringLiteral("Network input");
    if (type == QStringLiteral("playback")) return ukrainian_ ? QStringLiteral("Відтворення запису") : QStringLiteral("Recording playback");
    if (type == QStringLiteral("frequency_control")) return ukrainian_ ? QStringLiteral("Частоти") : QStringLiteral("Frequencies");
    if (type == QStringLiteral("fine_tune_control")) return ukrainian_ ? QStringLiteral("Точне налаштування") : QStringLiteral("Fine tune");
    if (type == QStringLiteral("display_scale")) return ukrainian_ ? QStringLiteral("Масштаб і рівні") : QStringLiteral("Scale and levels");
    if (type == QStringLiteral("channel_filter")) return ukrainian_ ? QStringLiteral("Канальний фільтр") : QStringLiteral("Channel filter");
    if (type == QStringLiteral("multi_vfo_channelizer")) return ukrainian_ ? QStringLiteral("Multi-VFO каналайзер") : QStringLiteral("Multi-VFO channelizer");
    if (type == QStringLiteral("vfo_channel")) return ukrainian_ ? QStringLiteral("Канал VFO") : QStringLiteral("VFO channel");
    if (type == QStringLiteral("vfo_spectrum")) return ukrainian_ ? QStringLiteral("Спектр VFO") : QStringLiteral("VFO spectrum");
    if (type == QStringLiteral("vfo_waterfall")) return ukrainian_ ? QStringLiteral("Водоспад VFO") : QStringLiteral("VFO waterfall");
    if (type == QStringLiteral("workspace_spectrum")) return ukrainian_ ? QStringLiteral("Робочий спектр") : QStringLiteral("Workspace spectrum");
    if (type == QStringLiteral("workspace_ruler")) return ukrainian_ ? QStringLiteral("Лінійка частот") : QStringLiteral("Frequency ruler");
    if (type == QStringLiteral("workspace_waterfall")) return ukrainian_ ? QStringLiteral("Робочий водоспад") : QStringLiteral("Workspace waterfall");
    if (type == QStringLiteral("resampler")) return ukrainian_ ? QStringLiteral("Sample rate приймача") : QStringLiteral("Receiver sample rate");
    if (type == QStringLiteral("fft")) return ukrainian_ ? QStringLiteral("FFT / спектр") : QStringLiteral("FFT / spectrum");
    if (type == QStringLiteral("hf_interference")) return ukrainian_ ? QStringLiteral("HF лабораторія завад") : QStringLiteral("HF interference lab");
    if (type == QStringLiteral("demodulator")) return ukrainian_ ? QStringLiteral("Демодулятор") : QStringLiteral("Demodulator");
    if (type == QStringLiteral("audio_filter")) return ukrainian_ ? QStringLiteral("Аудіофільтри") : QStringLiteral("Audio filters");
    if (type.startsWith(QStringLiteral("filter_"))) {
        const QString id = type.mid(7);
        const QHash<QString, QString> uk = {
            {QStringLiteral("low_pass"), QStringLiteral("ФНЧ")}, {QStringLiteral("high_pass"), QStringLiteral("ФВЧ")},
            {QStringLiteral("band_pass"), QStringLiteral("Смуговий фільтр")}, {QStringLiteral("notch"), QStringLiteral("Notch")},
            {QStringLiteral("dc_blocker"), QStringLiteral("DC blocker")}, {QStringLiteral("de_emphasis"), QStringLiteral("FM деемфаза")},
            {QStringLiteral("parametric_eq"), QStringLiteral("Параметричний EQ")}, {QStringLiteral("low_shelf"), QStringLiteral("Low shelf")},
            {QStringLiteral("high_shelf"), QStringLiteral("High shelf")}, {QStringLiteral("adaptive_notch"), QStringLiteral("Адаптивний notch")},
            {QStringLiteral("noise_blanker"), QStringLiteral("Noise blanker")}, {QStringLiteral("cw"), QStringLiteral("CW фільтр")},
            {QStringLiteral("ctcss"), QStringLiteral("CTCSS suppression")}, {QStringLiteral("spectral_denoise"), QStringLiteral("Спектральний denoise")},
            {QStringLiteral("custom_fir"), QStringLiteral("Користувацький FIR")}, {QStringLiteral("gain"), QStringLiteral("Підсилення")},
            {QStringLiteral("compressor"), QStringLiteral("Компресор / AGC")}, {QStringLiteral("limiter"), QStringLiteral("Лімітер")},
            {QStringLiteral("noise_gate"), QStringLiteral("Noise gate")}};
        const QHash<QString, QString> en = {
            {QStringLiteral("low_pass"), QStringLiteral("Low-pass")}, {QStringLiteral("high_pass"), QStringLiteral("High-pass")},
            {QStringLiteral("band_pass"), QStringLiteral("Band-pass")}, {QStringLiteral("notch"), QStringLiteral("Notch")},
            {QStringLiteral("dc_blocker"), QStringLiteral("DC blocker")}, {QStringLiteral("de_emphasis"), QStringLiteral("FM de-emphasis")},
            {QStringLiteral("parametric_eq"), QStringLiteral("Parametric EQ")}, {QStringLiteral("low_shelf"), QStringLiteral("Low shelf")},
            {QStringLiteral("high_shelf"), QStringLiteral("High shelf")}, {QStringLiteral("adaptive_notch"), QStringLiteral("Adaptive notch")},
            {QStringLiteral("noise_blanker"), QStringLiteral("Noise blanker")}, {QStringLiteral("cw"), QStringLiteral("CW filter")},
            {QStringLiteral("ctcss"), QStringLiteral("CTCSS suppression")}, {QStringLiteral("spectral_denoise"), QStringLiteral("Spectral denoise")},
            {QStringLiteral("custom_fir"), QStringLiteral("Custom FIR")}, {QStringLiteral("gain"), QStringLiteral("Gain")},
            {QStringLiteral("compressor"), QStringLiteral("Compressor / AGC")}, {QStringLiteral("limiter"), QStringLiteral("Limiter")},
            {QStringLiteral("noise_gate"), QStringLiteral("Noise gate")}};
        return (ukrainian_ ? uk : en).value(id, id);
    }
    if (type == QStringLiteral("decoder")) return ukrainian_ ? QStringLiteral("Цифровий декодер") : QStringLiteral("Digital decoder");
    if (type == QStringLiteral("digital_audio_settings")) return ukrainian_ ? QStringLiteral("Налаштування цифрового аудіо") : QStringLiteral("Digital audio settings");
    if (type == QStringLiteral("digital_video_settings")) return ukrainian_ ? QStringLiteral("Налаштування цифрового відео") : QStringLiteral("Digital video settings");
    if (type == QStringLiteral("digital_text_output")) return ukrainian_ ? QStringLiteral("Вихід тексту") : QStringLiteral("Text output");
    if (type == QStringLiteral("digital_image_output")) return ukrainian_ ? QStringLiteral("Вихід зображення") : QStringLiteral("Image output");
    if (type == QStringLiteral("spectrum_display")) return ukrainian_ ? QStringLiteral("Налаштування спектра") : QStringLiteral("Spectrum settings");
    if (type == QStringLiteral("waterfall_2d")) return ukrainian_ ? QStringLiteral("Налаштування 2D-водоспаду") : QStringLiteral("2D waterfall settings");
    if (type == QStringLiteral("waterfall_3d")) return ukrainian_ ? QStringLiteral("Налаштування 3D-водоспаду") : QStringLiteral("3D waterfall settings");
    if (type == QStringLiteral("second_spectrum")) return ukrainian_ ? QStringLiteral("Налаштування другого спектра") : QStringLiteral("Second spectrum settings");
    if (type == QStringLiteral("agile_scan")) return QStringLiteral("Agile scan");
    if (type == QStringLiteral("standard_scan")) return ukrainian_ ? QStringLiteral("Стандартний скан") : QStringLiteral("Standard scan");
    if (type == QStringLiteral("listening_scan")) return ukrainian_ ? QStringLiteral("Скан прослуховування") : QStringLiteral("Listening scan");
    if (type == QStringLiteral("spectrum_measurement")) return ukrainian_ ? QStringLiteral("Вимірювання спектра") : QStringLiteral("Spectrum measurement");
    if (type == QStringLiteral("zoom_spectrum")) return ukrainian_ ? QStringLiteral("Вузькосмуговий аналіз") : QStringLiteral("Zoom spectrum");
    if (type == QStringLiteral("zoom_density")) return ukrainian_ ? QStringLiteral("Щільність виділеної смуги") : QStringLiteral("Selected-band density");
    if (type == QStringLiteral("zero_span")) return QStringLiteral("Zero-span");
    if (type == QStringLiteral("research_analysis")) return ukrainian_ ? QStringLiteral("Дослідницький аналіз") : QStringLiteral("Research analysis");
    if (type == QStringLiteral("wireless_lab")) return ukrainian_ ? QStringLiteral("Бездротова лабораторія (бета)") : QStringLiteral("Wireless lab (beta)");
    if (type == QStringLiteral("radio_propagation")) return ukrainian_ ? QStringLiteral("Радіопоширення / КХ") : QStringLiteral("Radio propagation / HF");
    if (type == QStringLiteral("research_interference")) return ukrainian_ ? QStringLiteral("Гребінки та завади") : QStringLiteral("Interference and combs");
    if (type == QStringLiteral("research_statistics")) return ukrainian_ ? QStringLiteral("Статистика сигналу") : QStringLiteral("Signal statistics");
    if (type == QStringLiteral("research_iq")) return ukrainian_ ? QStringLiteral("Контроль IQ") : QStringLiteral("IQ diagnostics");
    if (type == QStringLiteral("research_dual_input")) return ukrainian_ ? QStringLiteral("Двоканальний HF аналіз") : QStringLiteral("Dual-input HF analysis");
    if (type == QStringLiteral("research_analyzer")) return ukrainian_ ? QStringLiteral("Науковий аналізатор") : QStringLiteral("Scientific analyzer");
    if (type == QStringLiteral("research_density")) return ukrainian_ ? QStringLiteral("Щільність сигналу") : QStringLiteral("Signal density");
    if (type == QStringLiteral("research_masks")) return ukrainian_ ? QStringLiteral("Маски та baseline") : QStringLiteral("Masks and baseline");
    if (type == QStringLiteral("research_pulse")) return ukrainian_ ? QStringLiteral("Імпульсний аналіз") : QStringLiteral("Pulse analysis");
    if (type == QStringLiteral("research_session")) return ukrainian_ ? QStringLiteral("Сесія вимірювань") : QStringLiteral("Measurement session");
    if (type == QStringLiteral("oscilloscope")) return ukrainian_ ? QStringLiteral("Осцилограф: параметри") : QStringLiteral("Oscilloscope settings");
    if (type == QStringLiteral("constellation")) return ukrainian_ ? QStringLiteral("Сузір'я: параметри") : QStringLiteral("Constellation settings");
    if (type == QStringLiteral("eye_diagram")) return ukrainian_ ? QStringLiteral("Окова діаграма: параметри") : QStringLiteral("Eye diagram settings");
    if (type == QStringLiteral("digital_sync_lab")) return ukrainian_ ? QStringLiteral("Синхронізація: параметри") : QStringLiteral("Synchronization settings");
    if (type == QStringLiteral("oscilloscope_view")) return ukrainian_ ? QStringLiteral("Осцилограф: графік") : QStringLiteral("Oscilloscope plot");
    if (type == QStringLiteral("constellation_view")) return ukrainian_ ? QStringLiteral("Сузір'я: графік") : QStringLiteral("Constellation plot");
    if (type == QStringLiteral("eye_diagram_view")) return ukrainian_ ? QStringLiteral("Окова діаграма: графік") : QStringLiteral("Eye diagram plot");
    if (type == QStringLiteral("digital_sync_view")) return ukrainian_ ? QStringLiteral("Синхронізація: графік") : QStringLiteral("Synchronization plot");
    if (type == QStringLiteral("spur_suppression")) return ukrainian_ ? QStringLiteral("Придушення spur") : QStringLiteral("Spur suppression");
    if (type == QStringLiteral("spectrum_recorder")) return ukrainian_ ? QStringLiteral("Запис кадрів спектра") : QStringLiteral("Spectrum frame recorder");
    if (type == QStringLiteral("spectrum_replay")) return ukrainian_ ? QStringLiteral("Реплей спектра / IQ") : QStringLiteral("Spectrum / IQ replay");
    if (type == QStringLiteral("dmr_decoder")) return QStringLiteral("DMR");
    if (type == QStringLiteral("cw_decoder")) return ukrainian_ ? QStringLiteral("Декодер CW") : QStringLiteral("CW decoder");
    if (type == QStringLiteral("sstv_decoder")) return QStringLiteral("SSTV");
    if (type == QStringLiteral("digital_video")) return ukrainian_ ? QStringLiteral("Цифрове відео") : QStringLiteral("Digital video");
    if (type == QStringLiteral("dmr_hunter")) return QStringLiteral("DMR Hunter");
    if (type == QStringLiteral("fpv_hunter")) return QStringLiteral("FPV Hunter");
    if (type == QStringLiteral("digital_video_hunter")) return QStringLiteral("Digital Video Hunter");
    if (type == QStringLiteral("gnss_sdr")) return ukrainian_ ? QStringLiteral("GNSS через SDR") : QStringLiteral("GNSS via SDR");
    if (type == QStringLiteral("gnss_serial")) return ukrainian_ ? QStringLiteral("GNSS модуль / NMEA / UBX") : QStringLiteral("GNSS module / NMEA / UBX");
    if (type == QStringLiteral("qth_map")) return ukrainian_ ? QStringLiteral("Карта QTH") : QStringLiteral("QTH map");
    if (type == QStringLiteral("gpio")) return QStringLiteral("GPIO");
    if (type == QStringLiteral("recorder")) return ukrainian_ ? QStringLiteral("Записувач") : QStringLiteral("Recorder");
    if (type == QStringLiteral("network_output")) return ukrainian_ ? QStringLiteral("Мережевий вихід") : QStringLiteral("Network output");
    if (type == QStringLiteral("audio_output")) return ukrainian_ ? QStringLiteral("Аудіовихід") : QStringLiteral("Audio output");
    if (type == QStringLiteral("transmitter")) return ukrainian_ ? QStringLiteral("Передавач") : QStringLiteral("Transmitter");
    if (type == QStringLiteral("presets")) return ukrainian_ ? QStringLiteral("Пресети й калібрування") : QStringLiteral("Presets and calibration");
    if (type == QStringLiteral("calibration")) return ukrainian_ ? QStringLiteral("Калібрування приймача") : QStringLiteral("Receiver calibration");
    if (type == QStringLiteral("application_settings")) return ukrainian_ ? QStringLiteral("Загальні налаштування") : QStringLiteral("Application settings");
    return type;
}

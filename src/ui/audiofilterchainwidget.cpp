#include "audiofilterchainwidget.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHeaderView>
#include <QLabel>
#include <QLineF>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScreen>
#include <QSet>
#include <QSignalBlocker>
#include <QStyle>
#include <QTableWidget>
#include <QToolButton>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>

namespace {
constexpr double AUDIO_SAMPLE_RATE = 48000.0;
constexpr double PI = 3.14159265358979323846;
constexpr double GRAPH_MIN_HZ = 20.0;
constexpr double GRAPH_MAX_HZ = 20000.0;
constexpr double GRAPH_MIN_DB = -60.0;
constexpr double GRAPH_MAX_DB = 24.0;

bool hasFrequencyResponse(AudioFilterKind kind) {
    return kind == AudioFilterKind::LowPass ||
           kind == AudioFilterKind::HighPass ||
           kind == AudioFilterKind::BandPass ||
           kind == AudioFilterKind::Notch ||
           kind == AudioFilterKind::DcBlocker ||
           kind == AudioFilterKind::Gain ||
           kind == AudioFilterKind::DeEmphasis ||
           kind == AudioFilterKind::ParametricEq ||
           kind == AudioFilterKind::LowShelf ||
           kind == AudioFilterKind::HighShelf ||
           kind == AudioFilterKind::CwFilter ||
           kind == AudioFilterKind::CtcssSuppressor ||
           kind == AudioFilterKind::CustomFir;
}

bool hasAdjustableQ(AudioFilterKind kind) {
    return kind == AudioFilterKind::LowPass ||
           kind == AudioFilterKind::HighPass ||
           kind == AudioFilterKind::BandPass ||
           kind == AudioFilterKind::Notch ||
           kind == AudioFilterKind::DcBlocker ||
           kind == AudioFilterKind::ParametricEq ||
           kind == AudioFilterKind::LowShelf ||
           kind == AudioFilterKind::HighShelf ||
           kind == AudioFilterKind::CwFilter ||
           kind == AudioFilterKind::CtcssSuppressor;
}

QVector<AudioFilterKind> availableFilterKinds() {
    return {
        AudioFilterKind::LowPass,
        AudioFilterKind::HighPass,
        AudioFilterKind::BandPass,
        AudioFilterKind::Notch,
        AudioFilterKind::DcBlocker,
        AudioFilterKind::DeEmphasis,
        AudioFilterKind::ParametricEq,
        AudioFilterKind::LowShelf,
        AudioFilterKind::HighShelf,
        AudioFilterKind::AdaptiveNotch,
        AudioFilterKind::NoiseBlanker,
        AudioFilterKind::CwFilter,
        AudioFilterKind::CtcssSuppressor,
        AudioFilterKind::SpectralDenoise,
        AudioFilterKind::CustomFir,
        AudioFilterKind::Gain,
        AudioFilterKind::Compressor,
        AudioFilterKind::Limiter,
        AudioFilterKind::NoiseGate
    };
}

class AudioFilterResponseWidget : public QWidget {
public:
    explicit AudioFilterResponseWidget(QWidget *parent = nullptr)
        : QWidget(parent) {
        setMinimumSize(360, 180);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setCursor(Qt::CrossCursor);
    }

    void setStage(const AudioFilterStage &value) {
        stage = value;
        update();
    }

    std::function<void(double, bool)> frequencyDragged;
    std::function<void(double)> qDragged;
    std::function<void(double)> gainDragged;
    std::function<void(double)> timeConstantDragged;
    std::function<void(int, double, double, bool)> curvePointEdited;
    std::function<void(int)> curvePointRemoved;

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.fillRect(rect(), palette().color(QPalette::Base));

        const QRectF plot = rect().adjusted(42, 10, -10, -28);
        painter.setPen(QPen(palette().color(QPalette::Mid), 1));
        const double frequencyTicks[] = {20.0, 50.0, 100.0, 200.0, 500.0,
                                         1000.0, 2000.0, 5000.0, 10000.0, 20000.0};
        for (double frequency : frequencyTicks) {
            const double x = frequencyToX(frequency, plot);
            painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
            if (frequency == 20.0 || frequency == 100.0 || frequency == 1000.0 ||
                frequency == 10000.0 || frequency == 20000.0) {
                const QString label = frequency >= 1000.0
                                          ? QStringLiteral("%1k").arg(frequency / 1000.0, 0, 'g', 3)
                                          : QString::number(static_cast<int>(frequency));
                painter.drawText(QRectF(x - 24.0, plot.bottom() + 4.0, 48.0, 18.0),
                                 Qt::AlignHCenter | Qt::AlignTop,
                                 label);
            }
        }
        for (double db : {24.0, 12.0, 0.0, -12.0, -24.0, -36.0, -48.0, -60.0}) {
            const double y = dbToY(db, plot);
            painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
            painter.drawText(QRectF(0.0, y - 9.0, 38.0, 18.0),
                             Qt::AlignRight | Qt::AlignVCenter,
                             QString::number(static_cast<int>(db)));
        }

        QPainterPath response;
        constexpr int pointCount = 420;
        for (int point = 0; point < pointCount; ++point) {
            const double ratio = static_cast<double>(point) / (pointCount - 1);
            const double frequency = GRAPH_MIN_HZ *
                std::pow(GRAPH_MAX_HZ / GRAPH_MIN_HZ, ratio);
            const double db = (std::clamp)(audioFilterMagnitudeDb(stage,
                                                                   frequency,
                                                                   AUDIO_SAMPLE_RATE),
                                            GRAPH_MIN_DB,
                                            GRAPH_MAX_DB);
            const QPointF sample(plot.left() + ratio * plot.width(), dbToY(db, plot));
            if (point == 0) response.moveTo(sample);
            else response.lineTo(sample);
        }
        painter.setPen(QPen(QColor(44, 166, 235), 2));
        painter.drawPath(response);

        if (stage.kind == AudioFilterKind::CustomFir) {
            painter.setBrush(QColor(44, 166, 235));
            painter.setPen(QPen(palette().color(QPalette::Text), 1));
            for (const AudioFilterPoint &point : stage.curve) {
                painter.drawEllipse(QPointF(frequencyToX(point.frequencyHz, plot),
                                            dbToY(point.gainDb, plot)),
                                    4.5,
                                    4.5);
            }
        }

        painter.setPen(QPen(QColor(238, 91, 73), 1, Qt::DashLine));
        if (stage.kind == AudioFilterKind::LowPass ||
            stage.kind == AudioFilterKind::HighPass ||
            stage.kind == AudioFilterKind::Notch ||
            stage.kind == AudioFilterKind::DcBlocker ||
            stage.kind == AudioFilterKind::BandPass ||
            stage.kind == AudioFilterKind::ParametricEq ||
            stage.kind == AudioFilterKind::LowShelf ||
            stage.kind == AudioFilterKind::HighShelf ||
            stage.kind == AudioFilterKind::CwFilter ||
            stage.kind == AudioFilterKind::CtcssSuppressor) {
            const double x = frequencyToX(stage.frequencyHz, plot);
            painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        }
        if (stage.kind == AudioFilterKind::DeEmphasis) {
            const double cutoff = 1.0 / (2.0 * PI * stage.timeConstantUs * 1.0e-6);
            const double x = frequencyToX(cutoff, plot);
            painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        }
        if (stage.kind == AudioFilterKind::BandPass) {
            const double x = frequencyToX(stage.frequency2Hz, plot);
            painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        }

        painter.setPen(palette().color(QPalette::Text));
        painter.drawText(QRectF(0.0, plot.top(), 36.0, plot.height()),
                         Qt::AlignCenter,
                         QStringLiteral("dB"));
        painter.drawText(QRectF(plot.left(), plot.bottom() + 4.0, plot.width(), 20.0),
                         Qt::AlignRight | Qt::AlignVCenter,
                         QStringLiteral("Hz"));
        if (hasAdjustableQ(stage.kind)) {
            painter.drawText(QRectF(plot.left() + 6.0, plot.top() + 4.0,
                                    plot.width() - 12.0, 20.0),
                             Qt::AlignRight | Qt::AlignTop,
                             QStringLiteral("Q %1").arg(stage.q, 0, 'f', 3));
        }
        if (stage.kind == AudioFilterKind::Gain ||
            stage.kind == AudioFilterKind::ParametricEq ||
            stage.kind == AudioFilterKind::LowShelf ||
            stage.kind == AudioFilterKind::HighShelf) {
            painter.drawText(QRectF(plot.left() + 6.0, plot.top() + 24.0,
                                    plot.width() - 12.0, 20.0),
                             Qt::AlignRight | Qt::AlignTop,
                             QStringLiteral("Gain %1 dB").arg(stage.gainDb, 0, 'f', 1));
        }
        if (stage.kind == AudioFilterKind::DeEmphasis) {
            const double cutoff = 1.0 / (2.0 * PI * stage.timeConstantUs * 1.0e-6);
            painter.drawText(QRectF(plot.left() + 6.0, plot.top() + 4.0,
                                    plot.width() - 12.0, 20.0),
                             Qt::AlignRight | Qt::AlignTop,
                             QStringLiteral("%1 us / %2 Hz")
                                 .arg(stage.timeConstantUs, 0, 'f', 1)
                                 .arg(cutoff, 0, 'f', 0));
        }
    }

    void mousePressEvent(QMouseEvent *event) override {
        if (event->button() == Qt::RightButton && stage.kind == AudioFilterKind::CustomFir) {
            const int index = nearestCurvePoint(event->pos());
            if (index >= 0 && curvePointRemoved && stage.curve.size() > 2) {
                curvePointRemoved(index);
            }
            event->accept();
            return;
        }
        if (event->button() == Qt::RightButton && hasAdjustableQ(stage.kind)) {
            rightDragging = true;
            rightDragStartY = event->pos().y();
            rightDragStartQ = stage.q;
            event->accept();
            return;
        }
        if (event->button() == Qt::LeftButton) {
            leftDragging = true;
            leftDragStartY = event->pos().y();
            leftDragStartGain = stage.gainDb;
            if (stage.kind == AudioFilterKind::CustomFir) {
                curveDragIndex = nearestCurvePoint(event->pos());
            }
            dragLeft(event->pos());
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override {
        if (rightDragging && (event->buttons() & Qt::RightButton)) {
            const double octaves = static_cast<double>(rightDragStartY - event->pos().y()) / 60.0;
            const double value = (std::clamp)(rightDragStartQ * std::pow(2.0, octaves),
                                               0.1,
                                               30.0);
            if (qDragged) {
                qDragged(value);
            }
            event->accept();
            return;
        }
        if (event->buttons() & Qt::LeftButton) {
            dragLeft(event->pos());
            event->accept();
            return;
        }
        QWidget::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override {
        if (event->button() == Qt::RightButton && rightDragging) {
            rightDragging = false;
            event->accept();
            return;
        }
        if (event->button() == Qt::LeftButton && leftDragging) {
            leftDragging = false;
            event->accept();
            return;
        }
        QWidget::mouseReleaseEvent(event);
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton &&
            stage.kind == AudioFilterKind::CustomFir && curvePointEdited) {
            const QRectF plot = rect().adjusted(42, 10, -10, -28);
            curvePointEdited(-1,
                             xToFrequency(event->pos().x(), plot),
                             yToCurveGain(event->pos().y(), plot),
                             true);
            event->accept();
            return;
        }
        QWidget::mouseDoubleClickEvent(event);
    }

private:
    static double frequencyToX(double frequency, const QRectF &plot) {
        const double ratio = std::log((std::clamp)(frequency, GRAPH_MIN_HZ, GRAPH_MAX_HZ) /
                                      GRAPH_MIN_HZ) /
                             std::log(GRAPH_MAX_HZ / GRAPH_MIN_HZ);
        return plot.left() + ratio * plot.width();
    }

    static double xToFrequency(double x, const QRectF &plot) {
        const double ratio = (std::clamp)((x - plot.left()) / plot.width(), 0.0, 1.0);
        return GRAPH_MIN_HZ * std::pow(GRAPH_MAX_HZ / GRAPH_MIN_HZ, ratio);
    }

    static double dbToY(double db, const QRectF &plot) {
        const double ratio = (GRAPH_MAX_DB - db) / (GRAPH_MAX_DB - GRAPH_MIN_DB);
        return plot.top() + ratio * plot.height();
    }

    static double yToCurveGain(double y, const QRectF &plot) {
        const double ratio = (std::clamp)((y - plot.top()) / plot.height(), 0.0, 1.0);
        return 24.0 - ratio * 48.0;
    }

    void dragFrequency(const QPoint &position) {
        if (!frequencyDragged ||
            (stage.kind != AudioFilterKind::LowPass &&
             stage.kind != AudioFilterKind::HighPass &&
             stage.kind != AudioFilterKind::BandPass &&
             stage.kind != AudioFilterKind::Notch &&
             stage.kind != AudioFilterKind::DcBlocker &&
             stage.kind != AudioFilterKind::ParametricEq &&
             stage.kind != AudioFilterKind::LowShelf &&
             stage.kind != AudioFilterKind::HighShelf &&
             stage.kind != AudioFilterKind::CwFilter &&
             stage.kind != AudioFilterKind::CtcssSuppressor)) {
            return;
        }
        const QRectF plot = rect().adjusted(42, 10, -10, -28);
        const double frequency = xToFrequency(position.x(), plot);
        bool secondary = false;
        if (stage.kind == AudioFilterKind::BandPass) {
            const double firstDistance = std::abs(std::log(frequency / stage.frequencyHz));
            const double secondDistance = std::abs(std::log(frequency / stage.frequency2Hz));
            secondary = secondDistance < firstDistance;
        }
        frequencyDragged(frequency, secondary);
    }

    void dragLeft(const QPoint &position) {
        if (stage.kind == AudioFilterKind::CustomFir) {
            if (curveDragIndex >= 0 && curvePointEdited) {
                const QRectF plot = rect().adjusted(42, 10, -10, -28);
                curvePointEdited(curveDragIndex,
                                 xToFrequency(position.x(), plot),
                                 yToCurveGain(position.y(), plot),
                                 false);
            }
            return;
        }
        if (stage.kind == AudioFilterKind::Gain) {
            dragGain(position);
            return;
        }
        if (stage.kind == AudioFilterKind::DeEmphasis) {
            if (!timeConstantDragged) {
                return;
            }
            const QRectF plot = rect().adjusted(42, 10, -10, -28);
            const double cutoff = xToFrequency(position.x(), plot);
            const double timeConstantUs = 1.0e6 / (2.0 * PI * cutoff);
            timeConstantDragged((std::clamp)(timeConstantUs, 10.0, 2000.0));
            return;
        }
        dragFrequency(position);
        if (stage.kind == AudioFilterKind::ParametricEq ||
            stage.kind == AudioFilterKind::LowShelf ||
            stage.kind == AudioFilterKind::HighShelf) {
            dragGain(position);
        }
    }

    void dragGain(const QPoint &position) {
        if (!gainDragged) {
            return;
        }
        const double gain = leftDragStartGain +
                            static_cast<double>(leftDragStartY - position.y()) / 4.0;
        gainDragged((std::clamp)(gain, -36.0, 36.0));
    }

    int nearestCurvePoint(const QPoint &position) const {
        if (stage.curve.isEmpty()) {
            return -1;
        }
        const QRectF plot = rect().adjusted(42, 10, -10, -28);
        int bestIndex = -1;
        double bestDistance = 14.0;
        for (int index = 0; index < stage.curve.size(); ++index) {
            const AudioFilterPoint &point = stage.curve.at(index);
            const QPointF screenPoint(frequencyToX(point.frequencyHz, plot),
                                      dbToY(point.gainDb, plot));
            const double distance = QLineF(screenPoint, QPointF(position)).length();
            if (distance < bestDistance) {
                bestDistance = distance;
                bestIndex = index;
            }
        }
        return bestIndex;
    }

    AudioFilterStage stage;
    bool rightDragging = false;
    bool leftDragging = false;
    int rightDragStartY = 0;
    int leftDragStartY = 0;
    double rightDragStartQ = 0.707;
    double leftDragStartGain = 0.0;
    int curveDragIndex = -1;
};

class AudioFilterEditorDialog : public QDialog {
public:
    AudioFilterEditorDialog(const AudioFilterStage &source,
                            bool ukrainian,
                            QWidget *parent = nullptr)
        : QDialog(parent), stage(source), ukrainianLanguage(ukrainian) {
        setModal(false);
        setAttribute(Qt::WA_DeleteOnClose);
        setWindowFlags((windowFlags() | Qt::WindowStaysOnTopHint) &
                       ~Qt::WindowContextHelpButtonHint);
        setWindowTitle(ukrainian ? QStringLiteral("Налаштування фільтра")
                                 : QStringLiteral("Filter settings"));

        nameEdit = new QLineEdit(stage.customName, this);
        frequencySpin = makeFrequencySpin(stage.frequencyHz);
        frequency2Spin = makeFrequencySpin(stage.frequency2Hz);
        qSpin = new QDoubleSpinBox(this);
        qSpin->setRange(0.1, 30.0);
        qSpin->setDecimals(3);
        qSpin->setSingleStep(0.1);
        qSpin->setValue(stage.q);
        gainSpin = new QDoubleSpinBox(this);
        gainSpin->setRange(-36.0, 36.0);
        gainSpin->setDecimals(1);
        gainSpin->setSuffix(QStringLiteral(" dB"));
        gainSpin->setValue(stage.gainDb);
        thresholdSpin = new QDoubleSpinBox(this);
        thresholdSpin->setRange(-80.0, 0.0);
        thresholdSpin->setDecimals(1);
        thresholdSpin->setSuffix(QStringLiteral(" dB"));
        thresholdSpin->setValue(stage.thresholdDb);
        releaseSpin = new QDoubleSpinBox(this);
        releaseSpin->setRange(1.0, 5000.0);
        releaseSpin->setDecimals(0);
        releaseSpin->setSuffix(QStringLiteral(" ms"));
        releaseSpin->setValue(stage.releaseMs);
        attackSpin = new QDoubleSpinBox(this);
        attackSpin->setRange(0.1, 2000.0);
        attackSpin->setDecimals(1);
        attackSpin->setSuffix(QStringLiteral(" ms"));
        attackSpin->setValue(stage.attackMs);
        ratioSpin = new QDoubleSpinBox(this);
        ratioSpin->setRange(1.0, 100.0);
        ratioSpin->setDecimals(1);
        ratioSpin->setSingleStep(0.5);
        ratioSpin->setSuffix(QStringLiteral(":1"));
        ratioSpin->setValue(stage.ratio);
        timeConstantSpin = new QDoubleSpinBox(this);
        timeConstantSpin->setRange(10.0, 2000.0);
        timeConstantSpin->setDecimals(1);
        timeConstantSpin->setSuffix(QStringLiteral(" us"));
        timeConstantSpin->setValue(stage.timeConstantUs);
        bandwidthSpin = makeFrequencySpin(stage.bandwidthHz);
        bandwidthSpin->setRange(10.0, 10000.0);
        bandwidthSpin->setValue(stage.bandwidthHz);
        amountSpin = new QDoubleSpinBox(this);
        amountSpin->setRange(0.0, 100.0);
        amountSpin->setDecimals(0);
        amountSpin->setSuffix(QStringLiteral(" %"));
        amountSpin->setValue(stage.amount * 100.0);

        QFormLayout *form = new QFormLayout();
        form->addRow(ukrainian ? QStringLiteral("Тип:") : QStringLiteral("Type:"),
                     new QLabel(audioFilterKindName(stage.kind, ukrainian), this));
        form->addRow(ukrainian ? QStringLiteral("Назва:") : QStringLiteral("Name:"), nameEdit);
        frequencyLabel = new QLabel(this);
        frequency2Label = new QLabel(this);
        qLabel = new QLabel(this);
        gainLabel = new QLabel(this);
        thresholdLabel = new QLabel(this);
        ratioLabel = new QLabel(this);
        attackLabel = new QLabel(this);
        releaseLabel = new QLabel(this);
        timeConstantLabel = new QLabel(this);
        bandwidthLabel = new QLabel(this);
        amountLabel = new QLabel(this);
        form->addRow(frequencyLabel, frequencySpin);
        form->addRow(frequency2Label, frequency2Spin);
        form->addRow(qLabel, qSpin);
        form->addRow(gainLabel, gainSpin);
        form->addRow(thresholdLabel, thresholdSpin);
        form->addRow(ratioLabel, ratioSpin);
        form->addRow(attackLabel, attackSpin);
        form->addRow(releaseLabel, releaseSpin);
        form->addRow(timeConstantLabel, timeConstantSpin);
        form->addRow(bandwidthLabel, bandwidthSpin);
        form->addRow(amountLabel, amountSpin);

        responseWidget = new AudioFilterResponseWidget(this);
        responseWidget->setStage(stage);
        responseWidget->setVisible(hasFrequencyResponse(stage.kind));
        responseWidget->frequencyDragged = [this](double frequency, bool secondary) {
            QDoubleSpinBox *target = secondary ? frequency2Spin : frequencySpin;
            target->setValue(frequency);
        };
        responseWidget->qDragged = [this](double q) {
            if (stage.kind == AudioFilterKind::CwFilter) {
                const QSignalBlocker qBlocker(qSpin);
                qSpin->setValue(q);
                bandwidthSpin->setValue(frequencySpin->value() / q);
            } else {
                qSpin->setValue(q);
            }
        };
        responseWidget->gainDragged = [this](double gainDb) {
            gainSpin->setValue(gainDb);
        };
        responseWidget->timeConstantDragged = [this](double timeConstantUs) {
            timeConstantSpin->setValue(timeConstantUs);
        };
        responseWidget->curvePointEdited = [this](int index,
                                                   double frequency,
                                                   double gain,
                                                   bool add) {
            if (add) {
                if (stage.curve.size() >= 32) {
                    return;
                }
                stage.curve.push_back({frequency, gain});
                std::sort(stage.curve.begin(), stage.curve.end(),
                          [](const AudioFilterPoint &a, const AudioFilterPoint &b) {
                              return a.frequencyHz < b.frequencyHz;
                          });
            } else if (index >= 0 && index < stage.curve.size()) {
                const double minimum = index > 0
                                           ? stage.curve.at(index - 1).frequencyHz * 1.01
                                           : GRAPH_MIN_HZ;
                const double maximum = index + 1 < stage.curve.size()
                                           ? stage.curve.at(index + 1).frequencyHz / 1.01
                                           : GRAPH_MAX_HZ;
                stage.curve[index].frequencyHz = (std::clamp)(frequency, minimum, maximum);
                stage.curve[index].gainDb = (std::clamp)(gain, -24.0, 24.0);
            }
            responseWidget->setStage(stage);
            if (stageChanged) {
                stageChanged(result());
            }
        };
        responseWidget->curvePointRemoved = [this](int index) {
            if (index < 0 || index >= stage.curve.size() || stage.curve.size() <= 2) {
                return;
            }
            stage.curve.removeAt(index);
            responseWidget->setStage(stage);
            if (stageChanged) {
                stageChanged(result());
            }
        };
        QString graphToolTip = ukrainian
            ? QStringLiteral("Ліва кнопка: частота. Права кнопка вгору/вниз: Q.")
            : QStringLiteral("Left button: frequency. Right-drag up/down: Q.");
        if (stage.kind == AudioFilterKind::Gain) {
            graphToolTip = ukrainian
                ? QStringLiteral("Тягніть лівою кнопкою вгору/вниз, щоб змінити підсилення.")
                : QStringLiteral("Left-drag up/down to change gain.");
        } else if (stage.kind == AudioFilterKind::DeEmphasis) {
            graphToolTip = ukrainian
                ? QStringLiteral("Тягніть лівою кнопкою по горизонталі: частота зламу автоматично перераховується у сталу часу.")
                : QStringLiteral("Left-drag horizontally: the corner frequency is converted to the time constant.");
        } else if (stage.kind == AudioFilterKind::ParametricEq ||
                   stage.kind == AudioFilterKind::LowShelf ||
                   stage.kind == AudioFilterKind::HighShelf) {
            graphToolTip = ukrainian
                ? QStringLiteral("Ліва кнопка: горизонталь задає частоту, вертикаль - gain. Права кнопка вгору/вниз: Q.")
                : QStringLiteral("Left-drag: horizontal sets frequency, vertical sets gain. Right-drag up/down: Q.");
        } else if (stage.kind == AudioFilterKind::CwFilter) {
            graphToolTip = ukrainian
                ? QStringLiteral("Ліва кнопка: тон CW. Права кнопка вгору/вниз: Q і ширина смуги.")
                : QStringLiteral("Left-drag: CW tone. Right-drag up/down: Q and bandwidth.");
        } else if (stage.kind == AudioFilterKind::CustomFir) {
            graphToolTip = ukrainian
                ? QStringLiteral("Тягніть точки лівою кнопкою. Подвійний клік додає точку, правий клік по точці видаляє її.")
                : QStringLiteral("Left-drag points. Double-click adds a point; right-click a point to remove it.");
        }
        responseWidget->setToolTip(graphToolTip);

        QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
        QPushButton *resetButton = buttons->addButton(
            ukrainian ? QStringLiteral("Скинути") : QStringLiteral("Reset"),
            QDialogButtonBox::ResetRole);
        buttons->button(QDialogButtonBox::Close)->setText(ukrainian ? QStringLiteral("Закрити")
                                                                     : QStringLiteral("Close"));
        resetButton->setToolTip(ukrainian
            ? QStringLiteral("Повернути типові параметри цього фільтра")
            : QStringLiteral("Restore this filter type's default parameters"));
        connect(resetButton, &QPushButton::clicked,
                this, [this]() { resetParameters(); });
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);

        auto *stayOnTop = new QCheckBox(
            ukrainian ? QStringLiteral("Залишатися поверх вікон") : QStringLiteral("Stay on top"), this);
        stayOnTop->setChecked(true);
        connect(stayOnTop, &QCheckBox::toggled, this, [this](bool enabled) {
            setWindowFlag(Qt::WindowStaysOnTopHint, enabled);
            show();
            raise();
            activateWindow();
        });

        QVBoxLayout *layout = new QVBoxLayout(this);
        auto *enabled = new QCheckBox(ukrainian ? QStringLiteral("Увімкнути фільтр") : QStringLiteral("Enable filter"), this);
        enabled->setObjectName(QStringLiteral("audioFilterEnabled"));
        enabled->setChecked(stage.enabled);
        connect(enabled, &QCheckBox::toggled, this, [this](bool value) { if (enabledChanged) enabledChanged(value); });
        layout->addWidget(enabled);
        layout->addLayout(form);
        layout->addWidget(responseWidget, 1);
        layout->addWidget(stayOnTop);
        layout->addWidget(buttons);

        connect(frequencySpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [this](double frequency) {
                    if (stage.kind == AudioFilterKind::CwFilter) {
                        const QSignalBlocker blocker(qSpin);
                        qSpin->setValue(frequency / bandwidthSpin->value());
                    }
                    notifyStageChanged();
                });
        connect(frequency2Spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [this](double) { notifyStageChanged(); });
        connect(qSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [this](double) { notifyStageChanged(); });
        connect(gainSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [this](double) { notifyStageChanged(); });
        connect(thresholdSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [this](double) { notifyStageChanged(); });
        connect(releaseSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [this](double) { notifyStageChanged(); });
        connect(attackSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [this](double) { notifyStageChanged(); });
        connect(ratioSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [this](double) { notifyStageChanged(); });
        connect(timeConstantSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [this](double) { notifyStageChanged(); });
        connect(bandwidthSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [this](double bandwidth) {
                    if (stage.kind == AudioFilterKind::CwFilter) {
                        const QSignalBlocker blocker(qSpin);
                        qSpin->setValue(frequencySpin->value() / bandwidth);
                    }
                    notifyStageChanged();
                });
        connect(amountSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [this](double) { notifyStageChanged(); });
        connect(nameEdit, &QLineEdit::textEdited,
                this, [this](const QString &) { notifyStageChanged(); });

        updateVisibility();
        if (hasFrequencyResponse(stage.kind)) {
            resize(570, 470);
        } else {
            adjustSize();
            resize(390, sizeHint().height());
        }
    }

    std::function<void(const AudioFilterStage &)> stageChanged;
    std::function<void(bool)> enabledChanged;

    AudioFilterStage result() const {
        AudioFilterStage value = stage;
        value.customName = nameEdit->text().trimmed().left(80);
        value.frequencyHz = frequencySpin->value();
        value.frequency2Hz = frequency2Spin->value();
        value.q = qSpin->value();
        value.gainDb = gainSpin->value();
        value.thresholdDb = thresholdSpin->value();
        value.ratio = ratioSpin->value();
        value.attackMs = attackSpin->value();
        value.releaseMs = releaseSpin->value();
        value.timeConstantUs = timeConstantSpin->value();
        value.bandwidthHz = bandwidthSpin->value();
        value.amount = amountSpin->value() / 100.0;
        value.curve = stage.curve;
        if (value.kind == AudioFilterKind::CwFilter) {
            value.q = value.frequencyHz / value.bandwidthHz;
        }
        if (value.kind == AudioFilterKind::BandPass && value.frequencyHz > value.frequency2Hz) {
            std::swap(value.frequencyHz, value.frequency2Hz);
        }
        return value;
    }

private:
    QDoubleSpinBox *makeFrequencySpin(double value) {
        QDoubleSpinBox *spin = new QDoubleSpinBox(this);
        spin->setRange(GRAPH_MIN_HZ, GRAPH_MAX_HZ);
        spin->setDecimals(1);
        spin->setSingleStep(10.0);
        spin->setSuffix(QStringLiteral(" Hz"));
        spin->setValue(value);
        return spin;
    }

    void updateVisibility() {
        const bool firstFrequency = stage.kind == AudioFilterKind::LowPass ||
                                    stage.kind == AudioFilterKind::HighPass ||
                                    stage.kind == AudioFilterKind::BandPass ||
                                    stage.kind == AudioFilterKind::Notch ||
                                    stage.kind == AudioFilterKind::DcBlocker ||
                                    stage.kind == AudioFilterKind::ParametricEq ||
                                    stage.kind == AudioFilterKind::LowShelf ||
                                    stage.kind == AudioFilterKind::HighShelf ||
                                    stage.kind == AudioFilterKind::AdaptiveNotch ||
                                    stage.kind == AudioFilterKind::CwFilter ||
                                    stage.kind == AudioFilterKind::CtcssSuppressor;
        const bool secondFrequency = stage.kind == AudioFilterKind::BandPass ||
                                     stage.kind == AudioFilterKind::AdaptiveNotch;
        const bool quality = stage.kind == AudioFilterKind::LowPass ||
                             stage.kind == AudioFilterKind::HighPass ||
                             stage.kind == AudioFilterKind::BandPass ||
                             stage.kind == AudioFilterKind::Notch ||
                             stage.kind == AudioFilterKind::DcBlocker ||
                             stage.kind == AudioFilterKind::ParametricEq ||
                             stage.kind == AudioFilterKind::LowShelf ||
                             stage.kind == AudioFilterKind::HighShelf ||
                             stage.kind == AudioFilterKind::AdaptiveNotch ||
                             stage.kind == AudioFilterKind::CwFilter ||
                             stage.kind == AudioFilterKind::CtcssSuppressor;
        const bool gain = stage.kind == AudioFilterKind::Gain ||
                          stage.kind == AudioFilterKind::ParametricEq ||
                          stage.kind == AudioFilterKind::LowShelf ||
                          stage.kind == AudioFilterKind::HighShelf ||
                          stage.kind == AudioFilterKind::Compressor;
        const bool dynamics = stage.kind == AudioFilterKind::Limiter ||
                              stage.kind == AudioFilterKind::NoiseGate ||
                              stage.kind == AudioFilterKind::Compressor;
        const bool compressor = stage.kind == AudioFilterKind::Compressor;
        const bool deEmphasis = stage.kind == AudioFilterKind::DeEmphasis;
        const bool adaptiveNotch = stage.kind == AudioFilterKind::AdaptiveNotch;
        const bool noiseBlanker = stage.kind == AudioFilterKind::NoiseBlanker;
        const bool cwFilter = stage.kind == AudioFilterKind::CwFilter;
        const bool spectralDenoise = stage.kind == AudioFilterKind::SpectralDenoise;

        frequencyLabel->setText(ukrainianLanguage
                                    ? (stage.kind == AudioFilterKind::BandPass || adaptiveNotch
                                           ? QStringLiteral("Нижня частота:")
                                           : QStringLiteral("Частота:"))
                                    : (stage.kind == AudioFilterKind::BandPass || adaptiveNotch
                                           ? QStringLiteral("Low frequency:")
                                           : QStringLiteral("Frequency:")));
        frequency2Label->setText(ukrainianLanguage ? QStringLiteral("Верхня частота:")
                                                    : QStringLiteral("High frequency:"));
        qLabel->setText(QStringLiteral("Q:"));
        gainLabel->setText(ukrainianLanguage ? QStringLiteral("Підсилення:")
                                              : QStringLiteral("Gain:"));
        if (compressor) {
            gainLabel->setText(ukrainianLanguage ? QStringLiteral("Вихідне підсилення:")
                                                  : QStringLiteral("Makeup gain:"));
        }
        thresholdLabel->setText(ukrainianLanguage ? QStringLiteral("Поріг:")
                                                   : QStringLiteral("Threshold:"));
        ratioLabel->setText(ukrainianLanguage ? QStringLiteral("Співвідношення:")
                                               : QStringLiteral("Ratio:"));
        attackLabel->setText(ukrainianLanguage ? QStringLiteral("Атака:")
                                                : QStringLiteral("Attack:"));
        releaseLabel->setText(ukrainianLanguage ? QStringLiteral("Відновлення:")
                                                 : QStringLiteral("Release:"));
        timeConstantLabel->setText(ukrainianLanguage ? QStringLiteral("Стала часу:")
                                                      : QStringLiteral("Time constant:"));
        bandwidthLabel->setText(ukrainianLanguage ? QStringLiteral("Ширина смуги:")
                                                   : QStringLiteral("Bandwidth:"));
        amountLabel->setText(ukrainianLanguage ? QStringLiteral("Інтенсивність:")
                                                : QStringLiteral("Amount:"));
        if (adaptiveNotch) {
            ratioLabel->setText(ukrainianLanguage ? QStringLiteral("Пік / фон:")
                                                   : QStringLiteral("Peak / floor:"));
        } else if (noiseBlanker) {
            ratioLabel->setText(ukrainianLanguage ? QStringLiteral("Поріг імпульсу:")
                                                   : QStringLiteral("Impulse ratio:"));
            attackLabel->setText(ukrainianLanguage ? QStringLiteral("Тривалість гасіння:")
                                                    : QStringLiteral("Blank duration:"));
            releaseLabel->setText(ukrainianLanguage ? QStringLiteral("Оцінка фону:")
                                                     : QStringLiteral("Floor tracking:"));
        } else if (spectralDenoise) {
            thresholdLabel->setText(ukrainianLanguage ? QStringLiteral("Макс. ослаблення:")
                                                       : QStringLiteral("Max attenuation:"));
            ratioLabel->setText(ukrainianLanguage ? QStringLiteral("Віднімання шуму:")
                                                   : QStringLiteral("Noise subtraction:"));
            attackLabel->setText(ukrainianLanguage ? QStringLiteral("Спад оцінки:")
                                                    : QStringLiteral("Floor fall:"));
            releaseLabel->setText(ukrainianLanguage ? QStringLiteral("Ріст оцінки:")
                                                     : QStringLiteral("Floor rise:"));
        }
        frequencyLabel->setVisible(firstFrequency);
        frequencySpin->setVisible(firstFrequency);
        frequency2Label->setVisible(secondFrequency);
        frequency2Spin->setVisible(secondFrequency);
        qLabel->setVisible(quality);
        qSpin->setVisible(quality);
        gainLabel->setVisible(gain);
        gainSpin->setVisible(gain);
        thresholdLabel->setVisible(dynamics || spectralDenoise);
        thresholdSpin->setVisible(dynamics || spectralDenoise);
        ratioLabel->setVisible(compressor || adaptiveNotch || noiseBlanker || spectralDenoise);
        ratioSpin->setVisible(compressor || adaptiveNotch || noiseBlanker || spectralDenoise);
        attackLabel->setVisible(compressor || noiseBlanker || spectralDenoise);
        attackSpin->setVisible(compressor || noiseBlanker || spectralDenoise);
        releaseLabel->setVisible(dynamics || noiseBlanker || spectralDenoise);
        releaseSpin->setVisible(dynamics || noiseBlanker || spectralDenoise);
        timeConstantLabel->setVisible(deEmphasis);
        timeConstantSpin->setVisible(deEmphasis);
        bandwidthLabel->setVisible(cwFilter);
        bandwidthSpin->setVisible(cwFilter);
        amountLabel->setVisible(spectralDenoise);
        amountSpin->setVisible(spectralDenoise);
        qSpin->setReadOnly(cwFilter);
    }

    void updatePreview() {
        AudioFilterStage preview = result();
        responseWidget->setStage(preview);
    }

    void notifyStageChanged() {
        updatePreview();
        if (stageChanged) {
            stageChanged(result());
        }
    }

    void resetParameters() {
        const AudioFilterStage defaults = defaultAudioFilterStage(stage.kind);
        const QSignalBlocker frequencyBlocker(frequencySpin);
        const QSignalBlocker frequency2Blocker(frequency2Spin);
        const QSignalBlocker qBlocker(qSpin);
        const QSignalBlocker gainBlocker(gainSpin);
        const QSignalBlocker thresholdBlocker(thresholdSpin);
        const QSignalBlocker ratioBlocker(ratioSpin);
        const QSignalBlocker attackBlocker(attackSpin);
        const QSignalBlocker releaseBlocker(releaseSpin);
        const QSignalBlocker timeConstantBlocker(timeConstantSpin);
        const QSignalBlocker bandwidthBlocker(bandwidthSpin);
        const QSignalBlocker amountBlocker(amountSpin);
        frequencySpin->setValue(defaults.frequencyHz);
        frequency2Spin->setValue(defaults.frequency2Hz);
        qSpin->setValue(defaults.q);
        gainSpin->setValue(defaults.gainDb);
        thresholdSpin->setValue(defaults.thresholdDb);
        ratioSpin->setValue(defaults.ratio);
        attackSpin->setValue(defaults.attackMs);
        releaseSpin->setValue(defaults.releaseMs);
        timeConstantSpin->setValue(defaults.timeConstantUs);
        bandwidthSpin->setValue(defaults.bandwidthHz);
        amountSpin->setValue(defaults.amount * 100.0);
        stage.curve = defaults.curve;
        notifyStageChanged();
    }

    AudioFilterStage stage;
    bool ukrainianLanguage = false;
    QLineEdit *nameEdit = nullptr;
    QDoubleSpinBox *frequencySpin = nullptr;
    QDoubleSpinBox *frequency2Spin = nullptr;
    QDoubleSpinBox *qSpin = nullptr;
    QDoubleSpinBox *gainSpin = nullptr;
    QDoubleSpinBox *thresholdSpin = nullptr;
    QDoubleSpinBox *ratioSpin = nullptr;
    QDoubleSpinBox *attackSpin = nullptr;
    QDoubleSpinBox *releaseSpin = nullptr;
    QDoubleSpinBox *timeConstantSpin = nullptr;
    QDoubleSpinBox *bandwidthSpin = nullptr;
    QDoubleSpinBox *amountSpin = nullptr;
    QLabel *frequencyLabel = nullptr;
    QLabel *frequency2Label = nullptr;
    QLabel *qLabel = nullptr;
    QLabel *gainLabel = nullptr;
    QLabel *thresholdLabel = nullptr;
    QLabel *ratioLabel = nullptr;
    QLabel *attackLabel = nullptr;
    QLabel *releaseLabel = nullptr;
    QLabel *timeConstantLabel = nullptr;
    QLabel *bandwidthLabel = nullptr;
    QLabel *amountLabel = nullptr;
    AudioFilterResponseWidget *responseWidget = nullptr;
};
}

AudioFilterChainWidget::AudioFilterChainWidget(QWidget *parent)
    : QWidget(parent) {
    table = new QTableWidget(0, 4, this);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->verticalHeader()->setVisible(false);
    table->horizontalHeader()->setStretchLastSection(false);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    table->setMinimumHeight(145);
    table->setContextMenuPolicy(Qt::CustomContextMenu);

    addMenu = new QMenu(this);
    addButton = new QToolButton(this);
    addButton->setText(QStringLiteral("+"));
    addButton->setPopupMode(QToolButton::InstantPopup);
    addButton->setMenu(addMenu);
    removeButton = new QToolButton(this);
    removeButton->setText(QStringLiteral("−"));
    moveUpButton = new QToolButton(this);
    moveUpButton->setIcon(style()->standardIcon(QStyle::SP_ArrowUp));
    moveDownButton = new QToolButton(this);
    moveDownButton->setIcon(style()->standardIcon(QStyle::SP_ArrowDown));

    for (QToolButton *button : {addButton, removeButton, moveUpButton, moveDownButton}) {
        button->setAutoRaise(false);
        button->setFixedSize(30, 28);
    }

    QHBoxLayout *buttons = new QHBoxLayout();
    buttons->setContentsMargins(0, 0, 0, 0);
    buttons->setSpacing(4);
    buttons->addWidget(addButton);
    buttons->addWidget(removeButton);
    buttons->addWidget(moveUpButton);
    buttons->addWidget(moveDownButton);
    buttons->addStretch(1);

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    layout->addWidget(table);
    layout->addLayout(buttons);

    connect(removeButton, &QToolButton::clicked, this, &AudioFilterChainWidget::removeSelectedStage);
    connect(moveUpButton, &QToolButton::clicked, this, [this]() { moveSelectedStage(-1); });
    connect(moveDownButton, &QToolButton::clicked, this, [this]() { moveSelectedStage(1); });
    connect(table, &QTableWidget::cellDoubleClicked, this, [this](int row, int) { editStage(row); });
    connect(table, &QTableWidget::customContextMenuRequested,
            this, &AudioFilterChainWidget::showStageTypeMenu);
    connect(table, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
        if (rebuilding || !item || item->column() != 1 ||
            item->row() < 0 || item->row() >= stages.size()) {
            return;
        }
        stages[item->row()].enabled = item->checkState() == Qt::Checked;
        if (auto *editor = openEditors.value(stages[item->row()].id).data()) {
            if (auto *box = editor->findChild<QCheckBox *>(QStringLiteral("audioFilterEnabled"))) {
                const QSignalBlocker blocker(box); box->setChecked(stages[item->row()].enabled);
            }
        }
        emitConfigurationChanged();
    });

    setLanguage(false);
}

QString AudioFilterChainWidget::configurationJson() const {
    return encodeAudioFilterChain(stages);
}

void AudioFilterChainWidget::setConfigurationJson(const QString &json) {
    const auto editors = openEditors.values();
    openEditors.clear();
    for (const QPointer<QWidget> &editor : editors) {
        if (editor) {
            editor->close();
        }
    }
    bool ok = false;
    const QVector<AudioFilterStage> decoded = decodeAudioFilterChain(json, &ok);
    stages = ok ? decoded : QVector<AudioFilterStage>();
    ensureStageIds();
    rebuildTable();
}

void AudioFilterChainWidget::setLanguage(bool ukrainian) {
    ukrainianLanguage = ukrainian;
    addButton->setToolTip(ukrainian ? QStringLiteral("Додати фільтр")
                                    : QStringLiteral("Add filter"));
    removeButton->setToolTip(ukrainian ? QStringLiteral("Видалити вибраний фільтр")
                                       : QStringLiteral("Remove selected filter"));
    moveUpButton->setToolTip(ukrainian ? QStringLiteral("Підняти фільтр")
                                       : QStringLiteral("Move filter up"));
    moveDownButton->setToolTip(ukrainian ? QStringLiteral("Опустити фільтр")
                                         : QStringLiteral("Move filter down"));
    rebuildAddMenu();
    rebuildTable(table->currentRow());
}

void AudioFilterChainWidget::ensureStage(const QString &id, AudioFilterKind kind, bool initiallyEnabled) {
    if (id.trimmed().isEmpty()) {
        return;
    }
    const int existingRow = rowForStageId(id);
    if (existingRow >= 0) {
        if (stages.at(existingRow).kind != kind) {
            AudioFilterStage replacement = defaultAudioFilterStage(kind);
            replacement.id = id;
            replacement.enabled = stages.at(existingRow).enabled;
            replacement.customName = stages.at(existingRow).customName;
            closeEditorForStage(id);
            stages[existingRow] = replacement;
            rebuildTable(existingRow);
            emitConfigurationChanged();
        }
        return;
    }
    if (stages.size() >= 32) {
        return;
    }
    AudioFilterStage stage = defaultAudioFilterStage(kind);
    stage.id = id;
    stage.enabled = initiallyEnabled;
    stages.push_back(stage);
    rebuildTable(stages.size() - 1);
    emitConfigurationChanged();
}

bool AudioFilterChainWidget::removeStageById(const QString &id) {
    const int row = rowForStageId(id);
    if (row < 0) return false;
    closeEditorForStage(id);
    stages.removeAt(row);
    rebuildTable(std::min(row, stages.size() - 1));
    emitConfigurationChanged();
    return true;
}

bool AudioFilterChainWidget::openStageEditor(const QString &id) {
    const int row = rowForStageId(id);
    if (row < 0) {
        return false;
    }
    editStage(row);
    return true;
}

void AudioFilterChainWidget::rebuildAddMenu() {
    addMenu->clear();
    for (AudioFilterKind kind : availableFilterKinds()) {
        QAction *action = addMenu->addAction(audioFilterKindName(kind, ukrainianLanguage));
        connect(action, &QAction::triggered, this, [this, kind]() { addStage(kind); });
    }
}

void AudioFilterChainWidget::showStageTypeMenu(const QPoint &position) {
    const QModelIndex index = table->indexAt(position);
    if (!index.isValid() || index.column() != 2 || index.row() >= stages.size()) {
        return;
    }
    const int row = index.row();
    QMenu menu(table);
    menu.setTitle(ukrainianLanguage ? QStringLiteral("Замінити фільтр")
                                    : QStringLiteral("Replace filter"));
    for (AudioFilterKind kind : availableFilterKinds()) {
        QAction *action = menu.addAction(audioFilterKindName(kind, ukrainianLanguage));
        action->setCheckable(true);
        action->setChecked(kind == stages.at(row).kind);
        action->setEnabled(kind != stages.at(row).kind);
        connect(action, &QAction::triggered, this, [this, row, kind]() {
            replaceStageType(row, kind);
        });
    }
    menu.exec(table->viewport()->mapToGlobal(position));
}

void AudioFilterChainWidget::replaceStageType(int row, AudioFilterKind kind) {
    if (row < 0 || row >= stages.size() || stages.at(row).kind == kind) {
        return;
    }
    const AudioFilterStage previous = stages.at(row);
    closeEditorForStage(previous.id);
    AudioFilterStage replacement = defaultAudioFilterStage(kind);
    replacement.id = previous.id;
    replacement.enabled = previous.enabled;
    replacement.customName = previous.customName;
    stages[row] = replacement;
    rebuildTable(row);
    emitConfigurationChanged();
    editStage(row);
}

void AudioFilterChainWidget::rebuildTable(int selectedRow) {
    rebuilding = true;
    table->clearContents();
    table->setRowCount(stages.size());
    table->setHorizontalHeaderLabels(ukrainianLanguage
        ? QStringList{QStringLiteral("№"), QStringLiteral("Увімк."), QStringLiteral("Назва"), QStringLiteral("Зміна")}
        : QStringList{QStringLiteral("#"), QStringLiteral("On"), QStringLiteral("Name"), QStringLiteral("Edit")});
    for (int row = 0; row < stages.size(); ++row) {
        QTableWidgetItem *number = new QTableWidgetItem(QString::number(row + 1));
        number->setTextAlignment(Qt::AlignCenter);
        table->setItem(row, 0, number);

        QTableWidgetItem *enabled = new QTableWidgetItem();
        enabled->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
        enabled->setCheckState(stages.at(row).enabled ? Qt::Checked : Qt::Unchecked);
        enabled->setTextAlignment(Qt::AlignCenter);
        table->setItem(row, 1, enabled);

        table->setItem(row, 2, new QTableWidgetItem(stageDisplayName(stages.at(row))));
        QPushButton *edit = new QPushButton(ukrainianLanguage ? QStringLiteral("Змінити")
                                                              : QStringLiteral("Edit"), table);
        edit->setToolTip(ukrainianLanguage ? QStringLiteral("Відкрити параметри фільтра")
                                           : QStringLiteral("Open filter parameters"));
        connect(edit, &QPushButton::clicked, this, [this, row]() { editStage(row); });
        table->setCellWidget(row, 3, edit);
        table->setRowHeight(row, 28);
    }
    if (!stages.isEmpty()) {
        const int row = (std::clamp)(selectedRow, 0, stages.size() - 1);
        table->selectRow(row);
    }
    rebuilding = false;
}

void AudioFilterChainWidget::addStage(AudioFilterKind kind) {
    if (stages.size() >= 32) {
        return;
    }
    AudioFilterStage stage = defaultAudioFilterStage(kind);
    stage.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    stages.push_back(stage);
    const int row = stages.size() - 1;
    rebuildTable(row);
    emitConfigurationChanged();
    editStage(row);
}

void AudioFilterChainWidget::removeSelectedStage() {
    const int row = table->currentRow();
    if (row < 0 || row >= stages.size()) {
        return;
    }
    closeEditorForStage(stages.at(row).id);
    stages.removeAt(row);
    rebuildTable((std::min)(row, stages.size() - 1));
    emitConfigurationChanged();
}

void AudioFilterChainWidget::moveSelectedStage(int delta) {
    const int row = table->currentRow();
    const int target = row + delta;
    if (row < 0 || row >= stages.size() || target < 0 || target >= stages.size()) {
        return;
    }
    stages.swapItemsAt(row, target);
    rebuildTable(target);
    emitConfigurationChanged();
}

void AudioFilterChainWidget::editStage(int row) {
    if (row < 0 || row >= stages.size()) {
        return;
    }
    ensureStageIds();
    const QString id = stages.at(row).id;
    if (QWidget *existing = openEditors.value(id).data()) {
        if (auto *box = existing->findChild<QCheckBox *>(QStringLiteral("audioFilterEnabled"))) {
            const QSignalBlocker blocker(box); box->setChecked(stages[row].enabled);
        }
        existing->show();
        existing->raise();
        existing->activateWindow();
        return;
    }
    AudioFilterEditorDialog *dialog = new AudioFilterEditorDialog(stages.at(row),
                                                                   ukrainianLanguage,
                                                                   this);
    dialog->enabledChanged = [this, id](bool enabled) {
        const int row = rowForStageId(id);
        if (row < 0 || stages[row].enabled == enabled) return;
        stages[row].enabled = enabled;
        { const QSignalBlocker blocker(table); table->item(row, 1)->setCheckState(enabled ? Qt::Checked : Qt::Unchecked); }
        emitConfigurationChanged();
    };
    dialog->stageChanged = [this, id](const AudioFilterStage &updated) {
        const int currentRow = rowForStageId(id);
        if (currentRow < 0) {
            return;
        }
        AudioFilterStage replacement = updated;
        replacement.id = id;
        replacement.enabled = stages.at(currentRow).enabled;
        stages[currentRow] = replacement;
        if (QTableWidgetItem *name = table->item(currentRow, 2)) {
            name->setText(stageDisplayName(replacement));
        }
        emitConfigurationChanged();
    };
    openEditors.insert(id, dialog);
    connect(dialog, &QObject::destroyed, this, [this, id]() {
        openEditors.remove(id);
    });
    dialog->show();
    positionEditor(dialog);
    dialog->raise();
    dialog->activateWindow();
}

void AudioFilterChainWidget::emitConfigurationChanged() {
    emit configurationChanged(configurationJson());
}

QString AudioFilterChainWidget::stageDisplayName(const AudioFilterStage &stage) const {
    return stage.customName.trimmed().isEmpty()
               ? audioFilterKindName(stage.kind, ukrainianLanguage)
               : stage.customName.trimmed();
}

void AudioFilterChainWidget::ensureStageIds() {
    QSet<QString> used;
    for (AudioFilterStage &stage : stages) {
        if (stage.id.isEmpty() || used.contains(stage.id)) {
            stage.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        }
        used.insert(stage.id);
    }
}

int AudioFilterChainWidget::rowForStageId(const QString &id) const {
    for (int row = 0; row < stages.size(); ++row) {
        if (stages.at(row).id == id) {
            return row;
        }
    }
    return -1;
}

void AudioFilterChainWidget::closeEditorForStage(const QString &id) {
    QPointer<QWidget> editor = openEditors.take(id);
    if (editor) {
        editor->close();
    }
}

void AudioFilterChainWidget::positionEditor(QWidget *editor) {
    if (!editor) {
        return;
    }
    QScreen *targetScreen = QGuiApplication::screenAt(mapToGlobal(rect().center()));
    if (!targetScreen) {
        targetScreen = screen();
    }
    if (!targetScreen) {
        return;
    }
    const QRect available = targetScreen->availableGeometry();
    const int slot = (std::max)(0, openEditors.size() - 1);
    const int spacing = 12;
    const int columns = (std::max)(1, available.width() / (editor->width() + spacing));
    int x = available.left() + spacing + (slot % columns) * (editor->width() + spacing);
    int y = available.top() + spacing + (slot / columns) * 42;
    x = (std::clamp)(x, available.left(),
                     (std::max)(available.left(), available.right() - editor->width() + 1));
    y = (std::clamp)(y, available.top(),
                     (std::max)(available.top(), available.bottom() - editor->height() + 1));
    editor->move(x, y);
}

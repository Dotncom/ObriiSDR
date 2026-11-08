#include "multivfowidget.h"

#include "radiosettings.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHeaderView>
#include <QHash>
#include <QDialog>
#include <QGridLayout>
#include <QImage>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QTableWidget>
#include <QUuid>
#include <QVBoxLayout>
#include <QScrollArea>
#include <QPainter>

#include <algorithm>
#include <cmath>
#include <limits>
#include <cstring>
#include <functional>
#include <utility>

namespace {
constexpr int kMaximumVfos = 16;
constexpr int kUiUpdateIntervalMs = 100;
constexpr int kMinimumDbfsRole = Qt::UserRole + 1;
constexpr int kMaximumDbfsRole = Qt::UserRole + 2;
constexpr int kChannelIdRole = Qt::UserRole + 3;

struct ModulationChoice {
    const char *name;
    int value;
};

constexpr ModulationChoice kModulations[] = {
    {"AM", MOD_AM}, {"NFM", MOD_NFM}, {"SAM", MOD_SAM}, {"USB", MOD_USB},
    {"LSB", MOD_LSB}, {"DSB", MOD_DSB}, {"CW", MOD_CW}, {"WFM", MOD_WFM},
    {"FT8", MOD_FT8}, {"RTTY", MOD_RTTY}, {"FSK", MOD_FSK}, {"PSK", MOD_PSK},
    {"DMR", MOD_DMR}
};

QCheckBox *checkBoxAt(QTableWidget *table, int row, int column) {
    return table ? qobject_cast<QCheckBox *>(table->cellWidget(row, column)) : nullptr;
}

QDoubleSpinBox *doubleSpinAt(QTableWidget *table, int row, int column) {
    return table ? qobject_cast<QDoubleSpinBox *>(table->cellWidget(row, column)) : nullptr;
}

QComboBox *comboAt(QTableWidget *table, int row, int column) {
    return table ? qobject_cast<QComboBox *>(table->cellWidget(row, column)) : nullptr;
}

class MultiVfoPreview final : public QWidget {
public:
    explicit MultiVfoPreview(int mode,
                             std::function<void(int)> editRequested,
                             QWidget *parent = nullptr)
        : QWidget(parent),
          mode_(mode),
          editRequested_(std::move(editRequested)),
          waterfall_(320, 92, QImage::Format_RGB32) {
        setMinimumSize(330, mode_ == 2 ? 250 : 155);
        waterfall_.fill(QColor(4, 7, 12));
    }

    void setChannel(const QString &channelId, int row, const QString &name,
                    double centerHz, double bandwidthHz) {
        const bool mappingChanged = channelId_ != channelId ||
                                    !qFuzzyCompare(centerHz_ + 1.0, centerHz + 1.0) ||
                                    !qFuzzyCompare(bandwidthHz_ + 1.0, bandwidthHz + 1.0);
        channelId_ = channelId; row_ = row; name_ = name;
        centerHz_ = centerHz; bandwidthHz_ = bandwidthHz;
        if (mappingChanged && mode_ != 0) {
            waterfallRows_ = 0;
            waterfallHead_ = 0;
            waterfall_.fill(QColor(4, 7, 12));
        }
        update();
    }
    int channelRow() const { return row_; }
    QString channelId() const { return channelId_; }
    int mode() const { return mode_; }
    void setDbfsRange(float minimumDbfs, float maximumDbfs) {
        minimumDbfs = std::clamp(minimumDbfs, -200.0f, 19.0f);
        maximumDbfs = std::clamp(maximumDbfs, minimumDbfs + 1.0f, 20.0f);
        if (qFuzzyCompare(minimumDbfs_ + 201.0f, minimumDbfs + 201.0f) &&
            qFuzzyCompare(maximumDbfs_ + 201.0f, maximumDbfs + 201.0f)) return;
        minimumDbfs_ = minimumDbfs;
        maximumDbfs_ = maximumDbfs;
        recolorWaterfall();
        update();
    }
    void setSpectrum(const std::vector<float> &frequencies, const std::vector<float> &levels) {
        if (frequencies.empty() || frequencies.size() != levels.size() || bandwidthHz_ <= 0.0) return;
        const double low = centerHz_ - bandwidthHz_ * 0.5;
        const double high = centerHz_ + bandwidthHz_ * 0.5;
        auto begin = std::lower_bound(frequencies.begin(), frequencies.end(), static_cast<float>(low));
        auto end = std::upper_bound(frequencies.begin(), frequencies.end(), static_cast<float>(high));
        if (begin == end) {
            begin = std::lower_bound(frequencies.begin(), frequencies.end(), static_cast<float>(centerHz_));
            if (begin == frequencies.end()) --begin;
            end = begin + 1;
        }
        const std::size_t first = static_cast<std::size_t>(std::distance(frequencies.begin(), begin));
        const std::size_t last = static_cast<std::size_t>(std::distance(frequencies.begin(), end));
        levels_.assign(320, -160.0f);
        for (int pixel = 0; pixel < 320; ++pixel) {
            const std::size_t sourceBegin = first + (last - first) * static_cast<std::size_t>(pixel) / 320U;
            const std::size_t sourceEnd = first + (last - first) * static_cast<std::size_t>(pixel + 1) / 320U;
            for (std::size_t index = sourceBegin; index < (std::max)(sourceBegin + 1U, sourceEnd) && index < levels.size(); ++index) {
                const std::size_t shiftedIndex = (index + levels.size() / 2U) % levels.size();
                if (std::isfinite(levels[shiftedIndex])) levels_[static_cast<std::size_t>(pixel)] =
                    (std::max)(levels_[static_cast<std::size_t>(pixel)], levels[shiftedIndex]);
            }
        }
        if (mode_ != 0) {
            if (waterfallLevels_.empty()) {
                waterfallLevels_.resize(static_cast<std::size_t>(waterfall_.width() * waterfall_.height()));
            }
            waterfallHead_ = (waterfallHead_ + waterfall_.height() - 1) % waterfall_.height();
            std::copy(levels_.begin(), levels_.end(),
                      waterfallLevels_.begin() + waterfallHead_ * waterfall_.width());
            waterfallRows_ = (std::min)(waterfallRows_ + 1, waterfall_.height());
            for (int y = waterfall_.height() - 1; y > 0; --y) {
                std::memcpy(waterfall_.scanLine(y), waterfall_.constScanLine(y - 1),
                            static_cast<std::size_t>(waterfall_.bytesPerLine()));
            }
            QRgb *line = reinterpret_cast<QRgb *>(waterfall_.scanLine(0));
            for (int x = 0; x < waterfall_.width(); ++x) {
                line[x] = colorForLevel(levels_[static_cast<std::size_t>(x)]);
            }
        }
        update();
    }

#ifdef OBRII_VFO_SELFTEST
    const QImage &historyImageForTest() const { return waterfall_; }
    const std::vector<float> &historyLevelsForTest() const { return waterfallLevels_; }
    int historyRowsForTest() const { return waterfallRows_; }
    int historyHeadForTest() const { return waterfallHead_; }
#endif

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(7, 10, 16));
        painter.setPen(QColor(226, 232, 240));
        painter.drawText(QRect(8, 3, width() - 16, 18), Qt::AlignLeft | Qt::AlignVCenter,
                         QStringLiteral("%1   %2 MHz   %3 kHz")
                             .arg(name_).arg(centerHz_ / 1.0e6, 0, 'f', 6).arg(bandwidthHz_ / 1000.0, 0, 'f', 1));
        QRect body = rect().adjusted(6, 24, -6, -18);
        QRect spectrumRect = body;
        QRect waterfallRect = body;
        if (mode_ == 2) {
            spectrumRect.setHeight(body.height() / 2 - 2);
            waterfallRect.setTop(spectrumRect.bottom() + 5);
        }
        if (mode_ != 1 && !levels_.empty()) {
            painter.setPen(QPen(QColor(36, 45, 58), 1));
            for (int i = 1; i < 4; ++i) painter.drawLine(spectrumRect.left(), spectrumRect.top() + spectrumRect.height() * i / 4,
                                                        spectrumRect.right(), spectrumRect.top() + spectrumRect.height() * i / 4);
            QPolygonF line;
            line.reserve(static_cast<int>(levels_.size()));
            for (std::size_t index = 0; index < levels_.size(); ++index) {
                const qreal x = spectrumRect.left() + spectrumRect.width() * qreal(index) / qreal(levels_.size() - 1U);
                const qreal value = std::clamp(
                    (levels_[index] - minimumDbfs_) / (maximumDbfs_ - minimumDbfs_),
                    0.0f,
                    1.0f);
                line << QPointF(x, spectrumRect.bottom() - value * spectrumRect.height());
            }
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.setPen(QPen(QColor(72, 222, 148), 1.4));
            painter.drawPolyline(line);
        }
        if (mode_ != 0) painter.drawImage(waterfallRect, waterfall_);
        painter.setPen(QColor(145, 157, 171));
        painter.drawText(QRect(6, height() - 17, width() - 12, 15), Qt::AlignLeft,
                         QString::number((centerHz_ - bandwidthHz_ * 0.5) / 1.0e6, 'f', 6));
        painter.drawText(QRect(6, height() - 17, width() - 12, 15), Qt::AlignRight,
                         QStringLiteral("%1 MHz").arg((centerHz_ + bandwidthHz_ * 0.5) / 1.0e6, 0, 'f', 6));
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton && editRequested_) {
            editRequested_(row_);
            event->accept();
            return;
        }
        QWidget::mouseDoubleClickEvent(event);
    }

private:
    QRgb colorForLevel(float level) const {
        const float value = std::clamp((level - minimumDbfs_) / (maximumDbfs_ - minimumDbfs_), 0.0f, 1.0f);
        const int red = int(255.0f * std::clamp((value - 0.50f) * 2.0f, 0.0f, 1.0f));
        const int green = int(255.0f * std::clamp(1.0f - std::abs(value - 0.55f) * 2.2f, 0.0f, 1.0f));
        const int blue = int(255.0f * std::clamp(1.0f - value * 1.35f, 0.0f, 1.0f));
        return qRgb(red, green, blue);
    }
    void recolorWaterfall() {
        if (mode_ == 0) return;
        for (int y = 0; y < waterfall_.height(); ++y) {
            QRgb *line = reinterpret_cast<QRgb *>(waterfall_.scanLine(y));
            if (y >= waterfallRows_) {
                std::fill(line, line + waterfall_.width(), qRgb(4, 7, 12));
                continue;
            }
            const auto offset = (waterfallHead_ + y) % waterfall_.height() * waterfall_.width();
            for (int x = 0; x < waterfall_.width(); ++x)
                line[x] = colorForLevel(waterfallLevels_[static_cast<std::size_t>(offset + x)]);
        }
    }
    int mode_ = 2;
    int row_ = -1;
    QString channelId_;
    QString name_;
    double centerHz_ = 0.0;
    double bandwidthHz_ = 0.0;
    std::vector<float> levels_;
    std::function<void(int)> editRequested_;
    QImage waterfall_;
    std::vector<float> waterfallLevels_;
    int waterfallHead_ = 0;
    int waterfallRows_ = 0;
    float minimumDbfs_ = -140.0f;
    float maximumDbfs_ = -40.0f;
};
}

#ifdef OBRII_VFO_SELFTEST
bool multiVfoPreviewHistorySelfTest() {
    const std::vector<float> frequencies{100, 101, 102, 103};
    for (int mode : {1, 2}) {
        MultiVfoPreview preview(mode, {}), other(mode, {});
        preview.setChannel(QStringLiteral("one"), 0, QStringLiteral("One"), 101.5, 4);
        other.setChannel(QStringLiteral("two"), 1, QStringLiteral("Two"), 101.5, 4);
        for (int row = 0; row < 130; ++row) {
            const std::vector<float> levels(4, -130.0f + row % 50);
            preview.setSpectrum(frequencies, levels);
            other.setSpectrum(frequencies, levels);
        }
        const QImage original = preview.historyImageForTest();
        const QImage otherOriginal = other.historyImageForTest();
        const auto history = preview.historyLevelsForTest();
        const int head = preview.historyHeadForTest();
        preview.setDbfsRange(-155, -30);
        if (preview.historyRowsForTest() != 92 || preview.historyHeadForTest() != head ||
            preview.historyLevelsForTest() != history || preview.historyImageForTest() == original ||
            other.historyImageForTest() != otherOriginal) return false;
        preview.setDbfsRange(-140, -40);
        if (preview.historyImageForTest() != original) return false;
        preview.setChannel(QStringLiteral("one"), 0, QStringLiteral("Renamed"), 101.5, 4);
        if (preview.historyImageForTest() != original) return false;
        preview.setChannel(QStringLiteral("one"), 0, QStringLiteral("Retuned"), 102.5, 4);
        if (preview.historyRowsForTest() != 0) return false;
        preview.setDbfsRange(-160, -20);
        for (int y = 0; y < 92; ++y)
            if (preview.historyImageForTest().pixel(0, y) != qRgb(4, 7, 12)) return false;
    }
    MultiVfoWidget host;
    QJsonArray channels{
        QJsonObject{{"id", "first"}, {"frequencyHz", 100000000}, {"bandwidthHz", 12500}},
        QJsonObject{{"id", "second"}, {"frequencyHz", 101000000}, {"bandwidthHz", 12500}}};
    const auto configure = [&] {
        return host.setConfigurationJson(QString::fromUtf8(QJsonDocument(
            QJsonObject{{"enabled", true}, {"viewMode", 2}, {"channels", channels}}).toJson()));
    };
    if (!configure()) return false;
    for (auto *button : host.findChildren<QPushButton *>())
        if (button->text() == QStringLiteral("Mosaic")) button->click();
    QHash<QString, MultiVfoPreview *> views;
    for (auto *widget : host.findChildren<QWidget *>()) {
        if (auto *preview = dynamic_cast<MultiVfoPreview *>(widget)) {
            views.insert(preview->channelId(), preview);
            preview->setSpectrum(frequencies, std::vector<float>(4, -90));
        }
    }
    if (views.size() != 2) return false;
    auto *first = views.value(QStringLiteral("first"));
    auto *second = views.value(QStringLiteral("second"));
    const auto firstHistory = first->historyLevelsForTest();
    const QImage secondHistory = second->historyImageForTest();
    QJsonObject updated = channels[0].toObject();
    updated.insert(QStringLiteral("minimumDbfs"), -155);
    channels[0] = updated;
    if (!configure() || first->historyLevelsForTest() != firstHistory ||
        first->historyRowsForTest() != 1 || second->historyImageForTest() != secondHistory) return false;
    const QImage firstImage = first->historyImageForTest();
    const QJsonValue originalFirst = channels[0];
    channels[0] = channels[1];
    channels[1] = originalFirst;
    if (!configure() || first->channelRow() != 1 || first->historyImageForTest() != firstImage) return false;
    channels.removeAt(0);
    if (!configure() || first->channelRow() != 0 || first->historyImageForTest() != firstImage) return false;
    return true;
}
#endif

MultiVfoWidget::MultiVfoWidget(QWidget *parent)
    : QWidget(parent) {
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(4);

    auto *header = new QHBoxLayout();
    header->setContentsMargins(0, 0, 0, 0);
    enabledCheckBox = new QCheckBox(this);
    enabledCheckBox->setChecked(false);
    engineLabel = new QLabel(this);
    engineLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    statusLabel = new QLabel(this);
    statusLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    header->addWidget(enabledCheckBox);
    header->addWidget(engineLabel, 1);
    header->addWidget(statusLabel);
    root->addLayout(header);

    table = new QTableWidget(0, ColumnCount, this);
    table->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    table->setMinimumWidth(0);
    table->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setAlternatingRowColors(true);
    table->verticalHeader()->hide();
    table->horizontalHeader()->setStretchLastSection(true);
    table->horizontalHeader()->setSectionResizeMode(EnabledColumn, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(MonitorColumn, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(NameColumn, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(FrequencyColumn, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(BandwidthColumn, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(ModulationColumn, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(SquelchColumn, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(LevelColumn, QHeaderView::ResizeToContents);
    table->setMinimumHeight(132);
    table->setMaximumHeight(260);
    root->addWidget(table);

    auto *buttons = new QHBoxLayout();
    buttons->setContentsMargins(0, 0, 0, 0);
    buttons->setSpacing(3);
    addButton = new QPushButton(QStringLiteral("+"), this);
    removeButton = new QPushButton(QStringLiteral("-"), this);
    upButton = new QPushButton(QStringLiteral("\u2191"), this);
    downButton = new QPushButton(QStringLiteral("\u2193"), this);
    monitorButton = new QPushButton(this);
    viewModeCombo = new QComboBox(this);
    viewModeCombo->addItem(QStringLiteral("Spectrum"), 0);
    viewModeCombo->addItem(QStringLiteral("Waterfall"), 1);
    viewModeCombo->addItem(QStringLiteral("Spectrum + waterfall"), 2);
    viewModeCombo->setCurrentIndex(2);
    mosaicButton = new QPushButton(this);
    for (QPushButton *button : {addButton, removeButton, upButton, downButton}) {
        button->setFixedWidth(28);
    }
    buttons->addWidget(addButton);
    buttons->addWidget(removeButton);
    buttons->addWidget(upButton);
    buttons->addWidget(downButton);
    buttons->addStretch(1);
    buttons->addWidget(viewModeCombo);
    buttons->addWidget(mosaicButton);
    buttons->addWidget(monitorButton);
    root->addLayout(buttons);

    connect(enabledCheckBox, &QCheckBox::toggled, this, [this]() {
        updateStatus();
        emitConfigurationChanged();
    });
    connect(addButton, &QPushButton::clicked, this, [this]() {
        addChannel(receiverListeningHz, receiverBandwidthHz, receiverModulationType);
        emitConfigurationChanged();
    });
    connect(removeButton, &QPushButton::clicked, this, &MultiVfoWidget::removeSelectedChannel);
    connect(upButton, &QPushButton::clicked, this, [this]() { moveSelectedChannel(-1); });
    connect(downButton, &QPushButton::clicked, this, [this]() { moveSelectedChannel(1); });
    connect(monitorButton, &QPushButton::clicked, this, [this]() {
        requestMonitorForRow(table->currentRow());
    });
    connect(mosaicButton, &QPushButton::clicked, this, &MultiVfoWidget::openMosaic);
    connect(viewModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() {
        if (mosaicDialog) rebuildMosaic();
        emitConfigurationChanged();
    });
    connect(table, &QTableWidget::itemChanged, this, [this]() { emitConfigurationChanged(); });

    spectrumUpdateTimer.start();
    setLanguage(false);
}

void MultiVfoWidget::setLanguage(bool useUkrainian) {
    ukrainian = useUkrainian;
    enabledCheckBox->setText(ukrainian ? QStringLiteral("Увімкнути") : QStringLiteral("Enable"));
    engineLabel->setText(ukrainian ? QStringLiteral("Спільний FFT-каналайзер")
                                   : QStringLiteral("Shared FFT channelizer"));
    engineLabel->setToolTip(ukrainian
        ? QStringLiteral("Усі VFO вимірюються з одного широкосмугового FFT без повторного читання IQ. Один обраний VFO передається до основного аудіодемодулятора.")
        : QStringLiteral("All VFOs are measured from one wideband FFT without rereading IQ. One selected VFO is routed to the main audio demodulator."));
    addButton->setToolTip(ukrainian ? QStringLiteral("Додати VFO") : QStringLiteral("Add VFO"));
    removeButton->setToolTip(ukrainian ? QStringLiteral("Видалити VFO") : QStringLiteral("Remove VFO"));
    upButton->setToolTip(ukrainian ? QStringLiteral("Підняти") : QStringLiteral("Move up"));
    downButton->setToolTip(ukrainian ? QStringLiteral("Опустити") : QStringLiteral("Move down"));
    monitorButton->setText(ukrainian ? QStringLiteral("Слухати обраний") : QStringLiteral("Monitor selected"));
    mosaicButton->setText(ukrainian ? QStringLiteral("Мозаїка") : QStringLiteral("Mosaic"));
    viewModeCombo->setItemText(0, ukrainian ? QStringLiteral("Спектр") : QStringLiteral("Spectrum"));
    viewModeCombo->setItemText(1, ukrainian ? QStringLiteral("Водоспад") : QStringLiteral("Waterfall"));
    viewModeCombo->setItemText(2, ukrainian ? QStringLiteral("Спектр + водоспад") : QStringLiteral("Spectrum + waterfall"));
    if (mosaicDialog) mosaicDialog->setWindowTitle(ukrainian ? QStringLiteral("Multi-VFO мозаїка") : QStringLiteral("Multi-VFO mosaic"));
    updateHeaders();
    updateStatus();
}

void MultiVfoWidget::setReceiverContext(double centerFrequencyHz,
                                        double sampleRate,
                                        double listeningFrequencyHz,
                                        double bandwidthHz,
                                        int modulationType) {
    if (receiverCenterHz == centerFrequencyHz && receiverSampleRate == sampleRate &&
        receiverListeningHz == listeningFrequencyHz && receiverBandwidthHz == bandwidthHz &&
        receiverModulationType == modulationType) return;
    receiverCenterHz = centerFrequencyHz;
    receiverSampleRate = sampleRate;
    receiverListeningHz = listeningFrequencyHz;
    receiverBandwidthHz = bandwidthHz;
    receiverModulationType = modulationType;
    updateStatus();
}

void MultiVfoWidget::addChannel(double frequencyHz,
                                double bandwidthHz,
                                int modulationType,
                                const QString &name,
                                const QString &channelId) {
    if (table->rowCount() >= kMaximumVfos) {
        return;
    }
    const QSignalBlocker tableBlocker(table);
    const int row = table->rowCount();
    table->insertRow(row);

    auto *enabled = new QCheckBox(table);
    enabled->setChecked(true);
    enabled->setStyleSheet(QStringLiteral("margin-left: 6px;"));
    table->setCellWidget(row, EnabledColumn, enabled);

    auto *monitor = new QCheckBox(table);
    monitor->setStyleSheet(QStringLiteral("margin-left: 6px;"));
    table->setCellWidget(row, MonitorColumn, monitor);

    auto *nameItem = new QTableWidgetItem(name.isEmpty()
                                              ? QStringLiteral("VFO %1").arg(row + 1)
                                              : name);
    QString effectiveChannelId = channelId.trimmed();
    bool duplicateId = effectiveChannelId.isEmpty();
    for (int candidate = 0; !duplicateId && candidate < row; ++candidate) {
        const QTableWidgetItem *candidateName = table->item(candidate, NameColumn);
        duplicateId = candidateName &&
                      candidateName->data(kChannelIdRole).toString() == effectiveChannelId;
    }
    if (duplicateId) {
        effectiveChannelId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    nameItem->setData(kChannelIdRole, effectiveChannelId);
    table->setItem(row, NameColumn, nameItem);

    auto *frequency = new QDoubleSpinBox(table);
    frequency->setRange(0.0, 7750.0);
    frequency->setDecimals(6);
    frequency->setSingleStep(0.0125);
    frequency->setSuffix(QStringLiteral(" MHz"));
    frequency->setKeyboardTracking(false);
    frequency->setValue((frequencyHz > 0.0 ? frequencyHz : receiverListeningHz) / 1.0e6);
    table->setCellWidget(row, FrequencyColumn, frequency);

    auto *bandwidth = new QDoubleSpinBox(table);
    bandwidth->setRange(0.1, 20000.0);
    bandwidth->setDecimals(1);
    bandwidth->setSuffix(QStringLiteral(" kHz"));
    bandwidth->setKeyboardTracking(false);
    bandwidth->setValue((std::max)(100.0, bandwidthHz) / 1000.0);
    table->setCellWidget(row, BandwidthColumn, bandwidth);

    auto *modulation = new QComboBox(table);
    for (const ModulationChoice &choice : kModulations) {
        modulation->addItem(QString::fromLatin1(choice.name), choice.value);
    }
    const int modulationIndex = modulation->findData(modulationType);
    modulation->setCurrentIndex(modulationIndex >= 0 ? modulationIndex : modulation->findData(MOD_NFM));
    table->setCellWidget(row, ModulationColumn, modulation);

    auto *squelch = new QDoubleSpinBox(table);
    squelch->setRange(-180.0, 20.0);
    squelch->setDecimals(1);
    squelch->setSuffix(QStringLiteral(" dB"));
    squelch->setKeyboardTracking(false);
    squelch->setValue(-95.0);
    table->setCellWidget(row, SquelchColumn, squelch);

    auto *levelItem = new QTableWidgetItem(QStringLiteral("--"));
    levelItem->setFlags(levelItem->flags() & ~Qt::ItemIsEditable);
    levelItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    table->setItem(row, LevelColumn, levelItem);
    auto *stateItem = new QTableWidgetItem(QStringLiteral("--"));
    stateItem->setFlags(stateItem->flags() & ~Qt::ItemIsEditable);
    table->setItem(row, StateColumn, stateItem);

    connect(enabled, &QCheckBox::toggled, this, [this]() { emitConfigurationChanged(); });
    connect(monitor, &QCheckBox::toggled, this, [this, monitor](bool checked) {
        if (!checked || loading) return;
        for (int candidate = 0; candidate < table->rowCount(); ++candidate) {
            if (checkBoxAt(table, candidate, MonitorColumn) == monitor) {
                enforceSingleMonitor(candidate);
                requestMonitorForRow(candidate);
                break;
            }
        }
        emitConfigurationChanged();
    });
    connect(frequency, &QDoubleSpinBox::editingFinished, this, [this, row]() {
        if (row == monitoredRow()) requestMonitorForRow(row);
        emitConfigurationChanged();
    });
    connect(bandwidth, &QDoubleSpinBox::editingFinished, this, [this, row]() {
        if (row == monitoredRow()) requestMonitorForRow(row);
        emitConfigurationChanged();
    });
    connect(squelch, &QDoubleSpinBox::editingFinished, this, [this]() { emitConfigurationChanged(); });
    connect(modulation, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, row]() {
        if (row == monitoredRow()) requestMonitorForRow(row);
        emitConfigurationChanged();
    });

    smoothedLevels.resize(static_cast<std::size_t>(table->rowCount()),
                          std::numeric_limits<float>::quiet_NaN());
    table->selectRow(row);
    updateStatus();
}

void MultiVfoWidget::removeSelectedChannel() {
    const int row = table->currentRow();
    if (row < 0) return;
    table->removeRow(row);
    smoothedLevels.assign(static_cast<std::size_t>(table->rowCount()),
                          std::numeric_limits<float>::quiet_NaN());
    if (table->rowCount() > 0) table->selectRow((std::min)(row, table->rowCount() - 1));
    updateStatus();
    emitConfigurationChanged();
}

void MultiVfoWidget::moveSelectedChannel(int direction) {
    const int row = table->currentRow();
    const int target = row + direction;
    if (row < 0 || target < 0 || target >= table->rowCount()) return;

    QJsonDocument document = QJsonDocument::fromJson(configurationJson().toUtf8());
    QJsonObject root = document.object();
    QJsonArray channels = root.value(QStringLiteral("channels")).toArray();
    QJsonValue value = channels.takeAt(row);
    channels.insert(target, value);
    root.insert(QStringLiteral("channels"), channels);
    setConfigurationJson(QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
    table->selectRow(target);
    emitConfigurationChanged();
}

void MultiVfoWidget::updateHeaders() {
    table->setHorizontalHeaderLabels({
        ukrainian ? QStringLiteral("Вкл") : QStringLiteral("On"),
        ukrainian ? QStringLiteral("Аудіо") : QStringLiteral("Audio"),
        ukrainian ? QStringLiteral("Назва") : QStringLiteral("Name"),
        ukrainian ? QStringLiteral("Частота") : QStringLiteral("Frequency"),
        ukrainian ? QStringLiteral("Смуга") : QStringLiteral("Bandwidth"),
        ukrainian ? QStringLiteral("Модуляція") : QStringLiteral("Mode"),
        QStringLiteral("Squelch"),
        ukrainian ? QStringLiteral("Рівень") : QStringLiteral("Level"),
        ukrainian ? QStringLiteral("Стан") : QStringLiteral("State")
    });
}

void MultiVfoWidget::updateStatus() {
    int active = 0;
    for (int row = 0; row < table->rowCount(); ++row) {
        if (QCheckBox *enabled = checkBoxAt(table, row, EnabledColumn); enabled && enabled->isChecked()) {
            ++active;
        }
    }
    const double lowMHz = (receiverCenterHz - receiverSampleRate * 0.5) / 1.0e6;
    const double highMHz = (receiverCenterHz + receiverSampleRate * 0.5) / 1.0e6;
    statusLabel->setText(ukrainian
        ? QStringLiteral("%1/%2 активні, %3-%4 МГц")
              .arg(active).arg(table->rowCount()).arg(lowMHz, 0, 'f', 3).arg(highMHz, 0, 'f', 3)
        : QStringLiteral("%1/%2 active, %3-%4 MHz")
              .arg(active).arg(table->rowCount()).arg(lowMHz, 0, 'f', 3).arg(highMHz, 0, 'f', 3));
}

void MultiVfoWidget::emitConfigurationChanged() {
    if (!loading) {
        if (mosaicDialog) updateMosaicConfiguration();
        emit configurationChanged(configurationJson());
    }
}

void MultiVfoWidget::requestMonitorForRow(int row) {
    if (row < 0 || row >= table->rowCount()) return;
    QDoubleSpinBox *frequency = doubleSpinAt(table, row, FrequencyColumn);
    QDoubleSpinBox *bandwidth = doubleSpinAt(table, row, BandwidthColumn);
    QComboBox *modulation = comboAt(table, row, ModulationColumn);
    if (!frequency || !bandwidth || !modulation) return;
    enforceSingleMonitor(row);
    emit monitorRequested(frequency->value() * 1.0e6,
                          bandwidth->value() * 1000.0,
                          modulation->currentData().toInt());
}

void MultiVfoWidget::enforceSingleMonitor(int selectedRow) {
    const bool oldLoading = loading;
    loading = true;
    for (int row = 0; row < table->rowCount(); ++row) {
        if (QCheckBox *monitor = checkBoxAt(table, row, MonitorColumn)) {
            const QSignalBlocker blocker(monitor);
            monitor->setChecked(row == selectedRow);
        }
    }
    loading = oldLoading;
}

int MultiVfoWidget::monitoredRow() const {
    for (int row = 0; row < table->rowCount(); ++row) {
        if (QCheckBox *monitor = checkBoxAt(table, row, MonitorColumn); monitor && monitor->isChecked()) {
            return row;
        }
    }
    return -1;
}

void MultiVfoWidget::updateSpectrum(const std::vector<float> &frequencies,
                                    const std::vector<float> &levels) {
    if (!isVisible() && (!mosaicDialog || !mosaicDialog->isVisible())) return;
    if (!enabledCheckBox->isChecked() || frequencies.empty() || frequencies.size() != levels.size()) return;
    if (spectrumUpdateTimer.isValid() && spectrumUpdateTimer.elapsed() < kUiUpdateIntervalMs) return;
    spectrumUpdateTimer.restart();
    if (smoothedLevels.size() != static_cast<std::size_t>(table->rowCount())) {
        smoothedLevels.assign(static_cast<std::size_t>(table->rowCount()),
                              std::numeric_limits<float>::quiet_NaN());
    }

    const bool ascending = frequencies.front() <= frequencies.back();
    if (!ascending) return;
    const QSignalBlocker blocker(table);
    const double visibleLow = frequencies.front();
    const double visibleHigh = frequencies.back();
    for (int row = 0; row < table->rowCount(); ++row) {
        QCheckBox *enabled = checkBoxAt(table, row, EnabledColumn);
        QDoubleSpinBox *frequency = doubleSpinAt(table, row, FrequencyColumn);
        QDoubleSpinBox *bandwidth = doubleSpinAt(table, row, BandwidthColumn);
        QDoubleSpinBox *squelch = doubleSpinAt(table, row, SquelchColumn);
        QTableWidgetItem *levelItem = table->item(row, LevelColumn);
        QTableWidgetItem *stateItem = table->item(row, StateColumn);
        if (!enabled || !frequency || !bandwidth || !squelch || !levelItem || !stateItem) continue;
        if (!enabled->isChecked()) {
            levelItem->setText(QStringLiteral("--"));
            stateItem->setText(ukrainian ? QStringLiteral("Вимкнено") : QStringLiteral("Off"));
            stateItem->setForeground(QColor(128, 128, 128));
            continue;
        }

        const double center = frequency->value() * 1.0e6;
        const double halfBandwidth = bandwidth->value() * 500.0;
        const double low = center - halfBandwidth;
        const double high = center + halfBandwidth;
        if (high < visibleLow || low > visibleHigh) {
            levelItem->setText(QStringLiteral("--"));
            stateItem->setText(ukrainian ? QStringLiteral("Поза смугою") : QStringLiteral("Out of span"));
            stateItem->setForeground(QColor(176, 112, 48));
            continue;
        }

        auto begin = std::lower_bound(frequencies.begin(), frequencies.end(), static_cast<float>(low));
        auto end = std::upper_bound(frequencies.begin(), frequencies.end(), static_cast<float>(high));
        if (begin == end) {
            begin = std::lower_bound(frequencies.begin(), frequencies.end(), static_cast<float>(center));
            if (begin == frequencies.end()) --begin;
            end = begin + 1;
        }
        const std::size_t first = static_cast<std::size_t>(std::distance(frequencies.begin(), begin));
        const std::size_t last = static_cast<std::size_t>(std::distance(frequencies.begin(), end));
        float peak = -200.0f;
        for (std::size_t index = first; index < last && index < levels.size(); ++index) {
            const std::size_t shiftedIndex = (index + levels.size() / 2U) % levels.size();
            if (std::isfinite(levels[shiftedIndex])) peak = (std::max)(peak, levels[shiftedIndex]);
        }
        float &smoothed = smoothedLevels[static_cast<std::size_t>(row)];
        smoothed = std::isfinite(smoothed) ? smoothed + 0.28f * (peak - smoothed) : peak;
        levelItem->setText(QStringLiteral("%1 dB").arg(smoothed, 0, 'f', 1));
        const bool open = smoothed >= static_cast<float>(squelch->value());
        stateItem->setText(open
            ? (ukrainian ? QStringLiteral("Сигнал") : QStringLiteral("Signal"))
            : (ukrainian ? QStringLiteral("Тиша") : QStringLiteral("Quiet")));
        stateItem->setForeground(open ? QColor(32, 170, 88) : QColor(128, 128, 128));
    }
    updateStatus();
    if (mosaicDialog && mosaicDialog->isVisible()) {
        for (QWidget *widget : mosaicViews) {
            if (auto *preview = dynamic_cast<MultiVfoPreview *>(widget)) {
                preview->setSpectrum(frequencies, levels);
            }
        }
    }
}

QString MultiVfoWidget::configurationJson() const {
    QJsonObject root;
    root.insert(QStringLiteral("version"), 2);
    root.insert(QStringLiteral("enabled"), enabledCheckBox->isChecked());
    root.insert(QStringLiteral("viewMode"), viewModeCombo->currentData().toInt());
    QJsonArray channels;
    for (int row = 0; row < table->rowCount(); ++row) {
        QCheckBox *enabled = checkBoxAt(table, row, EnabledColumn);
        QCheckBox *monitor = checkBoxAt(table, row, MonitorColumn);
        QTableWidgetItem *name = table->item(row, NameColumn);
        QDoubleSpinBox *frequency = doubleSpinAt(table, row, FrequencyColumn);
        QDoubleSpinBox *bandwidth = doubleSpinAt(table, row, BandwidthColumn);
        QComboBox *modulation = comboAt(table, row, ModulationColumn);
        QDoubleSpinBox *squelch = doubleSpinAt(table, row, SquelchColumn);
        if (!enabled || !monitor || !name || !frequency || !bandwidth || !modulation || !squelch) {
            continue;
        }
        QJsonObject channel;
        channel.insert(QStringLiteral("enabled"), enabled->isChecked());
        channel.insert(QStringLiteral("monitor"), monitor->isChecked());
        channel.insert(QStringLiteral("id"), name->data(kChannelIdRole).toString());
        channel.insert(QStringLiteral("name"), name->text());
        channel.insert(QStringLiteral("frequencyHz"), frequency->value() * 1.0e6);
        channel.insert(QStringLiteral("bandwidthHz"), bandwidth->value() * 1000.0);
        channel.insert(QStringLiteral("modulation"), modulation->currentData().toInt());
        channel.insert(QStringLiteral("squelchDb"), squelch->value());
        channel.insert(QStringLiteral("minimumDbfs"),
                       name->data(kMinimumDbfsRole).isValid()
                           ? name->data(kMinimumDbfsRole).toInt()
                           : -140);
        channel.insert(QStringLiteral("maximumDbfs"),
                       name->data(kMaximumDbfsRole).isValid()
                           ? name->data(kMaximumDbfsRole).toInt()
                           : -40);
        channels.append(channel);
    }
    root.insert(QStringLiteral("channels"), channels);
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

bool MultiVfoWidget::setConfigurationJson(const QString &json) {
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(json.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return false;
    const QJsonObject root = document.object();
    const QJsonArray channels = root.value(QStringLiteral("channels")).toArray();
    loading = true;
    table->setRowCount(0);
    enabledCheckBox->setChecked(root.value(QStringLiteral("enabled")).toBool(false));
    const int viewModeIndex = viewModeCombo->findData(root.value(QStringLiteral("viewMode")).toInt(2));
    if (viewModeIndex >= 0) viewModeCombo->setCurrentIndex(viewModeIndex);
    int requestedMonitor = -1;
    for (const QJsonValue &value : channels) {
        if (!value.isObject() || table->rowCount() >= kMaximumVfos) continue;
        const QJsonObject channel = value.toObject();
        addChannel(channel.value(QStringLiteral("frequencyHz")).toDouble(receiverListeningHz),
                   channel.value(QStringLiteral("bandwidthHz")).toDouble(12500.0),
                   channel.value(QStringLiteral("modulation")).toInt(MOD_NFM),
                   channel.value(QStringLiteral("name")).toString(),
                   channel.value(QStringLiteral("id")).toString());
        const int row = table->rowCount() - 1;
        checkBoxAt(table, row, EnabledColumn)->setChecked(channel.value(QStringLiteral("enabled")).toBool(true));
        doubleSpinAt(table, row, SquelchColumn)->setValue(channel.value(QStringLiteral("squelchDb")).toDouble(-95.0));
        if (QTableWidgetItem *name = table->item(row, NameColumn)) {
            int minimumDbfs = channel.value(QStringLiteral("minimumDbfs")).toInt(-140);
            int maximumDbfs = channel.value(QStringLiteral("maximumDbfs")).toInt(-40);
            minimumDbfs = std::clamp(minimumDbfs, -200, 19);
            maximumDbfs = std::clamp(maximumDbfs, minimumDbfs + 1, 20);
            name->setData(kMinimumDbfsRole, minimumDbfs);
            name->setData(kMaximumDbfsRole, maximumDbfs);
        }
        if (channel.value(QStringLiteral("monitor")).toBool(false)) requestedMonitor = row;
    }
    if (requestedMonitor >= 0) enforceSingleMonitor(requestedMonitor);
    smoothedLevels.assign(static_cast<std::size_t>(table->rowCount()),
                          std::numeric_limits<float>::quiet_NaN());
    loading = false;
    updateStatus();
    if (mosaicDialog) updateMosaicConfiguration();
    return true;
}

void MultiVfoWidget::openMosaic() {
    if (!mosaicDialog) {
        mosaicDialog = new QDialog(this, Qt::Window);
        mosaicDialog->setAttribute(Qt::WA_DeleteOnClose, false);
        mosaicDialog->resize(920, 650);
        auto *root = new QVBoxLayout(mosaicDialog);
        auto *scroll = new QScrollArea(mosaicDialog);
        scroll->setWidgetResizable(true);
        auto *content = new QWidget(scroll);
        mosaicLayout = new QGridLayout(content);
        mosaicLayout->setContentsMargins(6, 6, 6, 6);
        mosaicLayout->setSpacing(6);
        scroll->setWidget(content);
        root->addWidget(scroll);
    }
    mosaicDialog->setWindowTitle(ukrainian ? QStringLiteral("Multi-VFO мозаїка") : QStringLiteral("Multi-VFO mosaic"));
    rebuildMosaic();
    mosaicDialog->show();
    mosaicDialog->raise();
    mosaicDialog->activateWindow();
}

void MultiVfoWidget::openMosaicLevelEditor(int row) {
    if (row < 0 || row >= table->rowCount()) return;
    QTableWidgetItem *name = table->item(row, NameColumn);
    if (!name) return;

    QWidget *dialogParent = mosaicDialog
                                ? static_cast<QWidget *>(mosaicDialog)
                                : static_cast<QWidget *>(this);
    auto *dialog = new QDialog(dialogParent, Qt::Tool);
    dialog->setAttribute(Qt::WA_DeleteOnClose, true);
    dialog->setModal(false);
    dialog->setWindowFlag(Qt::WindowStaysOnTopHint, true);
    dialog->setWindowTitle(ukrainian
        ? QStringLiteral("Рівні мозаїки - %1").arg(name->text())
        : QStringLiteral("Mosaic levels - %1").arg(name->text()));
    dialog->setMinimumWidth(380);
    auto *root = new QVBoxLayout(dialog);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(7);

    auto *minimumSlider = new QSlider(Qt::Horizontal, dialog);
    auto *maximumSlider = new QSlider(Qt::Horizontal, dialog);
    auto *minimumLabel = new QLabel(dialog);
    auto *maximumLabel = new QLabel(dialog);
    minimumSlider->setRange(-200, 19);
    maximumSlider->setRange(-199, 20);
    minimumSlider->setValue(name->data(kMinimumDbfsRole).isValid()
                                ? name->data(kMinimumDbfsRole).toInt()
                                : -140);
    maximumSlider->setValue(name->data(kMaximumDbfsRole).isValid()
                                ? name->data(kMaximumDbfsRole).toInt()
                                : -40);
    if (minimumSlider->value() >= maximumSlider->value()) {
        minimumSlider->setValue((std::max)(minimumSlider->minimum(),
                                           maximumSlider->value() - 1));
    }
    const auto addSliderRow = [root, dialog](const QString &caption,
                                             QSlider *slider,
                                             QLabel *valueLabel) {
        auto *rowWidget = new QWidget(dialog);
        auto *rowLayout = new QHBoxLayout(rowWidget);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(6);
        auto *captionLabel = new QLabel(caption, rowWidget);
        captionLabel->setMinimumWidth(78);
        valueLabel->setMinimumWidth(62);
        valueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        rowLayout->addWidget(captionLabel);
        rowLayout->addWidget(slider, 1);
        rowLayout->addWidget(valueLabel);
        root->addWidget(rowWidget);
    };
    addSliderRow(ukrainian ? QStringLiteral("Мінімум:") : QStringLiteral("Minimum:"),
                 minimumSlider,
                 minimumLabel);
    addSliderRow(ukrainian ? QStringLiteral("Максимум:") : QStringLiteral("Maximum:"),
                 maximumSlider,
                 maximumLabel);
    const auto updateLabels = [minimumSlider, maximumSlider, minimumLabel, maximumLabel]() {
        minimumLabel->setText(QStringLiteral("%1 dBFS").arg(minimumSlider->value()));
        maximumLabel->setText(QStringLiteral("%1 dBFS").arg(maximumSlider->value()));
    };
    const QString editedChannelId = name->data(kChannelIdRole).toString();
    const auto apply = [this, editedChannelId, minimumSlider, maximumSlider]() {
        for (int channelRow = 0; channelRow < table->rowCount(); ++channelRow) {
            QTableWidgetItem *channelName = table->item(channelRow, NameColumn);
            if (!channelName || channelName->data(kChannelIdRole).toString() != editedChannelId) continue;
            {
                const QSignalBlocker blocker(table);
                channelName->setData(kMinimumDbfsRole, minimumSlider->value());
                channelName->setData(kMaximumDbfsRole, maximumSlider->value());
            }
            emitConfigurationChanged();
            return;
        }
    };
    connect(minimumSlider, &QSlider::valueChanged, dialog,
            [minimumSlider, maximumSlider, updateLabels, apply](int value) {
                if (value >= maximumSlider->value()) {
                    const QSignalBlocker blocker(maximumSlider);
                    maximumSlider->setValue((std::min)(maximumSlider->maximum(), value + 1));
                }
                updateLabels();
                apply();
            });
    connect(maximumSlider, &QSlider::valueChanged, dialog,
            [minimumSlider, maximumSlider, updateLabels, apply](int value) {
                if (value <= minimumSlider->value()) {
                    const QSignalBlocker blocker(minimumSlider);
                    minimumSlider->setValue((std::max)(minimumSlider->minimum(), value - 1));
                }
                updateLabels();
                apply();
            });
    auto *closeButton = new QPushButton(ukrainian ? QStringLiteral("Закрити")
                                                  : QStringLiteral("Close"),
                                        dialog);
    connect(closeButton, &QPushButton::clicked, dialog, &QDialog::close);
    root->addWidget(closeButton);
    updateLabels();
    dialog->show();
    dialog->adjustSize();
    dialog->raise();
}

void MultiVfoWidget::rebuildMosaic() {
    if (!mosaicLayout) return;
    while (QLayoutItem *item = mosaicLayout->takeAt(0)) {
        if (QWidget *widget = item->widget()) widget->deleteLater();
        delete item;
    }
    mosaicViews.clear();
    const int mode = viewModeCombo->currentData().toInt();
    int viewIndex = 0;
    for (int row = 0; row < table->rowCount(); ++row) {
        QCheckBox *enabled = checkBoxAt(table, row, EnabledColumn);
        QTableWidgetItem *name = table->item(row, NameColumn);
        QDoubleSpinBox *frequency = doubleSpinAt(table, row, FrequencyColumn);
        QDoubleSpinBox *bandwidth = doubleSpinAt(table, row, BandwidthColumn);
        if (!enabled || !name || !frequency || !bandwidth || !enabled->isChecked()) continue;
        auto *preview = new MultiVfoPreview(
            mode,
            [this](int channelRow) { openMosaicLevelEditor(channelRow); },
            mosaicDialog);
        preview->setToolTip(ukrainian
            ? QStringLiteral("Подвійний клік: налаштувати Min/Max dBFS")
            : QStringLiteral("Double-click: adjust Min/Max dBFS"));
        preview->setChannel(name->data(kChannelIdRole).toString(),
                            row,
                            name->text(),
                            frequency->value() * 1.0e6,
                            bandwidth->value() * 1000.0);
        preview->setDbfsRange(
            name->data(kMinimumDbfsRole).isValid()
                ? static_cast<float>(name->data(kMinimumDbfsRole).toInt())
                : -140.0f,
            name->data(kMaximumDbfsRole).isValid()
                ? static_cast<float>(name->data(kMaximumDbfsRole).toInt())
                : -40.0f);
        mosaicLayout->addWidget(preview, viewIndex / 2, viewIndex % 2);
        mosaicViews.push_back(preview);
        ++viewIndex;
    }
    if (viewIndex == 0) {
        auto *empty = new QLabel(ukrainian ? QStringLiteral("Немає увімкнених VFO") : QStringLiteral("No enabled VFOs"), mosaicDialog);
        empty->setAlignment(Qt::AlignCenter);
        mosaicLayout->addWidget(empty, 0, 0);
        mosaicViews.push_back(empty);
    }
}

void MultiVfoWidget::updateMosaicConfiguration() {
    if (!mosaicLayout) return;
    const int mode = viewModeCombo->currentData().toInt();

    struct ChannelView {
        int row = -1;
        QString id;
    };
    std::vector<ChannelView> enabledChannels;
    enabledChannels.reserve(static_cast<std::size_t>(table->rowCount()));
    for (int row = 0; row < table->rowCount(); ++row) {
        QCheckBox *enabled = checkBoxAt(table, row, EnabledColumn);
        QTableWidgetItem *name = table->item(row, NameColumn);
        if (enabled && name && enabled->isChecked()) {
            enabledChannels.push_back({row, name->data(kChannelIdRole).toString()});
        }
    }

    QHash<QString, MultiVfoPreview *> reusable;
    bool incompatibleMode = false;
    for (QWidget *widget : mosaicViews) {
        if (auto *preview = dynamic_cast<MultiVfoPreview *>(widget)) {
            incompatibleMode = incompatibleMode || preview->mode() != mode || preview->channelId().isEmpty();
            if (!preview->channelId().isEmpty()) reusable.insert(preview->channelId(), preview);
        }
    }
    if (incompatibleMode) {
        rebuildMosaic();
        return;
    }

    const auto configurePreview = [this](MultiVfoPreview *preview, const ChannelView &channel) {
        QTableWidgetItem *name = table->item(channel.row, NameColumn);
        QDoubleSpinBox *frequency = doubleSpinAt(table, channel.row, FrequencyColumn);
        QDoubleSpinBox *bandwidth = doubleSpinAt(table, channel.row, BandwidthColumn);
        if (!name || !frequency || !bandwidth) return;
        preview->setChannel(channel.id, channel.row, name->text(),
                            frequency->value() * 1.0e6, bandwidth->value() * 1000.0);
        preview->setDbfsRange(
            name->data(kMinimumDbfsRole).isValid()
                ? static_cast<float>(name->data(kMinimumDbfsRole).toInt()) : -140.0f,
            name->data(kMaximumDbfsRole).isValid()
                ? static_cast<float>(name->data(kMaximumDbfsRole).toInt()) : -40.0f);
    };
    bool unchangedLayout = mosaicViews.size() == enabledChannels.size();
    for (std::size_t index = 0; unchangedLayout && index < enabledChannels.size(); ++index) {
        auto *preview = dynamic_cast<MultiVfoPreview *>(mosaicViews[index]);
        unchangedLayout = preview && preview->channelId() == enabledChannels[index].id;
    }
    if (unchangedLayout) {
        for (std::size_t index = 0; index < enabledChannels.size(); ++index)
            configurePreview(static_cast<MultiVfoPreview *>(mosaicViews[index]), enabledChannels[index]);
        return;
    }

    while (QLayoutItem *item = mosaicLayout->takeAt(0)) delete item;
    std::vector<QWidget *> updatedViews;
    updatedViews.reserve((std::max)(std::size_t(1), enabledChannels.size()));

    int viewIndex = 0;
    for (const ChannelView &channel : enabledChannels) {
        QTableWidgetItem *name = table->item(channel.row, NameColumn);
        QDoubleSpinBox *frequency = doubleSpinAt(table, channel.row, FrequencyColumn);
        QDoubleSpinBox *bandwidth = doubleSpinAt(table, channel.row, BandwidthColumn);
        if (!name || !frequency || !bandwidth) continue;

        MultiVfoPreview *preview = reusable.take(channel.id);
        if (!preview) {
            preview = new MultiVfoPreview(
                mode,
                [this](int channelRow) { openMosaicLevelEditor(channelRow); },
                mosaicDialog);
            preview->setToolTip(ukrainian
                ? QStringLiteral("Подвійний клік: налаштувати Min/Max dBFS")
                : QStringLiteral("Double-click: adjust Min/Max dBFS"));
        }
        configurePreview(preview, channel);
        mosaicLayout->addWidget(preview, viewIndex / 2, viewIndex % 2);
        updatedViews.push_back(preview);
        ++viewIndex;
    }

    for (MultiVfoPreview *removed : reusable) {
        removed->hide();
        removed->deleteLater();
    }
    for (QWidget *oldWidget : mosaicViews) {
        if (!dynamic_cast<MultiVfoPreview *>(oldWidget)) {
            oldWidget->hide();
            oldWidget->deleteLater();
        }
    }
    if (updatedViews.empty()) {
        auto *empty = new QLabel(ukrainian ? QStringLiteral("Немає увімкнених VFO")
                                           : QStringLiteral("No enabled VFOs"), mosaicDialog);
        empty->setAlignment(Qt::AlignCenter);
        mosaicLayout->addWidget(empty, 0, 0);
        updatedViews.push_back(empty);
    }
    mosaicViews = std::move(updatedViews);
}
int MultiVfoWidget::addSelection(double lowFrequencyHz, double highFrequencyHz) {
    if (!std::isfinite(lowFrequencyHz) || !std::isfinite(highFrequencyHz) ||
        table->rowCount() >= kMaximumVfos) return -1;
    if (highFrequencyHz < lowFrequencyHz) std::swap(lowFrequencyHz, highFrequencyHz);
    const double minimumWidth = (std::max)(100.0, receiverBandwidthHz);
    double bandwidthHz = highFrequencyHz - lowFrequencyHz;
    double centerHz = (lowFrequencyHz + highFrequencyHz) * 0.5;
    if (bandwidthHz < 100.0) {
        centerHz = lowFrequencyHz;
        bandwidthHz = minimumWidth;
    }
    {
        const QSignalBlocker blocker(enabledCheckBox);
        enabledCheckBox->setChecked(true);
    }
    const int row = table->rowCount();
    addChannel(centerHz,
               (std::clamp)(bandwidthHz, 100.0, 20000000.0),
               receiverModulationType);
    emitConfigurationChanged();
    return row;
}

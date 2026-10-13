#include "dspbranchsettingswidget.h"

#include "audiofilterchainwidget.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QSlider>
#include <QVBoxLayout>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#elif defined(FOBOSAPP_HAS_QT_AUDIO)
#include <QAudioDeviceInfo>
#endif

#include <algorithm>
#include <cmath>

namespace {

double number(const QJsonObject &settings, const char *key, double fallback) {
    const double value = settings.value(QLatin1String(key)).toDouble(fallback);
    return std::isfinite(value) ? value : fallback;
}

void populateOutputDevices(QComboBox *combo) {
#ifdef Q_OS_WIN
    const UINT count = (std::min)(waveOutGetNumDevs(), UINT(256));
    for (UINT index = 0; index < count; ++index) {
        WAVEOUTCAPSW capabilities{};
        if (waveOutGetDevCapsW(index, &capabilities, sizeof(capabilities)) == MMSYSERR_NOERROR)
            combo->addItem(QString::fromWCharArray(capabilities.szPname), int(index));
    }
#elif defined(FOBOSAPP_HAS_QT_AUDIO)
    const auto devices = QAudioDeviceInfo::availableDevices(QAudio::AudioOutput);
    for (int index = 0; index < (std::min)(devices.size(), 256); ++index)
        combo->addItem(devices.at(index).deviceName(), index);
#else
    Q_UNUSED(combo);
#endif
}

}

bool DspBranchSettingsWidget::supportsType(const QString &type) {
    return type == QStringLiteral("pfb_channelizer") || type == QStringLiteral("branch_demodulator") ||
           type == QStringLiteral("branch_audio_filter") || type == QStringLiteral("branch_audio_output");
}

QString DspBranchSettingsWidget::blockTitle(const QString &type, bool ukrainian) {
    if (type == QStringLiteral("pfb_channelizer"))
        return ukrainian ? QStringLiteral("PFB-каналізатор") : QStringLiteral("PFB channelizer");
    if (type == QStringLiteral("branch_demodulator"))
        return ukrainian ? QStringLiteral("Демодулятор гілки") : QStringLiteral("Branch demodulator");
    if (type == QStringLiteral("branch_audio_filter"))
        return ukrainian ? QStringLiteral("Аудіофільтри гілки") : QStringLiteral("Branch audio filters");
    if (type == QStringLiteral("branch_audio_output"))
        return ukrainian ? QStringLiteral("Аудіовихід гілки") : QStringLiteral("Branch audio output");
    return {};
}

DspBranchSettingsWidget::DspBranchSettingsWidget(const QString &type, const QJsonObject &settings,
                                               bool ukrainian, QWidget *parent)
    : QWidget(parent) {
    const auto text = [ukrainian](const char *uk, const char *en) {
        return QString::fromUtf8(ukrainian ? uk : en);
    };
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    auto *form = new QFormLayout();
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setHorizontalSpacing(10);
    form->setVerticalSpacing(7);
    root->addLayout(form);

    const auto choice = [this, form](const QString &label, const char *key, const QList<int> &values,
                                     int current, int fallback) {
        auto *combo = new QComboBox(this);
        combo->setObjectName(QString::fromLatin1(key));
        for (int value : values) combo->addItem(QString::number(value), value);
        int selected = combo->findData(current);
        if (selected < 0) selected = combo->findData(fallback);
        combo->setCurrentIndex(selected);
        form->addRow(label, combo);
        connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this, combo, key](int) { emit settingsEdited(QJsonObject{{QLatin1String(key), combo->currentData().toInt()}}); });
        return combo;
    };
    const auto frequency = [this, form, &settings](const QString &label, const char *key,
                                                   double fallback, double low, double high,
                                                   double divisor, const QString &suffix) {
        auto *spin = new QDoubleSpinBox(this);
        spin->setObjectName(QString::fromLatin1(key));
        spin->setDecimals(divisor == 1.0 ? 1 : 3);
        spin->setRange(low / divisor, high / divisor);
        spin->setSuffix(suffix);
        spin->setKeyboardTracking(false);
        spin->setValue(number(settings, key, fallback) / divisor);
        form->addRow(label, spin);
        connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
                [this, key, divisor](double value) { emit settingsEdited(QJsonObject{{QLatin1String(key), value * divisor}}); });
        return spin;
    };

    if (type == QStringLiteral("pfb_channelizer")) {
        choice(text("Кількість каналів:", "Channels:"), "channels", {8, 16, 32, 64},
               settings.value(QStringLiteral("channels")).toInt(16), 16);
        QList<int> taps;
        for (int count = 8; count <= 64; count += 2) taps.append(count);
        choice(text("Коефіцієнтів на фазу:", "Taps per phase:"), "tapsPerPhase", taps,
               settings.value(QStringLiteral("tapsPerPhase")).toInt(16), 16);
    } else if (type == QStringLiteral("branch_demodulator")) {
        auto *modulation = new QComboBox(this);
        modulation->setObjectName(QStringLiteral("modulation"));
        const QStringList names = {QStringLiteral("AM"), QStringLiteral("NFM"), QStringLiteral("SAM"),
                                   QStringLiteral("USB"), QStringLiteral("LSB"), QStringLiteral("DSB"),
                                   QStringLiteral("CW"), QStringLiteral("WFM")};
        for (int index = 0; index < names.size(); ++index) modulation->addItem(names.at(index), index);
        const int selected = modulation->findData(settings.value(QStringLiteral("modulation")).toInt(1));
        modulation->setCurrentIndex(selected < 0 ? 1 : selected);
        form->addRow(text("Модуляція:", "Modulation:"), modulation);
        connect(modulation, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this, modulation](int) {
                    emit settingsEdited(QJsonObject{{QStringLiteral("modulation"), modulation->currentData().toInt()}});
                });
        frequency(text("Смуга демодуляції:", "Demodulation bandwidth:"), "bandwidthHz", 12500.0,
                  100.0, 250000.0, 1000.0, text(" кГц", " kHz"));
        frequency(text("Аудіо ФНЧ:", "Audio low-pass:"), "lowPassHz", 3000.0,
                  100.0, 20000.0, 1000.0, text(" кГц", " kHz"));
        frequency(text("Шумоподавлювач:", "Squelch:"), "squelchDb", -160.0,
                  -200.0, 0.0, 1.0, QStringLiteral(" dBFS"));
    } else if (type == QStringLiteral("branch_audio_filter")) {
        auto *chain = new AudioFilterChainWidget(this);
        chain->setObjectName(QStringLiteral("branchAudioFilters"));
        chain->setLanguage(ukrainian);
        chain->setConfigurationJson(settings.value(QStringLiteral("filters")).toString());
        root->addWidget(chain, 1);
        connect(chain, &AudioFilterChainWidget::configurationChanged, this, [this](const QString &json) {
            emit settingsEdited(QJsonObject{{QStringLiteral("filters"), json}});
        });
    } else if (type == QStringLiteral("branch_audio_output")) {
        auto *enabled = new QCheckBox(text("Увімкнути аудіовихід", "Enable audio output"), this);
        enabled->setObjectName(QStringLiteral("enabled"));
        enabled->setChecked(settings.value(QStringLiteral("enabled")).toBool(false));
        form->addRow(enabled);
        connect(enabled, &QCheckBox::toggled, this, [this](bool value) {
            emit settingsEdited(QJsonObject{{QStringLiteral("enabled"), value}});
        });

        auto *device = new QComboBox(this);
        device->setObjectName(QStringLiteral("device"));
        device->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        device->addItem(text("Системний аудіовихід", "System default output"), -1);
        populateOutputDevices(device);
        const int savedDevice = settings.value(QStringLiteral("device")).toInt(-1);
        if (device->findData(savedDevice) < 0)
            device->addItem(text("Недоступний пристрій #%1", "Unavailable device #%1").arg(savedDevice), savedDevice);
        device->setCurrentIndex(device->findData(savedDevice));
        form->addRow(text("Пристрій:", "Device:"), device);
        connect(device, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, device](int) {
            emit settingsEdited(QJsonObject{{QStringLiteral("device"), device->currentData().toInt()}});
        });

        auto *volumeRow = new QWidget(this);
        auto *volumeLayout = new QHBoxLayout(volumeRow);
        volumeLayout->setContentsMargins(0, 0, 0, 0);
        auto *volume = new QSlider(Qt::Horizontal, volumeRow);
        volume->setObjectName(QStringLiteral("volume"));
        volume->setRange(0, 100);
        volume->setValue(int(std::lround(std::clamp(number(settings, "volume", 0.5), 0.0, 1.0) * 100.0)));
        auto *level = new QLabel(QStringLiteral("%1%").arg(volume->value()), volumeRow);
        level->setMinimumWidth(level->fontMetrics().horizontalAdvance(QStringLiteral("100%")));
        level->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        volumeLayout->addWidget(volume, 1);
        volumeLayout->addWidget(level);
        form->addRow(text("Гучність:", "Volume:"), volumeRow);
        connect(volume, &QSlider::valueChanged, this, [this, level](int value) {
            level->setText(QStringLiteral("%1%").arg(value));
            emit settingsEdited(QJsonObject{{QStringLiteral("volume"), value / 100.0}});
        });
    }
}

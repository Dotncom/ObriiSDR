#include "spectrummarkerpairsdialog.h"

#include <QComboBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {
constexpr int kMaximumPairs = 10;
constexpr int kPairColumn = 0;
constexpr int kFirstMarkerColumn = 1;
constexpr int kSecondMarkerColumn = 2;
constexpr int kFirstFrequencyColumn = 3;
constexpr int kFirstLevelColumn = 4;
constexpr int kSecondFrequencyColumn = 5;
constexpr int kSecondLevelColumn = 6;
constexpr int kDeltaFrequencyColumn = 7;
constexpr int kDeltaLevelColumn = 8;
}

SpectrumMarkerPairsDialog::SpectrumMarkerPairsDialog(bool useUkrainian, QWidget *parent)
    : QDialog(parent), ukrainian(useUkrainian) {
    setWindowTitle(ukrainian ? QStringLiteral("Пари маркерів")
                             : QStringLiteral("Marker pairs"));
    setWindowFlag(Qt::Window, true);
    resize(1040, 360);
    setMinimumSize(760, 260);

    auto *layout = new QVBoxLayout(this);
    auto *description = new QLabel(
        ukrainian
            ? QStringLiteral("Перший рядок задає діапазон розширених вимірювань. Інші пари обчислюються паралельно.")
            : QStringLiteral("The first row defines the detailed measurement range. Other pairs are calculated in parallel."),
        this);
    description->setWordWrap(true);
    layout->addWidget(description);

    table = new QTableWidget(0, 9, this);
    table->setHorizontalHeaderLabels(ukrainian
        ? QStringList{QStringLiteral("Пара"), QStringLiteral("Маркер 1"), QStringLiteral("Маркер 2"),
                      QStringLiteral("F1"), QStringLiteral("L1"), QStringLiteral("F2"),
                      QStringLiteral("L2"), QStringLiteral("Δf"), QStringLiteral("ΔL")}
        : QStringList{QStringLiteral("Pair"), QStringLiteral("Marker 1"), QStringLiteral("Marker 2"),
                      QStringLiteral("F1"), QStringLiteral("L1"), QStringLiteral("F2"),
                      QStringLiteral("L2"), QStringLiteral("Δf"), QStringLiteral("ΔL")});
    table->verticalHeader()->setVisible(false);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->horizontalHeader()->setSectionResizeMode(kPairColumn, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(kFirstMarkerColumn, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(kSecondMarkerColumn, QHeaderView::ResizeToContents);
    for (int column = kFirstFrequencyColumn; column <= kDeltaLevelColumn; ++column)
        table->horizontalHeader()->setSectionResizeMode(column, QHeaderView::Stretch);
    layout->addWidget(table, 1);

    auto *buttons = new QHBoxLayout();
    addButton = new QPushButton(ukrainian ? QStringLiteral("Додати пару")
                                         : QStringLiteral("Add pair"), this);
    removeButton = new QPushButton(ukrainian ? QStringLiteral("Видалити пару")
                                            : QStringLiteral("Remove pair"), this);
    auto *closeButton = new QPushButton(ukrainian ? QStringLiteral("Закрити")
                                                  : QStringLiteral("Close"), this);
    buttons->addWidget(addButton);
    buttons->addWidget(removeButton);
    buttons->addStretch(1);
    buttons->addWidget(closeButton);
    layout->addLayout(buttons);

    connect(addButton, &QPushButton::clicked, this, [this]() {
        if (table->rowCount() >= kMaximumPairs || currentMarkers.size() < 2) return;
        addPair(0, 1);
        table->selectRow(table->rowCount() - 1);
        emitConfigurationChanged();
    });
    connect(removeButton, &QPushButton::clicked, this, [this]() {
        if (table->rowCount() <= 1) return;
        int row = table->currentRow();
        if (row < 0) row = table->rowCount() - 1;
        table->removeRow(row);
        refreshRows();
        emitConfigurationChanged();
    });
    connect(closeButton, &QPushButton::clicked, this, &QDialog::hide);
}

QString SpectrumMarkerPairsDialog::frequencyText(double hz) const {
    if (!std::isfinite(hz)) return QStringLiteral("--");
    if (std::abs(hz) >= 1.0e9) return QStringLiteral("%1 GHz").arg(hz / 1.0e9, 0, 'f', 6);
    if (std::abs(hz) >= 1.0e6) return QStringLiteral("%1 MHz").arg(hz / 1.0e6, 0, 'f', 6);
    if (std::abs(hz) >= 1.0e3) return QStringLiteral("%1 kHz").arg(hz / 1.0e3, 0, 'f', 3);
    return QStringLiteral("%1 Hz").arg(hz, 0, 'f', 2);
}

void SpectrumMarkerPairsDialog::setMarkers(const QVector<SpectrumScienceMarker> &markers) {
    const bool labelsChanged = currentMarkers.size() != markers.size();
    currentMarkers = markers;
    if (table->rowCount() == 0) rebuildDefaults();
    if (labelsChanged) {
        rebuilding = true;
        for (int row = 0; row < table->rowCount(); ++row) {
            for (int column : {kFirstMarkerColumn, kSecondMarkerColumn}) {
                auto *combo = qobject_cast<QComboBox *>(table->cellWidget(row, column));
                if (!combo) continue;
                const int selected = combo->currentData().toInt();
                combo->clear();
                for (int index = 0; index < currentMarkers.size(); ++index)
                    combo->addItem(currentMarkers[index].label, index);
                combo->setCurrentIndex(std::max(0, combo->findData(selected)));
            }
        }
        rebuilding = false;
    }
    refreshRows();
}

void SpectrumMarkerPairsDialog::rebuildDefaults() {
    if (currentMarkers.size() < 2) return;
    rebuilding = true;
    table->setRowCount(0);
    addPair(0, 1);
    rebuilding = false;
    refreshRows();
}

void SpectrumMarkerPairsDialog::addPair(int firstMarker, int secondMarker) {
    if (currentMarkers.size() < 2 || table->rowCount() >= kMaximumPairs) return;
    const int row = table->rowCount();
    table->insertRow(row);
    auto *first = new QComboBox(table);
    auto *second = new QComboBox(table);
    for (int index = 0; index < currentMarkers.size(); ++index) {
        first->addItem(currentMarkers[index].label, index);
        second->addItem(currentMarkers[index].label, index);
    }
    first->setCurrentIndex(std::max(0, first->findData(std::clamp(firstMarker, 0, currentMarkers.size() - 1))));
    second->setCurrentIndex(std::max(0, second->findData(std::clamp(secondMarker, 0, currentMarkers.size() - 1))));
    table->setCellWidget(row, kFirstMarkerColumn, first);
    table->setCellWidget(row, kSecondMarkerColumn, second);
    connect(first, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { if (!rebuilding) { refreshRows(); emitConfigurationChanged(); } });
    connect(second, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { if (!rebuilding) { refreshRows(); emitConfigurationChanged(); } });
}

void SpectrumMarkerPairsDialog::refreshRows() {
    for (int row = 0; row < table->rowCount(); ++row) {
        auto *firstCombo = qobject_cast<QComboBox *>(table->cellWidget(row, kFirstMarkerColumn));
        auto *secondCombo = qobject_cast<QComboBox *>(table->cellWidget(row, kSecondMarkerColumn));
        if (!firstCombo || !secondCombo) continue;
        const int firstIndex = firstCombo->currentData().toInt();
        const int secondIndex = secondCombo->currentData().toInt();
        const bool indicesValid = firstIndex >= 0 && secondIndex >= 0 &&
                                  firstIndex < currentMarkers.size() && secondIndex < currentMarkers.size();
        const SpectrumScienceMarker first = indicesValid ? currentMarkers[firstIndex] : SpectrumScienceMarker{};
        const SpectrumScienceMarker second = indicesValid ? currentMarkers[secondIndex] : SpectrumScienceMarker{};
        const bool valid = indicesValid && first.enabled && second.enabled;
        const QString pairName = row == 0
            ? (ukrainian ? QStringLiteral("1 • діапазон") : QStringLiteral("1 • range"))
            : QString::number(row + 1);
        const QStringList values = {
            pairName,
            QString(), QString(),
            first.enabled ? frequencyText(first.frequencyHz) : QStringLiteral("--"),
            first.enabled ? QStringLiteral("%1 dB").arg(first.levelDb, 0, 'f', 2) : QStringLiteral("--"),
            second.enabled ? frequencyText(second.frequencyHz) : QStringLiteral("--"),
            second.enabled ? QStringLiteral("%1 dB").arg(second.levelDb, 0, 'f', 2) : QStringLiteral("--"),
            valid ? frequencyText(std::abs(second.frequencyHz - first.frequencyHz)) : QStringLiteral("--"),
            valid ? QStringLiteral("%1 dB").arg(second.levelDb - first.levelDb, 0, 'f', 2) : QStringLiteral("--")
        };
        for (int column : {kPairColumn, kFirstFrequencyColumn, kFirstLevelColumn,
                           kSecondFrequencyColumn, kSecondLevelColumn,
                           kDeltaFrequencyColumn, kDeltaLevelColumn}) {
            QTableWidgetItem *item = table->item(row, column);
            if (!item) {
                item = new QTableWidgetItem();
                table->setItem(row, column, item);
            }
            item->setText(values[column]);
        }
    }
    addButton->setEnabled(currentMarkers.size() >= 2 && table->rowCount() < kMaximumPairs);
    removeButton->setEnabled(table->rowCount() > 1);
}

void SpectrumMarkerPairsDialog::emitConfigurationChanged() {
    if (rebuilding) return;
    for (int row = 0; row < table->rowCount(); ++row) {
        auto *first = qobject_cast<QComboBox *>(table->cellWidget(row, kFirstMarkerColumn));
        auto *second = qobject_cast<QComboBox *>(table->cellWidget(row, kSecondMarkerColumn));
        if (!first || !second || first->currentData() != second->currentData() ||
            currentMarkers.size() < 2)
            continue;
        const QSignalBlocker blocker(second);
        const int nextMarker = (first->currentData().toInt() + 1) % currentMarkers.size();
        second->setCurrentIndex(std::max(0, second->findData(nextMarker)));
    }
    refreshRows();
    if (table->rowCount() > 0) {
        auto *first = qobject_cast<QComboBox *>(table->cellWidget(0, kFirstMarkerColumn));
        auto *second = qobject_cast<QComboBox *>(table->cellWidget(0, kSecondMarkerColumn));
        if (first && second) emit primaryPairChanged(first->currentData().toInt(),
                                                     second->currentData().toInt());
    }
    emit configurationChanged();
}

QJsonArray SpectrumMarkerPairsDialog::pairConfiguration() const {
    QJsonArray configuration;
    for (int row = 0; row < table->rowCount(); ++row) {
        auto *first = qobject_cast<QComboBox *>(table->cellWidget(row, kFirstMarkerColumn));
        auto *second = qobject_cast<QComboBox *>(table->cellWidget(row, kSecondMarkerColumn));
        if (!first || !second) continue;
        configuration.append(QJsonObject{{QStringLiteral("first"), first->currentData().toInt()},
                                         {QStringLiteral("second"), second->currentData().toInt()}});
    }
    return configuration;
}

void SpectrumMarkerPairsDialog::setPairConfiguration(const QJsonArray &configuration) {
    if (currentMarkers.size() < 2) return;
    rebuilding = true;
    table->setRowCount(0);
    for (const QJsonValue &value : configuration) {
        if (table->rowCount() >= kMaximumPairs) break;
        const QJsonObject pair = value.toObject();
        const int first = pair.value(QStringLiteral("first")).toInt(-1);
        const int second = pair.value(QStringLiteral("second")).toInt(-1);
        if (first < 0 || second < 0 || first >= currentMarkers.size() || second >= currentMarkers.size())
            continue;
        addPair(first, second);
    }
    rebuilding = false;
    if (table->rowCount() == 0) rebuildDefaults();
    refreshRows();
    emitConfigurationChanged();
}

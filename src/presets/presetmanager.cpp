#include "main.h"

#include "presethelpers.h"
#include "qthlocator.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QJsonDocument>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

QTableWidgetItem *calibrationConditionsItem(const ReceiverCalibrationContext &context) {
    auto *item = new QTableWidgetItem(context.description());
    item->setData(Qt::UserRole, context.toJson());
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    item->setToolTip(context.description());
    return item;
}

class NumericTableWidgetItem : public QTableWidgetItem {
public:
    using QTableWidgetItem::QTableWidgetItem;

    bool operator<(const QTableWidgetItem &other) const override {
        bool leftOk = false;
        bool rightOk = false;
        const double left = text().toDouble(&leftOk);
        const double right = other.text().toDouble(&rightOk);
        return leftOk && rightOk ? left < right : QTableWidgetItem::operator<(other);
    }
};

double medianValue(QVector<double> values) {
    if (values.isEmpty()) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    std::sort(values.begin(), values.end());
    const int middle = values.size() / 2;
    if ((values.size() & 1) != 0) {
        return values.at(middle);
    }
    return (values.at(middle - 1) + values.at(middle)) * 0.5;
}

} // namespace

void YourClassName::openPresetManager() {
    if (presetManagerOpen) {
        return;
    }
    QScopedValueRollback<bool> presetManagerGuard(presetManagerOpen, true);
    ensureDefaultFrequencyPresets();
    ensureDefaultBandMarkers();

    QDialog dialog(this);
    dialog.setWindowTitle(uiText(QStringLiteral("preset_manager"), QStringLiteral("Preset Manager")));
    dialog.resize(720, 520);

    QVBoxLayout *rootLayout = new QVBoxLayout(&dialog);
    QTabWidget *tabs = new QTabWidget(&dialog);

    auto moveSelectedTableRow = [](QTableWidget *table, int direction) {
        if (!table || direction == 0) {
            return;
        }
        const int row = table->currentRow();
        const int targetRow = row + direction;
        if (row < 0 || targetRow < 0 || targetRow >= table->rowCount()) {
            return;
        }

        const int columnCount = table->columnCount();
        for (int column = 0; column < columnCount; ++column) {
            QTableWidgetItem *current = table->takeItem(row, column);
            QTableWidgetItem *target = table->takeItem(targetRow, column);
            table->setItem(row, column, target);
            table->setItem(targetRow, column, current);
        }
        table->setCurrentCell(targetRow, 0);
        table->selectRow(targetRow);
    };

    auto addOrderButtons = [this, moveSelectedTableRow](QHBoxLayout *layout,
                                                        QTableWidget *table,
                                                        QWidget *page) {
        QToolButton *moveUpButton = new QToolButton(page);
        moveUpButton->setArrowType(Qt::UpArrow);
        moveUpButton->setToolTip(uiText(QStringLiteral("move_up"), QStringLiteral("Move up")));
        QToolButton *moveDownButton = new QToolButton(page);
        moveDownButton->setArrowType(Qt::DownArrow);
        moveDownButton->setToolTip(uiText(QStringLiteral("move_down"), QStringLiteral("Move down")));
        layout->addWidget(moveUpButton);
        layout->addWidget(moveDownButton);
        QObject::connect(moveUpButton, &QToolButton::clicked, table, [table, moveSelectedTableRow]() {
            moveSelectedTableRow(table, -1);
        });
        QObject::connect(moveDownButton, &QToolButton::clicked, table, [table, moveSelectedTableRow]() {
            moveSelectedTableRow(table, 1);
        });
    };

    auto makeNumericTab = [this, &dialog, addOrderButtons](const QMap<QString, double> &presets,
                                                           const QStringList &order,
                                                           const QString &valueHeader,
                                                           double minimum,
                                                           double maximum) -> QTableWidget * {
        QWidget *page = new QWidget(&dialog);
        QVBoxLayout *pageLayout = new QVBoxLayout(page);
        QTableWidget *table = new QTableWidget(page);
        table->setColumnCount(2);
        table->setHorizontalHeaderLabels({uiText(QStringLiteral("name"), QStringLiteral("Name")), valueHeader});
        table->horizontalHeader()->setStretchLastSection(true);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::SingleSelection);
        table->setRowCount(presets.size());
        int row = 0;
        for (const QString &name : normalizedPresetOrder(order, presets)) {
            auto it = presets.constFind(name);
            if (it == presets.constEnd()) {
                continue;
            }
            table->setItem(row, 0, new QTableWidgetItem(name));
            table->setItem(row, 1, new QTableWidgetItem(QString::number(it.value(), 'f', 3)));
            ++row;
        }
        table->setRowCount(row);

        QHBoxLayout *buttonLayout = new QHBoxLayout();
        QPushButton *addButton = new QPushButton(uiText(QStringLiteral("add"), QStringLiteral("Add")), page);
        QPushButton *removeButton = new QPushButton(uiText(QStringLiteral("remove"), QStringLiteral("Remove")), page);
        buttonLayout->addWidget(addButton);
        buttonLayout->addWidget(removeButton);
        addOrderButtons(buttonLayout, table, page);
        buttonLayout->addStretch();
        pageLayout->addWidget(table);
        pageLayout->addLayout(buttonLayout);

        QObject::connect(addButton, &QPushButton::clicked, table, [this, table, minimum]() {
            const int row = table->rowCount();
            table->insertRow(row);
            table->setItem(row, 0, new QTableWidgetItem(uiText(QStringLiteral("new_preset"), QStringLiteral("New preset"))));
            table->setItem(row, 1, new QTableWidgetItem(QString::number((std::max)(0.0, minimum), 'f', 3)));
            table->setCurrentCell(row, 0);
            table->editItem(table->item(row, 0));
        });
        QObject::connect(removeButton, &QPushButton::clicked, table, [this, table]() {
            const int row = table->currentRow();
            if (row >= 0) {
                table->removeRow(row);
            }
        });

        table->setProperty("minimumValue", minimum);
        table->setProperty("maximumValue", maximum);
        table->setProperty("pageWidget", QVariant::fromValue(static_cast<void*>(page)));
        return table;
    };

    auto makeAgileTab = [this, &dialog, addOrderButtons](const QMap<QString, QString> &presets,
                                                         const QStringList &order) -> QTableWidget * {
        QWidget *page = new QWidget(&dialog);
        QVBoxLayout *pageLayout = new QVBoxLayout(page);
        QTableWidget *table = new QTableWidget(page);
        table->setColumnCount(3);
        table->setHorizontalHeaderLabels({uiText(QStringLiteral("name"), QStringLiteral("Name")),
                                          uiText(QStringLiteral("ranges_mhz_plain"), QStringLiteral("Ranges MHz")),
                                          uiText(QStringLiteral("step_mhz"), QStringLiteral("Step MHz"))});
        table->horizontalHeader()->setStretchLastSection(true);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::SingleSelection);
        table->setRowCount(presets.size());
        int row = 0;
        for (const QString &name : normalizedPresetOrder(order, presets)) {
            auto it = presets.constFind(name);
            if (it == presets.constEnd()) {
                continue;
            }
            table->setItem(row, 0, new QTableWidgetItem(name));
            table->setItem(row, 1, new QTableWidgetItem(agileScanPresetRanges(it.value())));
            table->setItem(row, 2, new QTableWidgetItem(QString::number(agileScanPresetStepMhz(it.value(), 0.0125), 'f', 6)));
            ++row;
        }
        table->setRowCount(row);

        QHBoxLayout *buttonLayout = new QHBoxLayout();
        QPushButton *addButton = new QPushButton(uiText(QStringLiteral("add"), QStringLiteral("Add")), page);
        QPushButton *removeButton = new QPushButton(uiText(QStringLiteral("remove"), QStringLiteral("Remove")), page);
        buttonLayout->addWidget(addButton);
        buttonLayout->addWidget(removeButton);
        addOrderButtons(buttonLayout, table, page);
        buttonLayout->addStretch();
        pageLayout->addWidget(table);
        pageLayout->addLayout(buttonLayout);

        QObject::connect(addButton, &QPushButton::clicked, table, [this, table]() {
            const int row = table->rowCount();
            table->insertRow(row);
            table->setItem(row, 0, new QTableWidgetItem(uiText(QStringLiteral("new_scan_preset"), QStringLiteral("New scan preset"))));
            table->setItem(row, 1, new QTableWidgetItem(QStringLiteral("430-432")));
            table->setItem(row, 2, new QTableWidgetItem(QStringLiteral("0.0125")));
            table->setCurrentCell(row, 0);
            table->editItem(table->item(row, 0));
        });
        QObject::connect(removeButton, &QPushButton::clicked, table, [this, table]() {
            const int row = table->currentRow();
            if (row >= 0) {
                table->removeRow(row);
            }
        });

        table->setProperty("pageWidget", QVariant::fromValue(static_cast<void*>(page)));
        return table;
    };

    auto makeStandardScanTab = [this, &dialog, addOrderButtons](const QMap<QString, QString> &presets,
                                                                const QStringList &order) -> QTableWidget * {
        QWidget *page = new QWidget(&dialog);
        QVBoxLayout *pageLayout = new QVBoxLayout(page);
        QTableWidget *table = new QTableWidget(page);
        table->setColumnCount(4);
        table->setHorizontalHeaderLabels({uiText(QStringLiteral("name"), QStringLiteral("Name")),
                                          uiText(QStringLiteral("centers_mhz_plain"), QStringLiteral("Centers MHz")),
                                          uiText(QStringLiteral("dwell_ms"), QStringLiteral("Dwell ms")),
                                          uiText(QStringLiteral("settle_ms"), QStringLiteral("Settle ms"))});
        table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
        table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::SingleSelection);
        table->setRowCount(presets.size());
        int row = 0;
        for (const QString &name : normalizedPresetOrder(order, presets)) {
            auto it = presets.constFind(name);
            if (it == presets.constEnd()) {
                continue;
            }
            table->setItem(row, 0, new QTableWidgetItem(name));
            table->setItem(row, 1, new QTableWidgetItem(standardScanPresetCenters(it.value())));
            table->setItem(row, 2, new QTableWidgetItem(QString::number(standardScanPresetDwellMs(it.value(), standardScanDwellMs))));
            table->setItem(row, 3, new QTableWidgetItem(QString::number(standardScanPresetSettleMs(it.value(), standardScanSettleMs))));
            ++row;
        }
        table->setRowCount(row);

        QHBoxLayout *buttonLayout = new QHBoxLayout();
        QPushButton *addButton = new QPushButton(uiText(QStringLiteral("add"), QStringLiteral("Add")), page);
        QPushButton *removeButton = new QPushButton(uiText(QStringLiteral("remove"), QStringLiteral("Remove")), page);
        buttonLayout->addWidget(addButton);
        buttonLayout->addWidget(removeButton);
        addOrderButtons(buttonLayout, table, page);
        buttonLayout->addStretch();
        pageLayout->addWidget(table);
        pageLayout->addLayout(buttonLayout);

        QObject::connect(addButton, &QPushButton::clicked, table, [this, table]() {
            const int row = table->rowCount();
            table->insertRow(row);
            table->setItem(row, 0, new QTableWidgetItem(uiText(QStringLiteral("new_standard_scan_preset"),
                                                               QStringLiteral("New standard scan preset"))));
            table->setItem(row, 1, new QTableWidgetItem(standardScanCentersMhz));
            table->setItem(row, 2, new QTableWidgetItem(QString::number(standardScanDwellMs)));
            table->setItem(row, 3, new QTableWidgetItem(QString::number(standardScanSettleMs)));
            table->setCurrentCell(row, 0);
            table->editItem(table->item(row, 0));
        });
        QObject::connect(removeButton, &QPushButton::clicked, table, [table]() {
            const int row = table->currentRow();
            if (row >= 0) {
                table->removeRow(row);
            }
        });

        table->setProperty("pageWidget", QVariant::fromValue(static_cast<void*>(page)));
        return table;
    };

    auto makeListeningScanTab = [this, &dialog, addOrderButtons](const QMap<QString, QString> &presets,
                                                                 const QStringList &order) -> QTableWidget * {
        QWidget *page = new QWidget(&dialog);
        QVBoxLayout *pageLayout = new QVBoxLayout(page);
        QTableWidget *table = new QTableWidget(page);
        table->setColumnCount(4);
        table->setHorizontalHeaderLabels({uiText(QStringLiteral("name"), QStringLiteral("Name")),
                                          uiText(QStringLiteral("targets_mhz_plain"), QStringLiteral("Targets MHz")),
                                          uiText(QStringLiteral("dwell_ms"), QStringLiteral("Dwell ms")),
                                          uiText(QStringLiteral("settle_ms"), QStringLiteral("Settle ms"))});
        table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
        table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::SingleSelection);
        table->setRowCount(presets.size());
        int row = 0;
        for (const QString &name : normalizedPresetOrder(order, presets)) {
            auto it = presets.constFind(name);
            if (it == presets.constEnd()) {
                continue;
            }
            table->setItem(row, 0, new QTableWidgetItem(name));
            table->setItem(row, 1, new QTableWidgetItem(listeningScanPresetTargets(it.value())));
            table->setItem(row, 2, new QTableWidgetItem(QString::number(listeningScanPresetDwellMs(it.value(), listeningScanDwellMs))));
            table->setItem(row, 3, new QTableWidgetItem(QString::number(listeningScanPresetSettleMs(it.value(), listeningScanSettleMs))));
            ++row;
        }
        table->setRowCount(row);

        QHBoxLayout *buttonLayout = new QHBoxLayout();
        QPushButton *addButton = new QPushButton(uiText(QStringLiteral("add"), QStringLiteral("Add")), page);
        QPushButton *removeButton = new QPushButton(uiText(QStringLiteral("remove"), QStringLiteral("Remove")), page);
        buttonLayout->addWidget(addButton);
        buttonLayout->addWidget(removeButton);
        addOrderButtons(buttonLayout, table, page);
        buttonLayout->addStretch();
        pageLayout->addWidget(table);
        pageLayout->addLayout(buttonLayout);

        QObject::connect(addButton, &QPushButton::clicked, table, [this, table]() {
            const int row = table->rowCount();
            table->insertRow(row);
            table->setItem(row, 0, new QTableWidgetItem(uiText(QStringLiteral("new_listening_scan_preset"),
                                                               QStringLiteral("New listening scan preset"))));
            table->setItem(row, 1, new QTableWidgetItem(listeningScanTargetsMhz));
            table->setItem(row, 2, new QTableWidgetItem(QString::number(listeningScanDwellMs)));
            table->setItem(row, 3, new QTableWidgetItem(QString::number(listeningScanSettleMs)));
            table->setCurrentCell(row, 0);
            table->editItem(table->item(row, 0));
        });
        QObject::connect(removeButton, &QPushButton::clicked, table, [table]() {
            const int row = table->currentRow();
            if (row >= 0) {
                table->removeRow(row);
            }
        });

        table->setProperty("pageWidget", QVariant::fromValue(static_cast<void*>(page)));
        return table;
    };

    auto makeBandMarkerTab = [this, &dialog](const QVector<GraphBandMarker> &markers) -> QTableWidget * {
        QWidget *page = new QWidget(&dialog);
        QVBoxLayout *pageLayout = new QVBoxLayout(page);
        QTableWidget *table = new QTableWidget(page);
        table->setColumnCount(4);
        table->setHorizontalHeaderLabels({uiText(QStringLiteral("layer"), QStringLiteral("Layer")),
                                          uiText(QStringLiteral("label"), QStringLiteral("Label")),
                                          uiText(QStringLiteral("start_mhz"), QStringLiteral("Start MHz")),
                                          uiText(QStringLiteral("end_mhz"), QStringLiteral("End MHz"))});
        table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
        table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::SingleSelection);

        auto setLayerCell = [this, table](int row, bool amateur) {
            QComboBox *layerCombo = new QComboBox(table);
            layerCombo->addItem(uiText(QStringLiteral("common"), QStringLiteral("Common")), QStringLiteral("general"));
            layerCombo->addItem(QStringLiteral("HAM"), QStringLiteral("amateur"));
            layerCombo->setCurrentIndex(amateur ? 1 : 0);
            table->setCellWidget(row, 0, layerCombo);
        };
        auto setBandRow = [table, setLayerCell](int row, const GraphBandMarker &marker) {
            setLayerCell(row, marker.amateur);
            table->setItem(row, 1, new QTableWidgetItem(marker.label));
            table->setItem(row, 2, new QTableWidgetItem(QString::number(marker.startHz / 1000000.0, 'f', 6)));
            table->setItem(row, 3, new QTableWidgetItem(QString::number(marker.endHz / 1000000.0, 'f', 6)));
        };

        table->setRowCount(markers.size());
        for (int row = 0; row < markers.size(); ++row) {
            setBandRow(row, markers.at(row));
        }

        QHBoxLayout *buttonLayout = new QHBoxLayout();
        QPushButton *addCommonButton = new QPushButton(uiText(QStringLiteral("add_common"), QStringLiteral("Add common")), page);
        QPushButton *addHamButton = new QPushButton(uiText(QStringLiteral("add_ham"), QStringLiteral("Add HAM")), page);
        QPushButton *removeButton = new QPushButton(uiText(QStringLiteral("remove"), QStringLiteral("Remove")), page);
        buttonLayout->addWidget(addCommonButton);
        buttonLayout->addWidget(addHamButton);
        buttonLayout->addWidget(removeButton);
        buttonLayout->addStretch();
        pageLayout->addWidget(table);
        pageLayout->addLayout(buttonLayout);

        auto addBandRow = [this, table, setBandRow](bool amateur) {
            const int row = table->rowCount();
            table->insertRow(row);
            GraphBandMarker marker;
            marker.label = amateur ? uiText(QStringLiteral("new_ham_band"), QStringLiteral("New HAM band"))
                                   : uiText(QStringLiteral("new_band"), QStringLiteral("New band"));
            marker.startHz = amateur ? 144000000.0 : 118000000.0;
            marker.endHz = amateur ? 146000000.0 : 137000000.0;
            marker.amateur = amateur;
            setBandRow(row, marker);
            table->setCurrentCell(row, 1);
            table->editItem(table->item(row, 1));
        };
        QObject::connect(addCommonButton, &QPushButton::clicked, table, [addBandRow]() {
            addBandRow(false);
        });
        QObject::connect(addHamButton, &QPushButton::clicked, table, [addBandRow]() {
            addBandRow(true);
        });
        QObject::connect(removeButton, &QPushButton::clicked, table, [table]() {
            const int row = table->currentRow();
            if (row >= 0) {
                table->removeRow(row);
            }
        });

        table->setProperty("pageWidget", QVariant::fromValue(static_cast<void*>(page)));
        return table;
    };

    auto makeQthMarkerTab = [this, &dialog](const QVector<qth::UserMarker> &markers) -> QTableWidget * {
        QWidget *page = new QWidget(&dialog);
        QVBoxLayout *pageLayout = new QVBoxLayout(page);
        QTableWidget *table = new QTableWidget(page);
        table->setColumnCount(5);
        table->setHorizontalHeaderLabels({uiText(QStringLiteral("number"), QStringLiteral("Number")),
                                          uiText(QStringLiteral("name"), QStringLiteral("Name")),
                                          uiText(QStringLiteral("latitude"), QStringLiteral("Latitude")),
                                          uiText(QStringLiteral("longitude"), QStringLiteral("Longitude")),
                                          uiText(QStringLiteral("description"), QStringLiteral("Description"))});
        table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
        table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::SingleSelection);

        auto setMarkerRow = [table](int row, const qth::UserMarker &marker) {
            table->setItem(row, 0, new QTableWidgetItem(QString::number(marker.number)));
            table->setItem(row, 1, new QTableWidgetItem(marker.name));
            table->setItem(row, 2, new QTableWidgetItem(QString::number(marker.latitude, 'f', 6)));
            table->setItem(row, 3, new QTableWidgetItem(QString::number(marker.longitude, 'f', 6)));
            table->setItem(row, 4, new QTableWidgetItem(marker.description));
        };

        table->setRowCount(markers.size());
        for (int row = 0; row < markers.size(); ++row) {
            setMarkerRow(row, markers.at(row));
        }

        QHBoxLayout *buttonLayout = new QHBoxLayout();
        QPushButton *addButton = new QPushButton(uiText(QStringLiteral("add_marker"), QStringLiteral("Add marker")), page);
        QPushButton *removeButton = new QPushButton(uiText(QStringLiteral("remove"), QStringLiteral("Remove")), page);
        buttonLayout->addWidget(addButton);
        buttonLayout->addWidget(removeButton);
        buttonLayout->addStretch();
        pageLayout->addWidget(table);
        pageLayout->addLayout(buttonLayout);

        QObject::connect(addButton, &QPushButton::clicked, table, [this, table, setMarkerRow]() {
            int nextNumber = 1;
            for (int row = 0; row < table->rowCount(); ++row) {
                bool ok = false;
                const int number = table->item(row, 0) ? table->item(row, 0)->text().toInt(&ok) : 0;
                if (ok) {
                    nextNumber = (std::max)(nextNumber, number + 1);
                }
            }
            qth::UserMarker marker;
            marker.number = nextNumber;
            marker.latitude = qthLatitude;
            marker.longitude = qthLongitude;
            marker.name = qth::maidenheadLocator(marker.latitude, marker.longitude, 6);
            const int row = table->rowCount();
            table->insertRow(row);
            setMarkerRow(row, marker);
            table->setCurrentCell(row, 1);
            table->editItem(table->item(row, 1));
        });
        QObject::connect(removeButton, &QPushButton::clicked, table, [table]() {
            const int row = table->currentRow();
            if (row >= 0) {
                table->removeRow(row);
            }
        });

        table->setProperty("pageWidget", QVariant::fromValue(static_cast<void*>(page)));
        return table;
    };

    QCheckBox *calibrationEnabledCheckbox = nullptr;
    auto makeCalibrationTab = [this, &dialog, &calibrationEnabledCheckbox]() -> QTableWidget * {
        QWidget *page = new QWidget(&dialog);
        QVBoxLayout *pageLayout = new QVBoxLayout(page);
        calibrationEnabledCheckbox = new QCheckBox(
            uiText(QStringLiteral("calibration_table_enabled"), QStringLiteral("Use frequency-dependent calibration table")),
            page);
        calibrationEnabledCheckbox->setChecked(calibrationTableEnabled);
        calibrationEnabledCheckbox->setToolTip(uiText(
            QStringLiteral("calibration_table_enabled_tooltip"),
            QStringLiteral("Linearly interpolate frequency and amplitude corrections between calibration points. Outside the table range, use the nearest endpoint.")));
        pageLayout->addWidget(calibrationEnabledCheckbox);

        QTableWidget *table = new QTableWidget(page);
        table->setColumnCount(6);
        table->setHorizontalHeaderLabels({
            uiText(QStringLiteral("calibration_frequency_mhz"), QStringLiteral("Frequency MHz")),
            uiText(QStringLiteral("calibration_frequency_offset_hz"), QStringLiteral("Frequency correction Hz")),
            uiText(QStringLiteral("calibration_amplitude_offset_db"), QStringLiteral("Amplitude correction dB")),
            uiText(QStringLiteral("calibration_uncertainty_db"), QStringLiteral("Uncertainty +/- dB")),
            uiText(QStringLiteral("note"), QStringLiteral("Note")),
            uiText(QStringLiteral("calibration_conditions"), QStringLiteral("Receiver conditions"))
        });
        table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
        table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
        table->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Interactive);
        table->setColumnWidth(5, 200);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::SingleSelection);
        table->setSortingEnabled(false);
        table->setRowCount(receiverCalibrationTable.points().size());
        int row = 0;
        for (const ReceiverCalibrationPoint &point : receiverCalibrationTable.points()) {
            table->setItem(row, 0, new NumericTableWidgetItem(QString::number(point.frequencyHz / 1000000.0, 'f', 6)));
            table->setItem(row, 1, new QTableWidgetItem(QString::number(point.frequencyOffsetHz, 'f', 3)));
            table->setItem(row, 2, new QTableWidgetItem(QString::number(point.amplitudeOffsetDb, 'f', 3)));
            table->setItem(row, 3, new QTableWidgetItem(QString::number(point.uncertaintyDb, 'f', 2)));
            table->setItem(row, 4, new QTableWidgetItem(point.note));
            table->setItem(row, 5, calibrationConditionsItem(point.conditions));
            ++row;
        }

        QLabel *interpolationHint = new QLabel(
            uiText(QStringLiteral("calibration_table_hint"),
                   QStringLiteral("Corrections are added to the global calibration offsets. Use generator reference frequency and level; points are sorted automatically when saved.")),
            page);
        interpolationHint->setWordWrap(true);

        QHBoxLayout *buttonLayout = new QHBoxLayout();
        QPushButton *addButton = new QPushButton(uiText(QStringLiteral("add"), QStringLiteral("Add")), page);
        QPushButton *removeButton = new QPushButton(uiText(QStringLiteral("remove"), QStringLiteral("Remove")), page);
        QPushButton *sortButton = new QPushButton(
            uiText(QStringLiteral("sort_by_frequency"), QStringLiteral("Sort by frequency")), page);
        QPushButton *wizardButton = new QPushButton(
            uiText(QStringLiteral("calibration_wizard"), QStringLiteral("Calibration wizard...")), page);
        buttonLayout->addWidget(addButton);
        buttonLayout->addWidget(removeButton);
        buttonLayout->addWidget(sortButton);
        buttonLayout->addWidget(wizardButton);
        auto *conditionsButton = new QPushButton(uiText("calibration_conditions_edit", "Conditions..."), page);
        buttonLayout->addWidget(conditionsButton);
        QObject::connect(conditionsButton, &QPushButton::clicked, table, [this, table]() {
            const int row = table->currentRow();
            if (row < 0) return;
            const auto old = ReceiverCalibrationContext::fromJson(table->item(row, 5)
                ? table->item(row, 5)->data(Qt::UserRole).toJsonObject() : QJsonObject{});
            QDialog editor(table);
            editor.setWindowTitle(uiText("calibration_conditions", "Receiver conditions"));
            auto *form = new QFormLayout(&editor);
            auto *backend = new QComboBox(&editor);
            backend->setEditable(true);
            backend->addItems({"*", "fobos-standard", "fobos-agile", "rtl-native", "rtl-tcp", "hackrf-native", "soapy"});
            backend->setCurrentText(old.backendId.isEmpty() ? "*" : old.backendId);
            form->addRow(uiText("receiver", "Receiver:"), backend);
            const auto integer = [&](const QString &label, int value) {
                auto *spin = new QSpinBox(&editor);
                spin->setRange(-1, 1000); spin->setSpecialValueText("*"); spin->setValue(value);
                form->addRow(label, spin); return spin;
            };
            auto *input = integer(uiText("calibration_input", "Input mode:"), old.inputMode);
            auto *rate = new QDoubleSpinBox(&editor);
            rate->setRange(0, 100000); rate->setDecimals(6); rate->setSpecialValueText("*"); rate->setSuffix(" MS/s");
            rate->setValue(old.sampleRateHz/1e6); form->addRow(uiText("sample_rate", "Sample rate:"), rate);
            auto *lna = integer("LNA", old.lnaGain);
            auto *vga = integer("VGA", old.vgaGain);
            auto *rtl = integer(uiText("calibration_rtl_gain", "RTL gain (0.1 dB):"), old.rtlGainTenthsDb);
            auto *state = integer(uiText("calibration_gain_state", "AGC / RF amp (0/1):"), old.gainState);
            state->setMaximum(1);
            auto *bind = new QPushButton(uiText("calibration_bind_current", "Use current receiver conditions"), &editor);
            form->addRow(bind);
            auto *warning = new QLabel(uiText("calibration_agc_warning", "For absolute levels disable AGC and keep the calibrated hardware gain. An AGC state is not a known gain."), &editor);
            warning->setWordWrap(true); form->addRow(warning);
            QObject::connect(bind, &QPushButton::clicked, &editor, [=]() {
                const auto c = receiverCalibrationContext(pendingSettings);
                backend->setCurrentText(c.backendId); input->setValue(c.inputMode); rate->setValue(c.sampleRateHz/1e6);
                lna->setValue(c.lnaGain); vga->setValue(c.vgaGain); rtl->setValue(c.rtlGainTenthsDb); state->setValue(c.gainState);
            });
            auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &editor);
            form->addRow(buttons);
            QObject::connect(buttons, &QDialogButtonBox::accepted, &editor, &QDialog::accept);
            QObject::connect(buttons, &QDialogButtonBox::rejected, &editor, &QDialog::reject);
            if (editor.exec() != QDialog::Accepted) return;
            ReceiverCalibrationContext c;
            c.backendId = backend->currentText().trimmed(); if (c.backendId == "*") c.backendId.clear();
            c.inputMode = input->value(); c.sampleRateHz = rate->value()*1e6;
            c.lnaGain = lna->value(); c.vgaGain = vga->value(); c.rtlGainTenthsDb = rtl->value(); c.gainState = state->value();
            table->setItem(row, 5, calibrationConditionsItem(c));
        });
        buttonLayout->addStretch();
        pageLayout->addWidget(table);
        pageLayout->addWidget(interpolationHint);
        pageLayout->addLayout(buttonLayout);

        QObject::connect(addButton, &QPushButton::clicked, table, [this, table]() {
            const int row = table->rowCount();
            table->insertRow(row);
            table->setItem(row, 0, new NumericTableWidgetItem(
                QString::number((std::max)(0.0, pendingSettings.centerFrequency) / 1000000.0, 'f', 6)));
            table->setItem(row, 1, new QTableWidgetItem(QStringLiteral("0.000")));
            table->setItem(row, 2, new QTableWidgetItem(QStringLiteral("0.000")));
            table->setItem(row, 3, new QTableWidgetItem(QStringLiteral("0.00")));
            table->setItem(row, 4, new QTableWidgetItem());
            table->setItem(row, 5, calibrationConditionsItem(receiverCalibrationContext(pendingSettings)));
            table->setCurrentCell(row, 0);
            table->editItem(table->item(row, 0));
        });
        QObject::connect(removeButton, &QPushButton::clicked, table, [table]() {
            if (table->currentRow() >= 0) table->removeRow(table->currentRow());
        });
        QObject::connect(sortButton, &QPushButton::clicked, table, [table]() {
            table->setSortingEnabled(true);
            table->sortItems(0, Qt::AscendingOrder);
            table->setSortingEnabled(false);
        });
        QObject::connect(wizardButton, &QPushButton::clicked, page,
                         [this, page, table, calibrationEnabledCheckbox]() {
            QDialog wizard(page);
            wizard.setWindowTitle(uiText(QStringLiteral("calibration_wizard"),
                                         QStringLiteral("Calibration wizard")));
            wizard.setMinimumWidth(460);

            QVBoxLayout *layout = new QVBoxLayout(&wizard);
            QLabel *intro = new QLabel(
                uiText(QStringLiteral("calibration_wizard_intro"),
                       QStringLiteral("Connect a stable generator, enter its reference frequency and level, then capture several live spectrum frames.")),
                &wizard);
            intro->setWordWrap(true);
            layout->addWidget(intro);

            QFormLayout *form = new QFormLayout();
            QDoubleSpinBox *frequencySpin = new QDoubleSpinBox(&wizard);
            frequencySpin->setDecimals(6);
            frequencySpin->setRange(0.0, PRESET_RF_EXPERIMENTAL_MAX_FREQUENCY / 1000000.0);
            const double seedFrequency = pendingSettings.listeningFrequency > 0.0
                                             ? pendingSettings.listeningFrequency
                                             : pendingSettings.centerFrequency;
            frequencySpin->setValue((std::max)(0.0, seedFrequency) / 1000000.0);
            frequencySpin->setSuffix(QStringLiteral(" MHz"));

            QDoubleSpinBox *levelSpin = new QDoubleSpinBox(&wizard);
            levelSpin->setDecimals(2);
            levelSpin->setRange(-200.0, 50.0);
            levelSpin->setValue(-60.0);
            levelSpin->setSuffix(QStringLiteral(" dBm"));

            QDoubleSpinBox *spanSpin = new QDoubleSpinBox(&wizard);
            spanSpin->setDecimals(1);
            spanSpin->setRange(0.1, 10000.0);
            spanSpin->setValue(20.0);
            spanSpin->setSuffix(QStringLiteral(" kHz"));

            QSpinBox *framesSpin = new QSpinBox(&wizard);
            framesSpin->setRange(1, 64);
            framesSpin->setValue(8);

            QDoubleSpinBox *uncertaintySpin = new QDoubleSpinBox(&wizard);
            uncertaintySpin->setDecimals(2);
            uncertaintySpin->setRange(0.0, 100.0);
            uncertaintySpin->setValue(1.0);
            uncertaintySpin->setSuffix(QStringLiteral(" dB"));

            QLineEdit *noteEdit = new QLineEdit(&wizard);
            noteEdit->setPlaceholderText(uiText(QStringLiteral("calibration_wizard_note_placeholder"),
                                                QStringLiteral("Generator, cable, gain, temperature...")));

            form->addRow(uiText(QStringLiteral("calibration_reference_frequency"), QStringLiteral("Reference frequency")), frequencySpin);
            form->addRow(uiText(QStringLiteral("calibration_reference_level"), QStringLiteral("Reference level")), levelSpin);
            form->addRow(uiText(QStringLiteral("calibration_search_span"), QStringLiteral("Peak search span")), spanSpin);
            form->addRow(uiText(QStringLiteral("calibration_capture_frames"), QStringLiteral("Capture frames")), framesSpin);
            form->addRow(uiText(QStringLiteral("calibration_uncertainty_db"), QStringLiteral("Uncertainty +/- dB")), uncertaintySpin);
            form->addRow(uiText(QStringLiteral("note"), QStringLiteral("Note")), noteEdit);
            layout->addLayout(form);

            QProgressBar *progress = new QProgressBar(&wizard);
            progress->setRange(0, framesSpin->value());
            progress->setValue(0);
            layout->addWidget(progress);

            QLabel *resultLabel = new QLabel(
                uiText(QStringLiteral("calibration_wizard_ready"),
                       QStringLiteral("Ready to capture live spectrum frames.")),
                &wizard);
            resultLabel->setWordWrap(true);
            layout->addWidget(resultLabel);

            QHBoxLayout *actions = new QHBoxLayout();
            QPushButton *captureButton = new QPushButton(
                uiText(QStringLiteral("capture"), QStringLiteral("Capture")), &wizard);
            QPushButton *addPointButton = new QPushButton(
                uiText(QStringLiteral("calibration_add_point"), QStringLiteral("Add calibration point")), &wizard);
            addPointButton->setEnabled(false);
            actions->addWidget(captureButton);
            actions->addWidget(addPointButton);
            actions->addStretch();
            layout->addLayout(actions);

            QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &wizard);
            if (QPushButton *closeButton = buttons->button(QDialogButtonBox::Close)) {
                closeButton->setText(uiText(QStringLiteral("close"), QStringLiteral("Close")));
            }
            layout->addWidget(buttons);
            QObject::connect(buttons, &QDialogButtonBox::rejected, &wizard, &QDialog::reject);

            struct CaptureResult {
                bool valid = false;
                double referenceHz = 0.0;
                double observedHz = 0.0;
                double observedLevelDb = 0.0;
                double frequencyOffsetHz = 0.0;
                double amplitudeOffsetDb = 0.0;
            } result;

            QObject::connect(framesSpin, qOverload<int>(&QSpinBox::valueChanged), progress,
                             [progress](int count) {
                progress->setRange(0, count);
                progress->setValue(0);
            });

            QObject::connect(captureButton, &QPushButton::clicked, &wizard,
                             [this, &wizard, frequencySpin, levelSpin, spanSpin, framesSpin,
                              progress, resultLabel, captureButton, addPointButton, &result]() {
                result = CaptureResult{};
                addPointButton->setEnabled(false);
                captureButton->setEnabled(false);
                const double referenceHz = frequencySpin->value() * 1000000.0;
                const double halfSpanHz = spanSpin->value() * 500.0;
                const int targetFrames = framesSpin->value();
                progress->setRange(0, targetFrames);
                progress->setValue(0);

                QVector<double> peakFrequencies;
                QVector<double> peakLevels;
                std::uint64_t lastCapturedFrame = spectrumCalibrationFrameSerial;
                QEventLoop captureLoop;
                QTimer captureTimer;
                captureTimer.setInterval(80);
                QElapsedTimer timeout;
                timeout.start();
                QObject::connect(&captureTimer, &QTimer::timeout, &captureLoop, [&]() {
                    if (spectrumCalibrationFrameSerial != lastCapturedFrame &&
                        spectrumFrequencyScratch.size() == spectrumMagnitudeScratch.size() &&
                        !spectrumFrequencyScratch.empty()) {
                        lastCapturedFrame = spectrumCalibrationFrameSerial;
                        double peakLevel = -std::numeric_limits<double>::infinity();
                        double peakFrequency = 0.0;
                        for (size_t index = 0; index < spectrumFrequencyScratch.size(); ++index) {
                            const double frequency = spectrumFrequencyScratch[index];
                            const double level = spectrumMagnitudeScratch[index];
                            if (!std::isfinite(frequency) || !std::isfinite(level) ||
                                std::abs(frequency - referenceHz) > halfSpanHz) {
                                continue;
                            }
                            if (level > peakLevel) {
                                peakLevel = level;
                                peakFrequency = frequency;
                            }
                        }
                        if (std::isfinite(peakLevel)) {
                            peakFrequencies.append(peakFrequency);
                            peakLevels.append(peakLevel);
                            progress->setValue(peakLevels.size());
                        }
                    }
                    if (peakLevels.size() >= targetFrames || timeout.elapsed() >= 5000) {
                        captureLoop.quit();
                    }
                });
                captureTimer.start();
                captureLoop.exec();
                captureTimer.stop();
                captureButton->setEnabled(true);

                if (peakLevels.isEmpty()) {
                    resultLabel->setText(uiText(
                        QStringLiteral("calibration_wizard_no_peak"),
                        QStringLiteral("No finite peak was found in the selected span. Start the receiver or widen the search span.")));
                    return;
                }

                result.referenceHz = referenceHz;
                result.observedHz = medianValue(peakFrequencies);
                result.observedLevelDb = medianValue(peakLevels);
                double activeTableFrequencyOffset = 0.0;
                double activeTableAmplitudeOffset = 0.0;
                if (calibrationTableEnabled) {
                    const ReceiverCalibrationCorrection current = receiverCalibrationTable.correctionAt(referenceHz);
                    if (current.valid) {
                        activeTableFrequencyOffset = current.frequencyOffsetHz;
                        activeTableAmplitudeOffset = current.amplitudeOffsetDb;
                    }
                }
                result.frequencyOffsetHz = activeTableFrequencyOffset + referenceHz - result.observedHz;
                result.amplitudeOffsetDb = activeTableAmplitudeOffset + levelSpin->value() - result.observedLevelDb;
                result.valid = std::isfinite(result.frequencyOffsetHz) &&
                               std::isfinite(result.amplitudeOffsetDb);
                addPointButton->setEnabled(result.valid);
                resultLabel->setText(uiText(
                    QStringLiteral("calibration_wizard_result"),
                    QStringLiteral("Observed: %1 MHz, %2 dB. Proposed corrections: %3 Hz, %4 dB (%5/%6 frames)."))
                    .arg(result.observedHz / 1000000.0, 0, 'f', 6)
                    .arg(result.observedLevelDb, 0, 'f', 2)
                    .arg(result.frequencyOffsetHz, 0, 'f', 3)
                    .arg(result.amplitudeOffsetDb, 0, 'f', 3)
                    .arg(peakLevels.size())
                    .arg(targetFrames));
            });

            QObject::connect(addPointButton, &QPushButton::clicked, &wizard,
                             [this, table, calibrationEnabledCheckbox, uncertaintySpin, noteEdit,
                              addPointButton, &result]() {
                if (!result.valid) return;
                const int row = table->rowCount();
                table->insertRow(row);
                table->setItem(row, 0, new NumericTableWidgetItem(
                    QString::number(result.referenceHz / 1000000.0, 'f', 6)));
                table->setItem(row, 1, new QTableWidgetItem(
                    QString::number(result.frequencyOffsetHz, 'f', 3)));
                table->setItem(row, 2, new QTableWidgetItem(
                    QString::number(result.amplitudeOffsetDb, 'f', 3)));
                table->setItem(row, 3, new QTableWidgetItem(
                    QString::number(uncertaintySpin->value(), 'f', 2)));
                table->setItem(row, 4, new QTableWidgetItem(noteEdit->text().trimmed()));
                table->setItem(row, 5, calibrationConditionsItem(receiverCalibrationContext(pendingSettings)));
                table->setCurrentCell(row, 0);
                calibrationEnabledCheckbox->setChecked(true);
                addPointButton->setEnabled(false);
                result.valid = false;
            });

            wizard.exec();
        });

        table->setProperty("pageWidget", QVariant::fromValue(static_cast<void*>(page)));
        return table;
    };

    QTableWidget *centerTable = makeNumericTab(centerFrequencyPresets,
                                              centerFrequencyPresetOrder,
                                              uiText(QStringLiteral("frequency_hz"), QStringLiteral("Frequency Hz")),
                                              0.0,
                                              PRESET_RF_EXPERIMENTAL_MAX_FREQUENCY);
    QTableWidget *listeningTable = makeNumericTab(listeningFrequencyPresets,
                                                 listeningFrequencyPresetOrder,
                                                 uiText(QStringLiteral("frequency_hz"), QStringLiteral("Frequency Hz")),
                                                 -PRESET_RF_EXPERIMENTAL_MAX_FREQUENCY,
                                                 PRESET_RF_EXPERIMENTAL_MAX_FREQUENCY);
    QTableWidget *bandwidthTable = makeNumericTab(bandwidthValuePresets,
                                                 bandwidthPresetOrder,
                                                 uiText(QStringLiteral("bandwidth_hz"), QStringLiteral("Bandwidth Hz")),
                                                 1.0,
                                                 PRESET_BANDWIDTH_MAX_HZ);
    QTableWidget *agileTable = makeAgileTab(agileScanPresets, agileScanPresetOrder);
    QTableWidget *standardScanTable = makeStandardScanTab(standardScanPresets, standardScanPresetOrder);
    QTableWidget *listeningScanTable = makeListeningScanTab(listeningScanPresets, listeningScanPresetOrder);
    QTableWidget *bandMarkerTable = makeBandMarkerTab(bandMarkers);
    QTableWidget *qthMarkerTable = makeQthMarkerTab(qthUserMarkers);
    QTableWidget *calibrationTable = makeCalibrationTab();

    tabs->addTab(static_cast<QWidget*>(centerTable->property("pageWidget").value<void*>()),
                 uiText(QStringLiteral("preset_tab_center"), QStringLiteral("Center")));
    tabs->addTab(static_cast<QWidget*>(listeningTable->property("pageWidget").value<void*>()),
                 uiText(QStringLiteral("preset_tab_listen"), QStringLiteral("Listen")));
    tabs->addTab(static_cast<QWidget*>(bandwidthTable->property("pageWidget").value<void*>()),
                 uiText(QStringLiteral("preset_tab_audio_bw"), QStringLiteral("Audio BW")));
    tabs->addTab(static_cast<QWidget*>(agileTable->property("pageWidget").value<void*>()),
                 uiText(QStringLiteral("agile_scan"), QStringLiteral("Agile scan")));
    tabs->addTab(static_cast<QWidget*>(standardScanTable->property("pageWidget").value<void*>()),
                 uiText(QStringLiteral("preset_tab_standard_scan"), QStringLiteral("Standard scan")));
    tabs->addTab(static_cast<QWidget*>(listeningScanTable->property("pageWidget").value<void*>()),
                 uiText(QStringLiteral("preset_tab_listening_scan"), QStringLiteral("Listening scan")));
    tabs->addTab(static_cast<QWidget*>(bandMarkerTable->property("pageWidget").value<void*>()),
                 uiText(QStringLiteral("general_band_markers"), QStringLiteral("Band markers")));
    tabs->addTab(static_cast<QWidget*>(qthMarkerTable->property("pageWidget").value<void*>()),
                 uiText(QStringLiteral("preset_tab_qth_markers"), QStringLiteral("QTH markers")));
    tabs->addTab(static_cast<QWidget*>(calibrationTable->property("pageWidget").value<void*>()),
                 uiText(QStringLiteral("preset_tab_calibration"), QStringLiteral("Calibration")));

    QLabel *hintLabel = new QLabel(uiText(QStringLiteral("presets_hint"),
                                          QStringLiteral("Values are stored in Hz for frequency/audio presets. Scan presets and band-marker ranges are edited in MHz. HAM defaults are Region-1-style hints; edit them for local rules.")),
                                   &dialog);
    hintLabel->setWordWrap(true);
    QDialogButtonBox *buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    if (QPushButton *okButton = buttonBox->button(QDialogButtonBox::Ok)) {
        okButton->setText(uiText(QStringLiteral("ok"), QStringLiteral("OK")));
    }
    if (QPushButton *cancelButton = buttonBox->button(QDialogButtonBox::Cancel)) {
        cancelButton->setText(uiText(QStringLiteral("cancel"), QStringLiteral("Cancel")));
    }
    rootLayout->addWidget(tabs);
    rootLayout->addWidget(hintLabel);
    rootLayout->addWidget(buttonBox);

    auto tableNameOrder = [](QTableWidget *table) {
        QStringList order;
        if (!table) {
            return order;
        }
        for (int row = 0; row < table->rowCount(); ++row) {
            const QString name = table->item(row, 0) ? table->item(row, 0)->text().trimmed() : QString();
            if (!name.isEmpty() && !order.contains(name)) {
                order.append(name);
            }
        }
        return order;
    };

    auto readNumericTable = [](QTableWidget *table,
                               const QString &tabName,
                               QMap<QString, double> &target,
                               QString *error) {
        QMap<QString, double> next;
        const double minimum = table->property("minimumValue").toDouble();
        const double maximum = table->property("maximumValue").toDouble();
        for (int row = 0; row < table->rowCount(); ++row) {
            const QString name = table->item(row, 0) ? table->item(row, 0)->text().trimmed() : QString();
            const QString valueText = table->item(row, 1) ? table->item(row, 1)->text().trimmed() : QString();
            if (name.isEmpty() && valueText.isEmpty()) {
                continue;
            }
            bool ok = false;
            const double value = valueText.toDouble(&ok);
            if (name.isEmpty() || !ok || !std::isfinite(value) || value < minimum || value > maximum) {
                if (error) {
                    *error = QStringLiteral("%1: bad numeric preset at row %2 (%3 = %4, allowed %5..%6)")
                                 .arg(tabName,
                                      QString::number(row + 1),
                                      name.isEmpty() ? QStringLiteral("<empty>") : name,
                                      valueText,
                                      QString::number(minimum, 'f', 0),
                                      QString::number(maximum, 'f', 0));
                }
                return false;
            }
            next[name] = value;
        }
        target = next;
        return true;
    };

    auto readAgileTable = [](QTableWidget *table, QMap<QString, QString> &target, QString *error) {
        QMap<QString, QString> next;
        for (int row = 0; row < table->rowCount(); ++row) {
            const QString name = table->item(row, 0) ? table->item(row, 0)->text().trimmed() : QString();
            const QString ranges = table->item(row, 1) ? table->item(row, 1)->text().trimmed() : QString();
            const QString stepText = table->item(row, 2) ? table->item(row, 2)->text().trimmed() : QString();
            if (name.isEmpty() && ranges.isEmpty() && stepText.isEmpty()) {
                continue;
            }
            bool ok = false;
            const double step = stepText.toDouble(&ok);
            if (name.isEmpty() || ranges.isEmpty() || !ok ||
                !std::isfinite(step) ||
                step < PRESET_AGILE_SCAN_MIN_STEP_MHZ ||
                step > PRESET_AGILE_SCAN_MAX_STEP_MHZ) {
                if (error) {
                    *error = QStringLiteral("Bad Agile scan preset at row %1").arg(row + 1);
                }
                return false;
            }
            QString parseError;
            parseAgileScanFrequenciesMhz(ranges, step, &parseError);
            if (!parseError.isEmpty()) {
                if (error) {
                    *error = QStringLiteral("%1: %2").arg(name, parseError);
                }
                return false;
            }
            next[name] = agileScanPresetSpec(ranges, step);
        }
        target = next;
        return true;
    };

    auto readStandardScanTable = [this](QTableWidget *table, QMap<QString, QString> &target, QString *error) {
        QMap<QString, QString> next;
        for (int row = 0; row < table->rowCount(); ++row) {
            const QString name = table->item(row, 0) ? table->item(row, 0)->text().trimmed() : QString();
            const QString centers = table->item(row, 1) ? table->item(row, 1)->text().trimmed() : QString();
            const QString dwellText = table->item(row, 2) ? table->item(row, 2)->text().trimmed() : QString();
            const QString settleText = table->item(row, 3) ? table->item(row, 3)->text().trimmed() : QString();
            if (name.isEmpty() && centers.isEmpty() && dwellText.isEmpty() && settleText.isEmpty()) {
                continue;
            }
            bool dwellOk = false;
            bool settleOk = false;
            const int dwellMs = dwellText.toInt(&dwellOk);
            const int settleMs = settleText.toInt(&settleOk);
            if (name.isEmpty() ||
                centers.isEmpty() ||
                !dwellOk ||
                !settleOk ||
                dwellMs < PRESET_STANDARD_SCAN_MIN_DWELL_MS ||
                dwellMs > PRESET_STANDARD_SCAN_MAX_DWELL_MS ||
                settleMs < PRESET_STANDARD_SCAN_MIN_SETTLE_MS ||
                settleMs > PRESET_STANDARD_SCAN_MAX_SETTLE_MS) {
                if (error) {
                    *error = QStringLiteral("Bad standard scan preset at row %1").arg(row + 1);
                }
                return false;
            }
            QString parseError;
            parseStandardScanCentersMhz(centers,
                                        pendingSettings.sampleRate,
                                        PRESET_AGILE_SCAN_MIN_POINTS,
                                        &parseError,
                                        nullptr);
            if (!parseError.isEmpty()) {
                if (error) {
                    *error = QStringLiteral("%1: %2").arg(name, parseError);
                }
                return false;
            }
            next[name] = standardScanPresetSpec(centers, dwellMs, settleMs);
        }
        target = next;
        return true;
    };

    auto readListeningScanTable = [](QTableWidget *table, QMap<QString, QString> &target, QString *error) {
        QMap<QString, QString> next;
        for (int row = 0; row < table->rowCount(); ++row) {
            const QString name = table->item(row, 0) ? table->item(row, 0)->text().trimmed() : QString();
            const QString targets = table->item(row, 1) ? table->item(row, 1)->text().trimmed() : QString();
            const QString dwellText = table->item(row, 2) ? table->item(row, 2)->text().trimmed() : QString();
            const QString settleText = table->item(row, 3) ? table->item(row, 3)->text().trimmed() : QString();
            if (name.isEmpty() && targets.isEmpty() && dwellText.isEmpty() && settleText.isEmpty()) {
                continue;
            }
            bool dwellOk = false;
            bool settleOk = false;
            const int dwellMs = dwellText.toInt(&dwellOk);
            const int settleMs = settleText.toInt(&settleOk);
            if (name.isEmpty() ||
                targets.isEmpty() ||
                !dwellOk ||
                !settleOk ||
                dwellMs < PRESET_LISTENING_SCAN_MIN_DWELL_MS ||
                dwellMs > PRESET_LISTENING_SCAN_MAX_DWELL_MS ||
                settleMs < PRESET_LISTENING_SCAN_MIN_SETTLE_MS ||
                settleMs > PRESET_LISTENING_SCAN_MAX_SETTLE_MS) {
                if (error) {
                    *error = QStringLiteral("Bad listening scan preset at row %1").arg(row + 1);
                }
                return false;
            }
            QString parseError;
            parseListeningScanTargetsMhz(targets, 0.0, PRESET_RF_EXPERIMENTAL_MAX_FREQUENCY, 1, &parseError);
            if (!parseError.isEmpty()) {
                if (error) {
                    *error = QStringLiteral("%1: %2").arg(name, parseError);
                }
                return false;
            }
            next[name] = listeningScanPresetSpec(targets, dwellMs, settleMs);
        }
        target = next;
        return true;
    };

    auto readBandMarkerTable = [](QTableWidget *table, QVector<GraphBandMarker> &target, QString *error) {
        QVector<GraphBandMarker> next;
        for (int row = 0; row < table->rowCount(); ++row) {
            QComboBox *layerCombo = qobject_cast<QComboBox*>(table->cellWidget(row, 0));
            const QString layer = layerCombo ? layerCombo->currentData().toString() : QStringLiteral("general");
            const QString label = table->item(row, 1) ? table->item(row, 1)->text().trimmed() : QString();
            const QString startText = table->item(row, 2) ? table->item(row, 2)->text().trimmed() : QString();
            const QString endText = table->item(row, 3) ? table->item(row, 3)->text().trimmed() : QString();
            if (label.isEmpty() && startText.isEmpty() && endText.isEmpty()) {
                continue;
            }

            bool startOk = false;
            bool endOk = false;
            const double startMhz = startText.toDouble(&startOk);
            const double endMhz = endText.toDouble(&endOk);
            if (label.isEmpty() ||
                !startOk ||
                !endOk ||
                !std::isfinite(startMhz) ||
                !std::isfinite(endMhz) ||
                startMhz < 0.0 ||
                endMhz <= startMhz ||
                endMhz > 100000.0) {
                if (error) {
                    *error = QStringLiteral("Bad band marker at row %1").arg(row + 1);
                }
                return false;
            }

            GraphBandMarker marker;
            marker.startHz = startMhz * 1000000.0;
            marker.endHz = endMhz * 1000000.0;
            marker.label = label;
            marker.amateur = layer == QStringLiteral("amateur");
            next.append(marker);
        }
        target = next;
        return true;
    };

    auto readQthMarkerTable = [this](QTableWidget *table, QVector<qth::UserMarker> &target, QString *error) {
        QVector<qth::UserMarker> next;
        QVector<int> usedNumbers;
        for (int row = 0; row < table->rowCount(); ++row) {
            const QString numberText = table->item(row, 0) ? table->item(row, 0)->text().trimmed() : QString();
            QString name = table->item(row, 1) ? table->item(row, 1)->text().trimmed() : QString();
            const QString latText = table->item(row, 2) ? table->item(row, 2)->text().trimmed() : QString();
            const QString lonText = table->item(row, 3) ? table->item(row, 3)->text().trimmed() : QString();
            const QString description = table->item(row, 4) ? table->item(row, 4)->text().trimmed() : QString();
            if (numberText.isEmpty() && name.isEmpty() && latText.isEmpty() && lonText.isEmpty() && description.isEmpty()) {
                continue;
            }

            bool numberOk = false;
            bool latOk = false;
            bool lonOk = false;
            const int number = numberText.toInt(&numberOk);
            const double latitude = latText.toDouble(&latOk);
            const double longitude = lonText.toDouble(&lonOk);
            if (!numberOk ||
                number <= 0 ||
                usedNumbers.contains(number) ||
                !latOk ||
                !lonOk ||
                !qth::isValidLatitude(latitude) ||
                !qth::isValidLongitude(longitude)) {
                if (error) {
                    *error = uiText(QStringLiteral("bad_qth_marker_row"),
                                    QStringLiteral("Bad QTH marker at row %1"))
                                 .arg(row + 1);
                }
                return false;
            }

            usedNumbers.append(number);
            if (name.isEmpty()) {
                name = qth::maidenheadLocator(latitude, longitude, 6);
            }

            qth::UserMarker marker;
            marker.number = number;
            marker.name = name.left(80);
            marker.description = description.left(512);
            marker.latitude = latitude;
            marker.longitude = longitude;
            next.append(marker);
        }
        target = next;
        return true;
    };

    auto readCalibrationTable = [this](QTableWidget *table,
                                       QVector<ReceiverCalibrationPoint> &target,
                                       QString *error) {
        QVector<ReceiverCalibrationPoint> next;
        for (int row = 0; row < table->rowCount(); ++row) {
            const QString frequencyText = table->item(row, 0) ? table->item(row, 0)->text().trimmed() : QString();
            const QString frequencyOffsetText = table->item(row, 1) ? table->item(row, 1)->text().trimmed() : QString();
            const QString amplitudeOffsetText = table->item(row, 2) ? table->item(row, 2)->text().trimmed() : QString();
            const QString uncertaintyText = table->item(row, 3) ? table->item(row, 3)->text().trimmed() : QString();
            const QString note = table->item(row, 4) ? table->item(row, 4)->text().trimmed() : QString();
            if (frequencyText.isEmpty() && frequencyOffsetText.isEmpty() &&
                amplitudeOffsetText.isEmpty() && uncertaintyText.isEmpty() && note.isEmpty()) {
                continue;
            }
            bool frequencyOk = false;
            bool frequencyOffsetOk = false;
            bool amplitudeOffsetOk = false;
            bool uncertaintyOk = uncertaintyText.isEmpty();
            const double frequencyMhz = frequencyText.toDouble(&frequencyOk);
            const double frequencyOffsetHz = frequencyOffsetText.toDouble(&frequencyOffsetOk);
            const double amplitudeOffsetDb = amplitudeOffsetText.toDouble(&amplitudeOffsetOk);
            const double uncertaintyDb = uncertaintyText.isEmpty() ? 0.0 : uncertaintyText.toDouble(&uncertaintyOk);
            if (!frequencyOk || !frequencyOffsetOk || !amplitudeOffsetOk ||
                !std::isfinite(frequencyMhz) || frequencyMhz < 0.0 || frequencyMhz > 100000.0 ||
                !std::isfinite(frequencyOffsetHz) || std::abs(frequencyOffsetHz) > 10000000.0 ||
                !std::isfinite(amplitudeOffsetDb) || std::abs(amplitudeOffsetDb) > 200.0 ||
                !uncertaintyOk || !std::isfinite(uncertaintyDb) || uncertaintyDb < 0.0 || uncertaintyDb > 100.0) {
                if (error) {
                    *error = uiText(QStringLiteral("bad_calibration_row"),
                                    QStringLiteral("Bad calibration point at row %1"))
                                 .arg(row + 1);
                }
                return false;
            }
            ReceiverCalibrationPoint point;
            point.frequencyHz = frequencyMhz * 1000000.0;
            point.frequencyOffsetHz = frequencyOffsetHz;
            point.amplitudeOffsetDb = amplitudeOffsetDb;
            point.uncertaintyDb = uncertaintyDb;
            point.note = note.left(256);
            if (table->item(row, 5)) point.conditions = ReceiverCalibrationContext::fromJson(table->item(row, 5)->data(Qt::UserRole).toJsonObject());
            next.append(point);
        }
        std::sort(next.begin(), next.end(),
                  [](const ReceiverCalibrationPoint &a, const ReceiverCalibrationPoint &b) {
                      const auto aKey = QJsonDocument(a.conditions.toJson()).toJson(QJsonDocument::Compact);
                      const auto bKey = QJsonDocument(b.conditions.toJson()).toJson(QJsonDocument::Compact);
                      if (aKey != bKey) return aKey < bKey;
                      return a.frequencyHz < b.frequencyHz;
                  });
        for (int i = 1; i < next.size(); ++i) {
            if (std::abs(next.at(i).frequencyHz - next.at(i - 1).frequencyHz) < 0.001 &&
                next.at(i).conditions.toJson() == next.at(i - 1).conditions.toJson()) {
                if (error) {
                    *error = uiText(QStringLiteral("duplicate_calibration_frequency"),
                                    QStringLiteral("Calibration frequencies must be unique."));
                }
                return false;
            }
        }
        target = next;
        return true;
    };

    connect(buttonBox, &QDialogButtonBox::accepted, &dialog, [&]() {
        QString error;
        QMap<QString, double> nextCenter = centerFrequencyPresets;
        QMap<QString, double> nextListening = listeningFrequencyPresets;
        QMap<QString, double> nextBandwidth = bandwidthValuePresets;
        QMap<QString, QString> nextAgile = agileScanPresets;
        QMap<QString, QString> nextStandardScan = standardScanPresets;
        QMap<QString, QString> nextListeningScan = listeningScanPresets;
        QVector<GraphBandMarker> nextBandMarkers = bandMarkers;
        QVector<qth::UserMarker> nextQthMarkers = qthUserMarkers;
        QVector<ReceiverCalibrationPoint> nextCalibrationPoints = receiverCalibrationTable.points();
        if (!readNumericTable(centerTable,
                              uiText(QStringLiteral("preset_tab_center"), QStringLiteral("Center")),
                              nextCenter,
                              &error) ||
            !readNumericTable(listeningTable,
                              uiText(QStringLiteral("preset_tab_listen"), QStringLiteral("Listen")),
                              nextListening,
                              &error) ||
            !readNumericTable(bandwidthTable,
                              uiText(QStringLiteral("preset_tab_audio_bw"), QStringLiteral("Audio BW")),
                              nextBandwidth,
                              &error) ||
            !readAgileTable(agileTable, nextAgile, &error) ||
            !readStandardScanTable(standardScanTable, nextStandardScan, &error) ||
            !readListeningScanTable(listeningScanTable, nextListeningScan, &error) ||
            !readBandMarkerTable(bandMarkerTable, nextBandMarkers, &error) ||
            !readQthMarkerTable(qthMarkerTable, nextQthMarkers, &error) ||
            !readCalibrationTable(calibrationTable, nextCalibrationPoints, &error)) {
            QMessageBox::warning(&dialog,
                                  uiText(QStringLiteral("preset_manager"), QStringLiteral("Preset Manager")),
                                  error);
            return;
        }
        centerFrequencyPresets = nextCenter;
        listeningFrequencyPresets = nextListening;
        bandwidthValuePresets = nextBandwidth;
        agileScanPresets = nextAgile;
        standardScanPresets = nextStandardScan;
        listeningScanPresets = nextListeningScan;
        centerFrequencyPresetOrder =
            normalizedPresetOrder(tableNameOrder(centerTable), centerFrequencyPresets);
        listeningFrequencyPresetOrder =
            normalizedPresetOrder(tableNameOrder(listeningTable), listeningFrequencyPresets);
        bandwidthPresetOrder =
            normalizedPresetOrder(tableNameOrder(bandwidthTable), bandwidthValuePresets);
        agileScanPresetOrder =
            normalizedPresetOrder(tableNameOrder(agileTable), agileScanPresets);
        standardScanPresetOrder =
            normalizedPresetOrder(tableNameOrder(standardScanTable), standardScanPresets);
        listeningScanPresetOrder =
            normalizedPresetOrder(tableNameOrder(listeningScanTable), listeningScanPresets);
        bandMarkers = nextBandMarkers;
        qthUserMarkers = nextQthMarkers;
        calibrationTableEnabled = calibrationEnabledCheckbox && calibrationEnabledCheckbox->isChecked();
        receiverCalibrationTable.setPoints(nextCalibrationPoints);
        bandMarkersCustomized = true;
        updateFrequencyPresetControls();
        updateGraphBandMarkers();
        updateQthControls();
        applyReceiverCalibrationLive();
        savePersistentSettings();
        dialog.accept();
    });
    connect(buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    dialog.exec();
}

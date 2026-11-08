#include "main.h"

#include "gnssqthhelpers.h"
#include "gnssserialutils.h"
#include "qthlocator.h"
#include "qthmapwidget.h"

#include <QDateTime>
#include <QHeaderView>
#include <QPainter>
#include <QPixmap>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTableWidgetItem>

#include <algorithm>
#include <cmath>

void YourClassName::resetGnssNmeaSatellites() {
    gnssNmeaSatellites.clear();
    gnssNmeaSatelliteEnabled.clear();
    gnssAcquisitionPlotShowsAcquisition = false;
    gnssSatelliteTableDirty = true;
    updateGnssSatelliteView(true);
}

bool YourClassName::isGnssNmeaSystemEnabled(const QString &system) const {
    const QString normalized = system.trimmed().toUpper();
    if (normalized == QStringLiteral("GPS")) {
        return gnssUseGps;
    }
    if (normalized == QStringLiteral("GLONASS")) {
        return gnssUseGlonass;
    }
    if (normalized == QStringLiteral("GALILEO")) {
        return gnssUseGalileo;
    }
    if (normalized == QStringLiteral("BEIDOU")) {
        return gnssUseBeidou;
    }
    if (normalized == QStringLiteral("QZSS")) {
        return gnssUseQzss;
    }
    if (normalized == QStringLiteral("SBAS")) {
        return gnssUseSbas;
    }
    if (normalized == QStringLiteral("MIXED")) {
        return gnssUseGps || gnssUseGlonass || gnssUseGalileo || gnssUseBeidou || gnssUseQzss || gnssUseSbas;
    }
    return gnssUseOther;
}

bool YourClassName::isGnssNmeaTalkerEnabled(const QString &talker) const {
    return isGnssNmeaSystemEnabled(gnssSystemForTalker(talker));
}

void YourClassName::ensureGnssSatelliteTableRows(int requiredRows) {
    if (!gnssSatelliteTable) {
        return;
    }
    requiredRows = (std::clamp)(requiredRows, 0, GNSS_SATELLITE_TABLE_MAX_ROWS);
    if (requiredRows <= gnssSatelliteTable->rowCount()) {
        return;
    }

    int newRowCount = gnssSatelliteTable->rowCount();
    while (newRowCount < requiredRows && newRowCount < GNSS_SATELLITE_TABLE_MAX_ROWS) {
        newRowCount += GNSS_SATELLITE_TABLE_GROW_STEP;
    }
    newRowCount = (std::min)(newRowCount, GNSS_SATELLITE_TABLE_MAX_ROWS);
    const int oldRowCount = gnssSatelliteTable->rowCount();
    gnssSatelliteTable->setRowCount(newRowCount);
    for (int row = oldRowCount; row < newRowCount; ++row) {
        for (int column = 0; column < gnssSatelliteTable->columnCount(); ++column) {
            auto *item = new QTableWidgetItem(column == 0 ? QString() : QStringLiteral("-"));
            item->setTextAlignment(Qt::AlignCenter);
            item->setFlags(Qt::ItemIsEnabled);
            if (column == 0) {
                item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
                item->setCheckState(Qt::Unchecked);
            }
            gnssSatelliteTable->setItem(row, column, item);
        }
        gnssSatelliteTable->setRowHidden(row, true);
    }
}

bool YourClassName::isGnssSatelliteEnabled(const QString &key) const {
    return gnssNmeaSatelliteEnabled.value(key, !gnssDisabledSatelliteKeys.contains(key));
}

bool YourClassName::hasEnabledGnssNmeaFixSatellite() const {
    bool sawUsedSatellite = false;
    for (const GnssNmeaSatellite &satellite : gnssNmeaSatellites) {
        if (!satellite.usedInFix) {
            continue;
        }
        sawUsedSatellite = true;
        if (isGnssNmeaSystemEnabled(satellite.system) &&
            isGnssSatelliteEnabled(satellite.key)) {
            return true;
        }
    }
    return !sawUsedSatellite;
}

void YourClassName::sortGnssSatelliteRows(QVector<GnssNmeaSatellite> &satellites) const {
    auto compareText = [](const QString &left, const QString &right) {
        return QString::localeAwareCompare(left, right) < 0;
    };

    std::sort(satellites.begin(), satellites.end(), [this, compareText](const GnssNmeaSatellite &left,
                                                                         const GnssNmeaSatellite &right) {
        int result = 0;
        switch (gnssSatelliteSortColumn) {
        case 0:
            result = static_cast<int>(gnssNmeaSatelliteEnabled.value(left.key, true)) -
                     static_cast<int>(gnssNmeaSatelliteEnabled.value(right.key, true));
            break;
        case 1:
            result = compareText(left.source, right.source) ? -1 : (compareText(right.source, left.source) ? 1 : 0);
            break;
        case 2:
            result = compareText(left.system, right.system) ? -1 : (compareText(right.system, left.system) ? 1 : 0);
            break;
        case 3:
            result = left.prn - right.prn;
            break;
        case 4:
            result = left.elevationDeg - right.elevationDeg;
            break;
        case 5:
            result = left.azimuthDeg - right.azimuthDeg;
            break;
        case 6:
            result = left.cn0DbHz - right.cn0DbHz;
            break;
        case 7:
            result = static_cast<int>(left.usedInFix) - static_cast<int>(right.usedInFix);
            break;
        case 8:
            result = left.lastSeenMs < right.lastSeenMs ? -1 : (left.lastSeenMs > right.lastSeenMs ? 1 : 0);
            break;
        case 9:
            result = left.cn0DbHz - right.cn0DbHz;
            break;
        default:
            result = 0;
            break;
        }

        if (result == 0) {
            if (left.usedInFix != right.usedInFix) {
                result = static_cast<int>(left.usedInFix) - static_cast<int>(right.usedInFix);
            } else if (left.system != right.system) {
                result = compareText(left.system, right.system) ? -1 : 1;
            } else {
                result = left.prn - right.prn;
            }
        }

        return gnssSatelliteSortAscending ? result < 0 : result > 0;
    });
}

void YourClassName::setGnssSatelliteRowsEnabled(bool enabled) {
    bool changed = false;
    for (const GnssNmeaSatellite &satellite : gnssNmeaSatellites) {
        if (!isGnssNmeaSystemEnabled(satellite.system)) {
            continue;
        }
        if (isGnssSatelliteEnabled(satellite.key) == enabled) {
            continue;
        }
        gnssNmeaSatelliteEnabled.insert(satellite.key, enabled);
        if (enabled) {
            gnssDisabledSatelliteKeys.remove(satellite.key);
        } else {
            gnssDisabledSatelliteKeys.insert(satellite.key);
        }
        changed = true;
    }
    if (changed) {
        gnssSatelliteTableDirty = true;
        updateGnssSatelliteView(true);
        savePersistentSettings();
    }
}

void YourClassName::updateGnssNmeaSatellitesFromSentence(const QString &line) {
    const QStringList fields = nmeaBodyFields(line);
    if (fields.isEmpty()) {
        return;
    }
    const QString type = fields.at(0).trimmed().toUpper();
    if (type.size() < 5) {
        return;
    }
    const QString talker = type.left(type.size() - 3);
    const QString sentence = type.right(3);
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();

    if (sentence == QStringLiteral("GSV")) {
        for (int index = 4; index + 3 < fields.size(); index += 4) {
            const int prn = parseNmeaIntField(fields, index);
            if (prn <= 0) {
                continue;
            }
            const QString key = gnssSatelliteKey(talker, prn);
            GnssNmeaSatellite satellite = gnssNmeaSatellites.value(key);
            satellite.key = key;
            satellite.source = QStringLiteral("NMEA");
            satellite.talker = talker;
            satellite.system = gnssSystemForTalker(talker, prn);
            satellite.prn = prn;
            satellite.elevationDeg = parseNmeaIntField(fields, index + 1, satellite.elevationDeg);
            satellite.azimuthDeg = parseNmeaIntField(fields, index + 2, satellite.azimuthDeg);
            satellite.cn0DbHz = parseNmeaIntField(fields, index + 3, satellite.cn0DbHz);
            satellite.lastSeenMs = nowMs;
            gnssNmeaSatellites.insert(key, satellite);
            if (!gnssNmeaSatelliteEnabled.contains(key)) {
                gnssNmeaSatelliteEnabled.insert(key, !gnssDisabledSatelliteKeys.contains(key));
            }
        }
    } else if (sentence == QStringLiteral("GSA")) {
        const bool mixedTalker = talker == QStringLiteral("GN");
        for (auto it = gnssNmeaSatellites.begin(); it != gnssNmeaSatellites.end(); ++it) {
            if (mixedTalker || it->talker == talker) {
                it->usedInFix = false;
            }
        }
        for (int index = 3; index <= 14 && index < fields.size(); ++index) {
            const int prn = parseNmeaIntField(fields, index);
            if (prn <= 0) {
                continue;
            }
            const QString key = gnssSatelliteKey(talker, prn);
            auto directIt = gnssNmeaSatellites.find(key);
            if (directIt != gnssNmeaSatellites.end()) {
                directIt->usedInFix = true;
                directIt->lastSeenMs = nowMs;
                continue;
            }
            for (auto it = gnssNmeaSatellites.begin(); it != gnssNmeaSatellites.end(); ++it) {
                if (it->prn == prn && (mixedTalker || it->talker == talker)) {
                    it->usedInFix = true;
                    it->lastSeenMs = nowMs;
                }
            }
        }
    } else {
        return;
    }

    const qint64 staleBeforeMs = nowMs - 30000;
    for (auto it = gnssNmeaSatellites.begin(); it != gnssNmeaSatellites.end();) {
        if (it->lastSeenMs > 0 && it->lastSeenMs < staleBeforeMs) {
            gnssNmeaSatelliteEnabled.remove(it.key());
            it = gnssNmeaSatellites.erase(it);
        } else {
            ++it;
        }
    }
    updateGnssSatelliteView();
}

void YourClassName::updateGnssSatelliteView(bool forceTableRefresh) {
    if (gnssUseGpsCheckbox) {
        QSignalBlocker blocker(gnssUseGpsCheckbox);
        gnssUseGpsCheckbox->setChecked(gnssUseGps);
    }
    if (gnssUseGlonassCheckbox) {
        QSignalBlocker blocker(gnssUseGlonassCheckbox);
        gnssUseGlonassCheckbox->setChecked(gnssUseGlonass);
    }
    if (gnssUseGalileoCheckbox) {
        QSignalBlocker blocker(gnssUseGalileoCheckbox);
        gnssUseGalileoCheckbox->setChecked(gnssUseGalileo);
    }
    if (gnssUseBeidouCheckbox) {
        QSignalBlocker blocker(gnssUseBeidouCheckbox);
        gnssUseBeidouCheckbox->setChecked(gnssUseBeidou);
    }
    if (gnssUseQzssCheckbox) {
        QSignalBlocker blocker(gnssUseQzssCheckbox);
        gnssUseQzssCheckbox->setChecked(gnssUseQzss);
    }
    if (gnssUseSbasCheckbox) {
        QSignalBlocker blocker(gnssUseSbasCheckbox);
        gnssUseSbasCheckbox->setChecked(gnssUseSbas);
    }
    if (gnssUseOtherCheckbox) {
        QSignalBlocker blocker(gnssUseOtherCheckbox);
        gnssUseOtherCheckbox->setChecked(gnssUseOther);
    }

    QVector<GnssNmeaSatellite> satellites;
    satellites.reserve(gnssNmeaSatellites.size());
    for (const GnssNmeaSatellite &satellite : gnssNmeaSatellites) {
        if (isGnssNmeaSystemEnabled(satellite.system)) {
            satellites.append(satellite);
        }
    }
    sortGnssSatelliteRows(satellites);

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    int enabledCount = 0;
    int usedCount = 0;
    int cn0Sum = 0;
    int cn0Count = 0;
    int ubxRows = 0;
    int nmeaRows = 0;
    int ignoredCount = 0;
    for (const GnssNmeaSatellite &satellite : satellites) {
        if (satellite.source == QStringLiteral("UBX")) {
            ++ubxRows;
        } else if (satellite.source == QStringLiteral("NMEA")) {
            ++nmeaRows;
        }
        const bool rowEnabled = isGnssSatelliteEnabled(satellite.key);
        if (rowEnabled) {
            ++enabledCount;
            if (satellite.cn0DbHz >= 0) {
                cn0Sum += satellite.cn0DbHz;
                ++cn0Count;
            }
            if (satellite.usedInFix) {
                ++usedCount;
            }
        } else {
            ++ignoredCount;
        }
    }
    const QString averageCn0Text = cn0Count > 0
                                       ? QStringLiteral("%1").arg(static_cast<double>(cn0Sum) / cn0Count, 0, 'f', 1)
                                       : QStringLiteral("-");
    const QString policyText = normalizedGnssPositionPolicy(gnssPositionPolicy).replace(QLatin1Char('_'),
                                                                                         QLatin1Char(' '));

    auto fixText = [this](int fixQuality, int fixMode) {
        if (fixMode > 0) {
            return QStringLiteral("%1D").arg(fixMode);
        }
        if (fixQuality > 0) {
            return uiText(QStringLiteral("fix_number"), QStringLiteral("fix %1")).arg(fixQuality);
        }
        return uiText(QStringLiteral("no_fix"), QStringLiteral("no fix"));
    };

    auto qualityText = [this](const GnssNmeaSatellite &satellite) {
        if (satellite.cn0DbHz < 0) {
            return satellite.usedInFix ? uiText(QStringLiteral("used"), QStringLiteral("used")) : QStringLiteral("-");
        }
        if (satellite.cn0DbHz >= 38) {
            return satellite.usedInFix ? uiText(QStringLiteral("used_strong"), QStringLiteral("used strong"))
                                       : uiText(QStringLiteral("strong"), QStringLiteral("strong"));
        }
        if (satellite.cn0DbHz >= 28) {
            return satellite.usedInFix ? uiText(QStringLiteral("used_good"), QStringLiteral("used good"))
                                       : uiText(QStringLiteral("good"), QStringLiteral("good"));
        }
        if (satellite.cn0DbHz >= 18) {
            return satellite.usedInFix ? uiText(QStringLiteral("used_weak"), QStringLiteral("used weak"))
                                       : uiText(QStringLiteral("weak"), QStringLiteral("weak"));
        }
        return satellite.usedInFix ? uiText(QStringLiteral("used_poor"), QStringLiteral("used poor"))
                                   : uiText(QStringLiteral("poor"), QStringLiteral("poor"));
    };

    const bool satelliteDialogVisible = gnssSatelliteDialog && gnssSatelliteDialog->isVisible();
    constexpr qint64 GNSS_SATELLITE_LIVE_REFRESH_MS = 500;
    const bool refreshTableDue = !gnssSatelliteTableUpdateTimer.isValid() ||
                                 gnssSatelliteTableUpdateTimer.elapsed() >= GNSS_SATELLITE_LIVE_REFRESH_MS;
    const bool refreshLight = forceTableRefresh ||
                              !gnssSatelliteOverlayUpdateTimer.isValid() ||
                              gnssSatelliteOverlayUpdateTimer.elapsed() >= GNSS_SATELLITE_LIVE_REFRESH_MS;

    if (gnssSatelliteStatusLabel && satelliteDialogVisible && refreshLight) {
        auto chip = [](const QString &title, const QString &value, const QString &accent) {
            return QStringLiteral(
                       "<td style='padding:5px 8px;border:1px solid #3b4652;background:#202832;'>"
                       "<div style='font-size:8pt;color:#9aa7b5;'>%1</div>"
                       "<div style='font-size:10pt;font-weight:600;color:%3;'>%2</div>"
                       "</td>")
                .arg(title.toHtmlEscaped(), value.toHtmlEscaped(), accent);
        };
        QStringList chips;
        chips << chip(uiText(QStringLiteral("fix"), QStringLiteral("Fix")), fixText(gnssSerialFixQuality, gnssSerialFixMode),
                      gnssSerialFixQuality > 0 ? QStringLiteral("#8ee38e") : QStringLiteral("#f08a7a"));
        chips << chip(uiText(QStringLiteral("policy"), QStringLiteral("Policy")), policyText, QStringLiteral("#d9e2ec"));
        chips << chip(uiText(QStringLiteral("satellites"), QStringLiteral("Satellites")),
                      uiText(QStringLiteral("gnss_satellite_count_summary"), QStringLiteral("%1 used / %2 visible"))
                          .arg(gnssSerialSatellitesUsed >= 0 ? gnssSerialSatellitesUsed : usedCount)
                          .arg(satellites.size()),
                      QStringLiteral("#d9e2ec"));
        chips << chip(QStringLiteral("DOP"),
                      QStringLiteral("H %1  V %2  P %3")
                          .arg(std::isfinite(gnssSerialHdop) ? QString::number(gnssSerialHdop, 'f', 2) : QStringLiteral("-"))
                          .arg(std::isfinite(gnssSerialVdop) ? QString::number(gnssSerialVdop, 'f', 2) : QStringLiteral("-"))
                          .arg(std::isfinite(gnssSerialPdop) ? QString::number(gnssSerialPdop, 'f', 2) : QStringLiteral("-")),
                      QStringLiteral("#d9e2ec"));
        chips << chip(uiText(QStringLiteral("signal"), QStringLiteral("Signal")),
                      uiText(QStringLiteral("gnss_signal_summary"), QStringLiteral("%1 avg C/N0, %2 enabled, %3 ignored"))
                          .arg(averageCn0Text)
                          .arg(enabledCount)
                          .arg(ignoredCount),
                      QStringLiteral("#d9e2ec"));
        chips << chip(uiText(QStringLiteral("source"), QStringLiteral("Source")),
                      QStringLiteral("UBX %1 / NMEA %2").arg(ubxRows).arg(nmeaRows),
                      QStringLiteral("#d9e2ec"));
        chips << chip(uiText(QStringLiteral("position"), QStringLiteral("Position")),
                      QStringLiteral("%1 m").arg(std::isfinite(gnssSerialAltitudeM)
                                                     ? QString::number(gnssSerialAltitudeM, 'f', 1)
                                                     : QStringLiteral("-")),
                      QStringLiteral("#d9e2ec"));
        chips << chip(uiText(QStringLiteral("motion"), QStringLiteral("Motion")),
                      QStringLiteral("%1 km/h  %2 deg")
                          .arg(std::isfinite(gnssSerialSpeedKmh) ? QString::number(gnssSerialSpeedKmh, 'f', 1) : QStringLiteral("-"))
                          .arg(std::isfinite(gnssSerialCourseDeg) ? QString::number(gnssSerialCourseDeg, 'f', 1) : QStringLiteral("-")),
                      QStringLiteral("#d9e2ec"));
        chips << chip(uiText(QStringLiteral("time"), QStringLiteral("Time")), formattedGnssUtc(), QStringLiteral("#d9e2ec"));
        gnssSatelliteStatusLabel->setText(QStringLiteral(
            "<table cellspacing='3' cellpadding='0'><tr>%1</tr></table>").arg(chips.join(QString())));
    }

    bool refreshTable = forceTableRefresh;
    if (satelliteDialogVisible && !refreshTable) {
        refreshTable = gnssSatelliteTableDirty && refreshTableDue;
    }
    if (!satelliteDialogVisible && !forceTableRefresh) {
        refreshTable = false;
        gnssSatelliteTableDirty = true;
    }
    if (!gnssSatelliteTableVisible) {
        refreshTable = false;
        if (gnssSatelliteTable && gnssSatelliteTable->isVisible()) {
            gnssSatelliteTable->setVisible(false);
        }
    }

    if (gnssSatelliteTable && refreshTable) {
        if (!gnssSatelliteTable->isVisible()) {
            gnssSatelliteTable->setVisible(true);
        }
        QSignalBlocker blocker(gnssSatelliteTable);
        gnssSatelliteTable->setUpdatesEnabled(false);
        ensureGnssSatelliteTableRows(satellites.size());
        const int tableRows = gnssSatelliteTable->rowCount();
        const int rowsToShow = (std::min)(satellites.size(), tableRows);
        auto setText = [this](int row, int column, const QString &text) {
            if (QTableWidgetItem *item = gnssSatelliteTable->item(row, column)) {
                if (item->text() != text) {
                    item->setText(text);
                }
            }
        };
        for (int row = 0; row < tableRows; ++row) {
            if (row >= rowsToShow) {
                gnssSatelliteTable->setRowHidden(row, true);
                continue;
            }
            const GnssNmeaSatellite &satellite = satellites.at(row);
            gnssSatelliteTable->setRowHidden(row, false);
            const bool rowEnabled = isGnssSatelliteEnabled(satellite.key);
            if (QTableWidgetItem *useItem = gnssSatelliteTable->item(row, 0)) {
                useItem->setData(Qt::UserRole, satellite.key);
                const Qt::CheckState state = rowEnabled ? Qt::Checked : Qt::Unchecked;
                if (useItem->checkState() != state) {
                    useItem->setCheckState(state);
                }
            }
            setText(row, 1, satellite.source.isEmpty() ? QStringLiteral("-") : satellite.source);
            setText(row, 2, satellite.system);
            setText(row, 3, satellite.prn >= 0 ? QString::number(satellite.prn) : QStringLiteral("-"));
            setText(row, 4, satellite.elevationDeg >= 0 ? QString::number(satellite.elevationDeg) : QStringLiteral("-"));
            setText(row, 5, satellite.azimuthDeg >= 0 ? QString::number(satellite.azimuthDeg) : QStringLiteral("-"));
            setText(row, 6, satellite.cn0DbHz >= 0 ? QString::number(satellite.cn0DbHz) : QStringLiteral("-"));
            setText(row, 7, satellite.usedInFix ? QStringLiteral("yes") : QStringLiteral("-"));
            const qint64 ageMs = satellite.lastSeenMs > 0 ? nowMs - satellite.lastSeenMs : -1;
            setText(row, 8, ageMs >= 0 ? QStringLiteral("%1s").arg(ageMs / 1000) : QStringLiteral("-"));
            setText(row, 9, qualityText(satellite));
        }
        gnssSatelliteTable->setUpdatesEnabled(true);
        gnssSatelliteTableUpdateTimer.restart();
        gnssSatelliteTableDirty = false;
    } else if (gnssSatelliteTable) {
        gnssSatelliteTableDirty = true;
    }
    if (refreshLight) {
        if (satelliteDialogVisible) {
            updateGnssNmeaSkyPlot();
        }
        updateQthMapSatelliteOverlay();
        gnssSatelliteOverlayUpdateTimer.restart();
    }

    if (gnssSerialStatusLabel && (gnssSerialPort && gnssSerialPort->isOpen())) {
        QStringList parts;
        parts << QStringLiteral("%1 sentences").arg(gnssSerialSentenceCount);
        if (gnssSerialUbxFrameCount > 0) {
            parts << QStringLiteral("%1 UBX").arg(gnssSerialUbxFrameCount);
        }
        if (gnssSerialFixQuality >= 0) {
            parts << QStringLiteral("fix %1").arg(gnssSerialFixQuality);
        }
        if (gnssSerialSatellitesUsed >= 0) {
            parts << QStringLiteral("used %1").arg(gnssSerialSatellitesUsed);
        }
        if (std::isfinite(gnssSerialHdop)) {
            parts << QStringLiteral("HDOP %1").arg(gnssSerialHdop, 0, 'f', 1);
        }
        if (std::isfinite(gnssSerialPdop)) {
            parts << QStringLiteral("PDOP %1").arg(gnssSerialPdop, 0, 'f', 1);
        }
        if (std::isfinite(gnssSerialVdop)) {
            parts << QStringLiteral("VDOP %1").arg(gnssSerialVdop, 0, 'f', 1);
        }
        if (std::isfinite(gnssSerialAltitudeM)) {
            parts << QStringLiteral("alt %1 m").arg(gnssSerialAltitudeM, 0, 'f', 1);
        }
        if (std::isfinite(gnssSerialGeoidSeparationM)) {
            parts << QStringLiteral("geoid %1 m").arg(gnssSerialGeoidSeparationM, 0, 'f', 1);
        }
        if (std::isfinite(gnssSerialSpeedKmh)) {
            parts << QStringLiteral("speed %1 km/h").arg(gnssSerialSpeedKmh, 0, 'f', 1);
        }
        if (std::isfinite(gnssSerialCourseDeg)) {
            parts << QStringLiteral("course %1 deg").arg(gnssSerialCourseDeg, 0, 'f', 1);
        }
        if (!satellites.isEmpty()) {
            const QString cn0Text = cn0Count > 0
                                        ? QStringLiteral(", avg C/N0 %1").arg(static_cast<double>(cn0Sum) / cn0Count, 0, 'f', 1)
                                        : QString();
            parts << QStringLiteral("filtered sats %1/%2, fix rows %3%4")
                         .arg(enabledCount)
                         .arg(satellites.size())
                         .arg(usedCount)
                         .arg(cn0Text);
        }
        if (!gnssSerialUtc.isEmpty()) {
            parts << QStringLiteral("time %1").arg(formattedGnssUtc());
        }
        gnssSerialStatusLabel->setText(uiText(QStringLiteral("gnss_serial_live_summary"),
                                              QStringLiteral("NMEA serial: %1"))
                                           .arg(parts.join(QStringLiteral(", "))));
    }
}

void YourClassName::updateGnssNmeaSkyPlot() {
    if (!gnssSatelliteSkyLabel) {
        return;
    }

    QVector<GnssNmeaSatellite> satellites;
    satellites.reserve(gnssNmeaSatellites.size());
    for (const GnssNmeaSatellite &satellite : gnssNmeaSatellites) {
        if (isGnssNmeaSystemEnabled(satellite.system)) {
            satellites.append(satellite);
        }
    }
    std::sort(satellites.begin(), satellites.end(), [](const GnssNmeaSatellite &left,
                                                       const GnssNmeaSatellite &right) {
        if (left.usedInFix != right.usedInFix) {
            return left.usedInFix > right.usedInFix;
        }
        if (left.cn0DbHz != right.cn0DbHz) {
            return left.cn0DbHz > right.cn0DbHz;
        }
        if (left.system != right.system) {
            return left.system < right.system;
        }
        return left.prn < right.prn;
    });

    const int width = (std::max)(520, gnssSatelliteSkyLabel->width());
    const int height = (std::max)(240, gnssSatelliteSkyLabel->height());
    QPixmap pixmap(width, height);
    pixmap.fill(QColor(18, 22, 27));
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setFont(QFont(QStringLiteral("Segoe UI"), 8));
    painter.setPen(QColor(220, 226, 232));
    painter.drawText(12, 16, uiText(QStringLiteral("gnss_sky_title"),
                                    QStringLiteral("GNSS sky view / C/N0")));

    const int skySize = (std::min)(height - 42, width / 2 - 28);
    const QPoint center(28 + skySize / 2, 30 + skySize / 2);
    const int radius = skySize / 2;
    painter.setPen(QPen(QColor(80, 92, 104), 1));
    painter.drawEllipse(center, radius, radius);
    painter.drawEllipse(center, radius * 2 / 3, radius * 2 / 3);
    painter.drawEllipse(center, radius / 3, radius / 3);
    painter.drawLine(center.x() - radius, center.y(), center.x() + radius, center.y());
    painter.drawLine(center.x(), center.y() - radius, center.x(), center.y() + radius);
    painter.setPen(QColor(140, 150, 160));
    painter.drawText(center.x() - 4, center.y() - radius - 4, QStringLiteral("N"));
    painter.drawText(center.x() + radius + 4, center.y() + 4, QStringLiteral("E"));
    painter.drawText(center.x() - 4, center.y() + radius + 12, QStringLiteral("S"));
    painter.drawText(center.x() - radius - 12, center.y() + 4, QStringLiteral("W"));

    auto colorForCn0 = [](int cn0) {
        if (cn0 < 0) {
            return QColor(125, 135, 145);
        }
        if (cn0 < 20) {
            return QColor(230, 95, 80);
        }
        if (cn0 < 35) {
            return QColor(235, 190, 70);
        }
        return QColor(105, 215, 125);
    };
    auto systemShort = [](const QString &system) {
        if (system == QStringLiteral("GLONASS")) return QStringLiteral("R");
        if (system == QStringLiteral("Galileo")) return QStringLiteral("E");
        if (system == QStringLiteral("BeiDou")) return QStringLiteral("C");
        if (system == QStringLiteral("QZSS")) return QStringLiteral("Q");
        if (system == QStringLiteral("SBAS")) return QStringLiteral("S");
        if (system == QStringLiteral("GPS")) return QStringLiteral("G");
        return QStringLiteral("?");
    };

    for (const GnssNmeaSatellite &satellite : satellites) {
        if (satellite.elevationDeg < 0 || satellite.azimuthDeg < 0) {
            continue;
        }
        const double az = qDegreesToRadians(static_cast<double>(satellite.azimuthDeg));
        const double normalizedRadius =
            (std::clamp)(90.0 - static_cast<double>(satellite.elevationDeg), 0.0, 90.0) / 90.0;
        const int r = static_cast<int>(normalizedRadius * radius);
        const QPoint point(center.x() + static_cast<int>(std::sin(az) * r),
                           center.y() - static_cast<int>(std::cos(az) * r));
        QColor color = colorForCn0(satellite.cn0DbHz);
        const bool rowEnabled = isGnssSatelliteEnabled(satellite.key);
        if (!rowEnabled) {
            color = color.darker(220);
        }
        painter.setBrush(color);
        painter.setPen(QPen(satellite.usedInFix ? QColor(255, 255, 255) : QColor(20, 24, 28),
                            satellite.usedInFix ? 2 : 1));
        painter.drawEllipse(point, 7, 7);
        painter.setPen(rowEnabled ? QColor(235, 240, 245) : QColor(135, 145, 155));
        painter.drawText(point + QPoint(9, 4),
                         QStringLiteral("%1%2").arg(systemShort(satellite.system)).arg(satellite.prn));
    }

    const QRect barRect(width / 2 + 20, 32, width / 2 - 42, height - 54);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setPen(QColor(130, 142, 154));
    painter.drawRect(barRect);
    if (satellites.isEmpty()) {
        painter.setPen(QColor(150, 160, 170));
        painter.drawText(barRect.adjusted(8, 8, -8, -8),
                         Qt::AlignCenter,
                         uiText(QStringLiteral("gnss_no_live_satellites"),
                                QStringLiteral("No live GNSS satellites yet.")));
    } else {
        const int rows = (std::min)(satellites.size(), 14);
        const int rowHeight = (std::max)(12, barRect.height() / (std::max)(1, rows));
        for (int i = 0; i < rows; ++i) {
            const GnssNmeaSatellite &satellite = satellites.at(i);
            const int y = barRect.top() + i * rowHeight + 2;
            const int cn0 = satellite.cn0DbHz >= 0 ? satellite.cn0DbHz : 0;
            const int barWidth = static_cast<int>((std::clamp)(cn0, 0, 55) / 55.0 * (barRect.width() - 96));
            const bool rowEnabled = isGnssSatelliteEnabled(satellite.key);
            painter.setPen(rowEnabled ? QColor(205, 214, 222) : QColor(120, 130, 140));
            painter.drawText(barRect.left() + 6,
                             y + rowHeight - 4,
                             QStringLiteral("%1%2").arg(systemShort(satellite.system)).arg(satellite.prn, 2));
            QColor barColor = colorForCn0(satellite.cn0DbHz);
            if (!rowEnabled) {
                barColor = barColor.darker(220);
            }
            painter.fillRect(QRect(barRect.left() + 48, y + 2, barWidth, rowHeight - 5), barColor);
            painter.setPen(rowEnabled ? QColor(190, 200, 210) : QColor(115, 125, 135));
            painter.drawText(barRect.right() - 38,
                             y + rowHeight - 4,
                             satellite.cn0DbHz >= 0 ? QString::number(satellite.cn0DbHz) : QStringLiteral("-"));
        }
    }

    gnssSatelliteSkyLabel->setPixmap(pixmap);
}

void YourClassName::updateQthMapSatelliteOverlay() {
    if (!qthMapWidget) {
        return;
    }

    auto systemShort = [](const QString &system) {
        if (system == QStringLiteral("GLONASS")) return QStringLiteral("R");
        if (system == QStringLiteral("Galileo")) return QStringLiteral("E");
        if (system == QStringLiteral("BeiDou")) return QStringLiteral("C");
        if (system == QStringLiteral("QZSS")) return QStringLiteral("Q");
        if (system == QStringLiteral("SBAS")) return QStringLiteral("S");
        if (system == QStringLiteral("GPS")) return QStringLiteral("G");
        return QStringLiteral("?");
    };

    QVector<QthMapWidget::SatelliteMarker> markers;
    markers.reserve(gnssNmeaSatellites.size());
    for (const GnssNmeaSatellite &satellite : gnssNmeaSatellites) {
        if (!isGnssNmeaSystemEnabled(satellite.system) ||
            satellite.elevationDeg < 0 ||
            satellite.azimuthDeg < 0) {
            continue;
        }
        QthMapWidget::SatelliteMarker marker;
        marker.system = satellite.system;
        marker.label = QStringLiteral("%1%2").arg(systemShort(satellite.system)).arg(satellite.prn);
        marker.elevationDeg = satellite.elevationDeg;
        marker.azimuthDeg = satellite.azimuthDeg;
        marker.cn0DbHz = satellite.cn0DbHz;
        marker.usedInFix = satellite.usedInFix;
        marker.enabled = isGnssSatelliteEnabled(satellite.key);
        markers.append(marker);
    }
    qthMapWidget->setSatelliteMarkers(markers);
}

QString YourClassName::formattedGnssUtc() const {
    return formatGnssUtcForDisplay(gnssSerialUtc, gnssTimeZoneOffsetMinutes);
}
